/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022, eunomia-bpf org
 * All rights reserved.
 */
#ifndef _BPF_JIT_HELPER
#define _BPF_JIT_HELPER

#include "llvm_jit_context.h"
#include "llvm_bpf_jit.h"
#include "ebpf_inst.h"
#include <functional>
#include <llvm-14/llvm/IR/Constants.h>
#include <llvm-14/llvm/Support/Alignment.h>
#include <llvm-14/llvm/Support/AtomicOrdering.h>
#include <llvm-15/llvm/ADT/APInt.h>
#include <llvm-15/llvm/IR/BasicBlock.h>
#include <llvm-15/llvm/IR/DerivedTypes.h>
#include <llvm-15/llvm/Support/Debug.h>
#include <llvm-15/llvm/Support/Error.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/IRBuilder.h>
#include <map>
#include <tuple>
#include <utility>
#include <spdlog/spdlog.h>

static inline bool is_jmp(const ebpf_inst &insn)
{
	return (insn.code & 0x07) == EBPF_CLS_JMP ||
	       (insn.code & 0x07) == EBPF_CLS_JMP32;
}
/// Get the source representation of certain ALU operands
static llvm::Value *emitLoadALUSource(const ebpf_inst &inst, llvm::Value **regs,
				      llvm::IRBuilder<> &builder)
{
	int srcTy = inst.code & 0x08;
	int code = inst.code & 0xf0;
	llvm::Value *src_val;
	if ((inst.code & 0x07) == EBPF_CLS_ALU64) {
		if (srcTy == EBPF_SRC_IMM) {
			src_val =
				builder.getInt64((uint64_t)((int64_t)inst.imm));
		} else {
			src_val = builder.CreateLoad(builder.getInt64Ty(),
						     regs[inst.src_reg]);
		}
	} else {
		if (srcTy == EBPF_SRC_IMM) {
			src_val = builder.getInt32(inst.imm);
		} else {
			// Registers are 64bits, so we need to
			// truncate them
			src_val = builder.CreateTrunc(
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[inst.src_reg]),
				builder.getInt32Ty());
		}
	}
	return src_val;
}
static llvm::Value *emitLoadALUDest(const ebpf_inst &inst, llvm::Value **regs,
				    llvm::IRBuilder<> &builder,
				    bool dstAlways64)
{
	if (((inst.code & 0x07) == EBPF_CLS_ALU64) || dstAlways64) {
		return builder.CreateLoad(builder.getInt64Ty(),
					  regs[inst.dst_reg]);
	} else {
		return builder.CreateLoad(builder.getInt32Ty(),
					  regs[inst.dst_reg]);
	}
}

static void emitStoreALUResult(const ebpf_inst &inst, llvm::Value **regs,
			       llvm::IRBuilder<> &builder, llvm::Value *result)
{
	if ((inst.code & 0x07) == EBPF_CLS_ALU64) {
		builder.CreateStore(result, regs[inst.dst_reg]);
	} else {
		// For 32-bit ALU operations, clear the
		// upper 32bits of the 64-bit register
		builder.CreateStore(builder.CreateZExt(result,
						       builder.getInt64Ty()),
				    regs[inst.dst_reg]);
	}
}
static inline llvm::Expected<llvm::Value *>
emitALUEndianConversion(const ebpf_inst &inst, llvm::IRBuilder<> &builder,
			llvm::Value *dst_val)
{
	// TODO: Support 64bit conversion
	//  Convert to big endian
	if ((inst.code & 0x08) == 0x08) {
		// Split bytes of the dst register
		std::vector<llvm::Value *> bytes;
		if (inst.imm != 16 && inst.imm != 32 && inst.imm != 64) {
			return llvm::make_error<llvm::StringError>(
				"Unexpected endian size: " +
					std::to_string(inst.imm),
				llvm::inconvertibleErrorCode());
		}
		for (int i = 0; i < inst.imm; i += 8) {
			bytes.push_back(builder.CreateAnd(
				builder.CreateLShr(dst_val,
						   llvm::ConstantInt::get(
							   dst_val->getType(),
							   i)),
				llvm::ConstantInt::get(dst_val->getType(),
						       0xff)));
		}
		// Merge these bytes together, with
		// reversed order
		llvm::Value *last = nullptr;
		for (auto val : bytes) {
			if (last == nullptr) {
				last = val;
			} else {
				last = builder.CreateOr(
					builder.CreateShl(
						last,
						llvm::ConstantInt::get(
							last->getType(), 8)),
					val);
			}
		}
		return last;
	} else {
		// We haven't take cast to little endian
		// into consideration, because we only
		// like little-endian machines
		return dst_val;
	}
}

static void emitALUWithDstAndSrc(
	const ebpf_inst &inst, llvm::IRBuilder<> &builder, llvm::Value **regs,
	std::function<llvm::Value *(llvm::Value *, llvm::Value *)> func)
{
	using namespace llvm;
	Value *dst_val = emitLoadALUDest(inst, &regs[0], builder, false);
	Value *src_val = emitLoadALUSource(inst, &regs[0], builder);
	Value *result = func(dst_val, src_val);
	emitStoreALUResult(inst, regs, builder, result);
}

static llvm::Value *emitStoreLoadingSrc(const ebpf_inst &inst,
					llvm::IRBuilder<> &builder,
					llvm::Value **regs)
{
	if ((inst.code & 0x07) == EBPF_CLS_STX) {
		return builder.CreateLoad(builder.getInt64Ty(),
					  regs[inst.src_reg]);
	} else {
		return builder.getInt64(inst.imm);
	}
}

static void emitStoreWritingResult(const ebpf_inst &inst,
				   llvm::IRBuilder<> &builder,
				   llvm::Value **regs, llvm::Value *result)
{
	builder.CreateStore(
		result,
		builder.CreateGEP(builder.getInt8Ty(),
				  builder.CreateLoad(builder.getPtrTy(),
						     regs[inst.dst_reg]),
				  { builder.getInt64(inst.off) }));
}

static void emitStore(const ebpf_inst &inst, llvm::IRBuilder<> &builder,
		      llvm::Value **regs, llvm::IntegerType *destTy)
{
	using namespace llvm;
	Value *src = emitStoreLoadingSrc(inst, builder, &regs[0]);

	Value *result = builder.CreateTrunc(src, destTy);
	emitStoreWritingResult(inst, builder, &regs[0], result);
}

static std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
emitJmpLoadSrcAndDstAndZero(const ebpf_inst &inst, llvm::Value **regs,
			    llvm::IRBuilder<> &builder)
{
	int regSrc = (inst.code & 0x8) == 0x8;
	using namespace llvm;
	Value *src, *dst, *zero;
	if ((inst.code & 0x07) == 0x06) {
		// JMP32
		if (regSrc) {
			src = builder.CreateLoad(builder.getInt32Ty(),
						 regs[inst.src_reg]);
		} else {
			src = builder.getInt32(inst.imm);
		}
		dst = builder.CreateLoad(builder.getInt32Ty(),
					 regs[inst.dst_reg]);
		zero = builder.getInt32(0);
	} else {
		// JMP64
		if (regSrc) {
			src = builder.CreateLoad(builder.getInt64Ty(),
						 regs[inst.src_reg]);
		} else {
			src = builder.getInt64(inst.imm);
		}
		dst = builder.CreateLoad(builder.getInt64Ty(),
					 regs[inst.dst_reg]);
		zero = builder.getInt64(0);
	}
	return { src, dst, zero };
}

static inline std::string ext_func_sym(uint32_t idx)
{
	char buf[16];
	sprintf(buf, "ext_%04" PRIu32, idx);
	return buf;
}

static inline bool is_alu64(const ebpf_inst &insn)
{
	return (insn.code & 0x07) == EBPF_CLS_ALU64;
}

static llvm::Expected<llvm::BasicBlock *>
loadJmpDstBlock(uint16_t pc, const ebpf_inst &inst,
		const std::map<uint16_t, llvm::BasicBlock *> &instBlocks)
{
	spdlog::trace("pc {} request jump to {}", pc, pc + 1 + inst.off);
	uint16_t dstBlkId = pc + 1 + inst.off;
	if (auto itr = instBlocks.find(dstBlkId); itr != instBlocks.end()) {
		return itr->second;
	} else {
		return llvm::make_error<llvm::StringError>(
			"Instruction at pc=" + std::to_string(pc) +
				" is going to jump to an illegal position " +
				std::to_string(dstBlkId),
			llvm::inconvertibleErrorCode());
	}
}

static llvm::Expected<llvm::BasicBlock *>
loadCallDstBlock(uint16_t pc, const ebpf_inst &inst,
		 const std::map<uint16_t, llvm::BasicBlock *> &instBlocks)
{
	uint16_t dstBlkId = pc + 1 + inst.imm;
	if (auto itr = instBlocks.find(dstBlkId); itr != instBlocks.end()) {
		return itr->second;
	} else {
		return llvm::make_error<llvm::StringError>(
			"Instruction at pc=" + std::to_string(pc) +
				" is going to jump to an illegal position " +
				std::to_string(dstBlkId),
			llvm::inconvertibleErrorCode());
	}
}

static llvm::Expected<llvm::BasicBlock *>
loadJmpNextBlock(uint16_t pc, const ebpf_inst &inst,
		 const std::map<uint16_t, llvm::BasicBlock *> &instBlocks)
{
	uint16_t nextBlkId = pc + 1;
	if (auto itr = instBlocks.find(nextBlkId); itr != instBlocks.end()) {
		return itr->second;
	} else {
		return llvm::make_error<llvm::StringError>(
			"Instruction at pc=" + std::to_string(pc) +
				" is going to jump to an illegal position " +
				std::to_string(nextBlkId),
			llvm::inconvertibleErrorCode());
	}
}

static llvm::Expected<std::pair<llvm::BasicBlock *, llvm::BasicBlock *> >
localJmpDstAndNextBlk(uint16_t pc, const ebpf_inst &inst,
		      const std::map<uint16_t, llvm::BasicBlock *> &instBlocks)
{
	if (auto dst = loadJmpDstBlock(pc, inst, instBlocks); dst) {
		if (auto next = loadJmpNextBlock(pc, inst, instBlocks); next) {
			return std::make_pair(dst.get(), next.get());
		} else {
			return next.takeError();
		}
	} else {
		return dst.takeError();
	}
}

static llvm::Value *emitLDXLoadingAddr(llvm::IRBuilder<> &builder,
				       llvm::Value **regs,
				       const ebpf_inst &inst)
{
	// [rX + OFFSET]
	return builder.CreateGEP(builder.getInt8Ty(),
				 builder.CreateLoad(builder.getPtrTy(),
						    regs[inst.src_reg]),
				 { builder.getInt64(inst.off) });
}

static void emitLDXStoringResult(llvm::IRBuilder<> &builder, llvm::Value **regs,
				 const ebpf_inst &inst, llvm::Value *result)
{
	// Extend the loaded value to 64bits, then store it into
	// the register
	builder.CreateStore(builder.CreateZExt(result, builder.getInt64Ty()),
			    regs[inst.dst_reg]);
}

static void emitLoadX(llvm::IRBuilder<> &builder, llvm::Value **regs,
		      const ebpf_inst &inst, llvm::IntegerType *srcTy)
{
	using namespace llvm;
	Value *addr = emitLDXLoadingAddr(builder, &regs[0], inst);
	Value *result = builder.CreateLoad(srcTy, addr);
	emitLDXStoringResult(builder, &regs[0], inst, result);
}

static llvm::Expected<int> emitCondJmpWithDstAndSrc(
	llvm::IRBuilder<> &builder, uint16_t pc, const ebpf_inst &inst,
	const std::map<uint16_t, llvm::BasicBlock *> &instBlocks,
	llvm::Value **regs,
	std::function<llvm::Value *(llvm::Value *, llvm::Value *)> func)
{
	if (auto ret = localJmpDstAndNextBlk(pc, inst, instBlocks); ret) {
		auto [dstBlk, nextBlk] = ret.get();
		auto [src, dst, _] =
			emitJmpLoadSrcAndDstAndZero(inst, &regs[0], builder);
		builder.CreateCondBr(func(dst, src), dstBlk, nextBlk);
		return 0;
	} else {
		return ret.takeError();
	}
}

static inline llvm::Expected<int>
emitExtFuncCall(llvm::IRBuilder<> &builder, const ebpf_inst &inst,
		const std::map<std::string, llvm::Function *> &extFunc,
		llvm::Value **regs, llvm::FunctionType *helperFuncTy,
		uint16_t pc)
{
	auto funcNameToCall = ext_func_sym(inst.imm);
	if (auto itr = extFunc.find(funcNameToCall); itr != extFunc.end()) {
		spdlog::debug("Emitting ext func call to {} name {} at pc {}",
			      inst.imm, funcNameToCall, pc);
		auto callInst = builder.CreateCall(
			helperFuncTy, itr->second,
			{
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[1]),
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[2]),
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[3]),
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[4]),
				builder.CreateLoad(builder.getInt64Ty(),
						   regs[5]),

			});
		builder.CreateStore(callInst, regs[0]);
		return 0;
	} else {
		return llvm::make_error<llvm::StringError>(
			"Ext func not found: " + funcNameToCall,
			llvm::inconvertibleErrorCode());
	}
}
static inline void emitAtomicBinOp(llvm::IRBuilder<> &builder,
				   llvm::Value **regs,
				   llvm::AtomicRMWInst::BinOp op,
				   const ebpf_inst &inst, bool is64,
				   bool is_fetch)
{
	auto oldValue = builder.CreateAtomicRMW(
		op,
		builder.CreateGEP(builder.getInt8Ty(),
				  builder.CreateLoad(builder.getPtrTy(),
						     regs[inst.dst_reg]),
				  { builder.getInt64(inst.off) }),
		is64 ? builder.CreateLoad(builder.getInt64Ty(),
					  regs[inst.src_reg]) :
		       builder.CreateTrunc(
			       builder.CreateLoad(builder.getInt64Ty(),
						  regs[inst.src_reg]),
			       builder.getInt32Ty()),
		llvm::MaybeAlign(32), llvm::AtomicOrdering::Monotonic);
	if (is_fetch) {
		builder.CreateStore(oldValue, regs[inst.src_reg]);
	}
}
#endif
