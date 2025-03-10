#include <catch2/catch_test_macros.hpp>
#include <attach/attach_manager/frida_attach_manager.hpp>
#if !defined(__x86_64__) && defined(_M_X64)
#error Only supports x86_64
#endif

using namespace bpftime;
__attribute__((__noinline__)) extern "C" uint64_t
__bpftime_func_to_replace(uint64_t a, uint64_t b)
{
	// Forbid inline
	asm("");
	return (a << 32) | b;
}
TEST_CASE("Test attaching replace programs and revert")
{
	frida_attach_manager man;
	auto func_addr =
		man.find_function_addr_by_name("__bpftime_func_to_replace");

	REQUIRE(func_addr != nullptr);
	const uint64_t a = 0xabce;
	const uint64_t b = 0x1234;
	const uint64_t expected_result = (a << 32) | b;
	REQUIRE(__bpftime_func_to_replace(a, b) == expected_result);
	int invoke_times = 0;
	int id = man.attach_replace_at(func_addr,
				       [&](const pt_regs &regs) -> uint64_t {
					       invoke_times++;
					       return regs.si + regs.di;
				       });
	REQUIRE(id >= 0);
	REQUIRE(__bpftime_func_to_replace(a, b) == a + b);
	REQUIRE(invoke_times == 1);
	// Revert it
	invoke_times = 0;
	SECTION("Revert by id")
	{
		REQUIRE(man.destroy_attach(id) >= 0);
	}
	SECTION("Revert by function address")
	{
		REQUIRE(man.destroy_attach_by_func_addr(func_addr) >= 0);
	}
	REQUIRE(__bpftime_func_to_replace(a, b) == expected_result);
	REQUIRE(invoke_times == 0);
}
