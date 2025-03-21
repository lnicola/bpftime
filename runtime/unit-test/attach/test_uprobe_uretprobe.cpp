#include "attach/attach_internal.hpp"
#include "attach/attach_manager/frida_attach_manager.hpp"
#include "catch2/catch_message.hpp"
#include "catch2/generators/catch_generators_random.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#if !defined(__x86_64__) && defined(_M_X64)
#error Only supports x86_64
#endif

using namespace bpftime;

__attribute__((__noinline__)) extern "C" uint64_t __test_simple_add(uint64_t a,
								    uint64_t b)
{
	// Forbid inline
	asm("");
	return a * 2 + b;
}

TEST_CASE("Test attaching uprobing programs and reverting")
{
	int invoke_times = 0;
	uint64_t a, b;
	uint64_t a2, b2;
	uint64_t ret;
	frida_attach_manager man;
	auto func_addr = man.find_function_addr_by_name("__test_simple_add");

	REQUIRE(func_addr != 0);
	int id1 = man.attach_uprobe_at(func_addr, [&](const pt_regs &regs) {
		invoke_times++;
		a = regs.di;
		b = regs.si;
	});
	INFO("id1=" << id1);
	REQUIRE(id1 >= 0);
	ret = __test_simple_add(2333, 6666);
	REQUIRE(invoke_times == 1);
	REQUIRE(a == 2333);
	REQUIRE(b == 6666);
	REQUIRE(ret == 2333 * 2 + 6666);
	invoke_times = 0;

	a = b = 0;
	int id2 = man.attach_uprobe_at(func_addr, [&](const pt_regs &regs) {
		invoke_times++;
		a2 = regs.di;
		b2 = regs.si;
	});
	INFO("id2=" << id2);
	REQUIRE(id2 >= 0);
	ret = __test_simple_add(2333, 6666);
	REQUIRE(invoke_times == 2);
	REQUIRE(a == 2333);
	REQUIRE(b == 6666);
	REQUIRE(a2 == 2333);
	REQUIRE(b2 == 6666);
	REQUIRE(ret == 2333 * 2 + 6666);
	// Revert them
	SECTION("Revert by id")
	{
		REQUIRE(man.destroy_attach(id1) == 0);
		REQUIRE(man.destroy_attach(id2) == 0);
	}
	SECTION("Revert by function address")
	{
		REQUIRE(man.destroy_attach_by_func_addr(func_addr) >= 0);
	}
	invoke_times = 0;
	a = b = a2 = b2 = 0;
	ret = __test_simple_add(2333, 6666);
	REQUIRE(ret == 2333 * 2 + 6666);
	REQUIRE(a == 0);
	REQUIRE(b == 0);
	REQUIRE(a2 == 0);
	REQUIRE(b2 == 0);
	REQUIRE(invoke_times == 0);
}

TEST_CASE("Test uretprobe and reverting")
{
	int invoke_times = 0;
	uint64_t dummy;
	uint64_t ret1, ret2;
	frida_attach_manager man;
	auto func_addr = man.find_function_addr_by_name("__test_simple_add");

	REQUIRE(func_addr != 0);
	int id1 = man.attach_uretprobe_at(func_addr, [&](const pt_regs &regs) {
		invoke_times++;
		ret1 = regs.ax;
	});
	INFO("id1=" << id1);
	REQUIRE(id1 >= 0);
	dummy = __test_simple_add(2333, 6666);
	REQUIRE(invoke_times == 1);
	REQUIRE(dummy == 2333 * 2 + 6666);
	REQUIRE(ret1 == dummy);
	invoke_times = 0;
	ret1 = 0;
	dummy = 0;
	int id2 = man.attach_uretprobe_at(func_addr, [&](const pt_regs &regs) {
		invoke_times++;
		ret2 = regs.ax;
	});
	INFO("id2=" << id2);
	REQUIRE(id2 >= 0);
	dummy = __test_simple_add(2333, 6666);
	REQUIRE(invoke_times == 2);
	REQUIRE(dummy == ret1);
	REQUIRE(ret1 == 2333 * 2 + 6666);
	REQUIRE(ret2 == 2333 * 2 + 6666);
	// Revert them
	SECTION("Revert by id")
	{
		REQUIRE(man.destroy_attach(id1) == 0);
		REQUIRE(man.destroy_attach(id2) == 0);
	}
	SECTION("Revert by function address")
	{
		REQUIRE(man.destroy_attach_by_func_addr(func_addr) >= 0);
	}
	invoke_times = 0;
	ret1 = ret2 = 0;
	dummy = __test_simple_add(2333, 6666);
	REQUIRE(ret1 == 0);
	REQUIRE(ret2 == 0);

	REQUIRE(invoke_times == 0);
}

TEST_CASE("Test the mix usage of uprobe and uretprobe")
{
	using namespace Catch::Generators;
	int uprobe_invoke_times = 0;
	int uretprobe_invoke_times = 0;
	uint64_t a = 0, b = 0, ret = 0;
	frida_attach_manager man;
	auto func_addr = man.find_function_addr_by_name("__test_simple_add");

	REQUIRE(func_addr != 0);
	int id1 = man.attach_uprobe_at(func_addr, [&](const pt_regs &regs) {
		uprobe_invoke_times++;
		a = regs.di;
		b = regs.si;
	});
	REQUIRE(id1 >= 0);

	int id2 = man.attach_uretprobe_at(func_addr, [&](const pt_regs &regs) {
		uretprobe_invoke_times++;
		ret = regs.ax;
	});
	REQUIRE(id2 >= 0);
	uint64_t i = GENERATE(take(10, random(0, 1000)));
	uint64_t j = GENERATE(take(10, random(0, 1000)));
	uint64_t expected_sum = i * 2 + j;
	uint64_t dummy = __test_simple_add(i, j);
	REQUIRE(dummy == expected_sum);
	REQUIRE(uprobe_invoke_times == 1);
	REQUIRE(uretprobe_invoke_times == 1);
	REQUIRE(a == i);
	REQUIRE(b == j);
	REQUIRE(ret == dummy);
}
