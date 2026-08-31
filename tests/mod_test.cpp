#include "core/loader/mod.h"

#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::kPC;
using zeebulator::kR0;

TEST(Mod, LoadsBytesAtBaseAddressAndSetsEntryPoint) {
  ArmInterpreter cpu;
  std::vector<uint8_t> mod_data = {0x05, 0x00, 0xA0, 0xE3};  // MOV R0, #5 (LE bytes)
  uint32_t base = 0x00100000;

  zeebulator::LoadMod(cpu, mod_data, base);

  EXPECT_EQ(cpu.GetRegister(kPC), base);
  EXPECT_EQ(cpu.GetMemory().Read32(base), 0xE3A00005u);
}

TEST(Mod, LoadedImageActuallyExecutesCorrectlyAtItsBaseAddress) {
  ArmInterpreter cpu;
  std::vector<uint8_t> mod_data = {0x05, 0x00, 0xA0, 0xE3};  // MOV R0, #5
  uint32_t base = 0x00200000;  // arbitrary, non-zero -- position independence

  zeebulator::LoadMod(cpu, mod_data, base);
  cpu.Step();

  EXPECT_EQ(cpu.GetRegister(kR0), 5u);
  EXPECT_EQ(cpu.GetRegister(kPC), base + 4);
}

TEST(Mod, SameImageProducesSameBehaviorAtADifferentBaseAddress) {
  // Loadability at an arbitrary address is the whole point of the
  // position-independence finding -- this isn't redundant with the test
  // above, it's the actual property being verified.
  std::vector<uint8_t> mod_data = {0x05, 0x00, 0xA0, 0xE3};  // MOV R0, #5

  ArmInterpreter cpu_a;
  zeebulator::LoadMod(cpu_a, mod_data, 0x00100000);
  cpu_a.Step();

  ArmInterpreter cpu_b;
  zeebulator::LoadMod(cpu_b, mod_data, 0x00500000);
  cpu_b.Step();

  EXPECT_EQ(cpu_a.GetRegister(kR0), cpu_b.GetRegister(kR0));
}
