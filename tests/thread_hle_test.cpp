#include "core/brew/thread_hle.h"
#include <gtest/gtest.h>
#include <vector>
#include "core/cpu/arm_interpreter.h"

TEST(ThreadHle, AddRefQueryInterfaceAndReleaseOwnExactlyOneLifetime) {
  zeebulator::ArmInterpreter cpu;
  zeebulator::HleRuntime hle(cpu, 0xf0000000u, 0x10000u);
  uint32_t next = 0x80010000u;
  std::vector<uint32_t> freed;
  zeebulator::ThreadHle threads(
      cpu.GetMemory(), hle,
      [&](uint32_t size) { uint32_t p = next; next += (size + 15u) & ~15u; return p; },
      [&](uint32_t p) { freed.push_back(p); });
  const uint32_t obj = threads.CreateThreadObject();
  ASSERT_NE(obj, 0u);
  const uint32_t vt = cpu.GetMemory().Read32(obj);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 0), obj), 2u);
  const uint32_t out = 0x90000u;
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 8), obj, 0, out), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(out), obj);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 4), obj), 2u);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 4), obj), 1u);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 4), obj), 0u);
  const size_t frees_after_final_release = freed.size();
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(vt + 4), obj), 0u);
  EXPECT_EQ(freed.size(), frees_after_final_release);
}
