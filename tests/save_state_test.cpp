#include "core/save_state.h"

#include <sstream>

#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::LoadState;
using zeebulator::SaveState;

TEST(SaveState, RoundTripsRegistersAndMemoryThroughTheVersionedWrapper) {
  ArmInterpreter cpu;
  cpu.SetRegister(zeebulator::kR2, 0x22222222);
  cpu.SetCpsr(0x60000000);
  cpu.GetMemory().Write32(0x2000, 0x12345678);

  std::stringstream stream;
  ASSERT_TRUE(SaveState(cpu, stream));

  ArmInterpreter restored;
  ASSERT_TRUE(LoadState(restored, stream));
  EXPECT_EQ(restored.GetRegister(zeebulator::kR2), 0x22222222u);
  EXPECT_EQ(restored.GetCpsr(), 0x60000000u);
  EXPECT_EQ(restored.GetMemory().Read32(0x2000), 0x12345678u);
}

TEST(SaveState, LoadRejectsAStreamWithTheWrongMagic) {
  std::stringstream stream;
  uint32_t wrong_magic = 0xDEADBEEF;
  stream.write(reinterpret_cast<const char*>(&wrong_magic), sizeof(wrong_magic));
  uint32_t version = 1;
  stream.write(reinterpret_cast<const char*>(&version), sizeof(version));

  ArmInterpreter cpu;
  EXPECT_FALSE(LoadState(cpu, stream));
}

TEST(SaveState, LoadRejectsAFutureVersionNumber) {
  ArmInterpreter cpu;
  std::stringstream stream;
  ASSERT_TRUE(SaveState(cpu, stream));

  std::string bytes = stream.str();
  uint32_t future_version = 999;
  bytes.replace(4, sizeof(future_version), reinterpret_cast<const char*>(&future_version),
                sizeof(future_version));
  std::stringstream tampered(bytes);

  ArmInterpreter restored;
  EXPECT_FALSE(LoadState(restored, tampered));
}

TEST(SaveState, LoadOnAnEmptyStreamFailsWithoutCrashing) {
  ArmInterpreter cpu;
  std::stringstream empty_stream;
  EXPECT_FALSE(LoadState(cpu, empty_stream));
}
