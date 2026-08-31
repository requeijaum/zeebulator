#include "core/brew/hle_runtime.h"

#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::kR0;
using zeebulator::kR1;

TEST(HleRuntime, CallArmFunctionRunsCodeAndReturnsR0) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);

  // ADD R0, R0, R1 ; BX LR
  cpu.GetMemory().Write32(0x1000, 0xE0800001);
  cpu.GetMemory().Write32(0x1004, 0xE12FFF1E);

  uint32_t result = hle.CallArmFunction(0x1000, /*r0=*/3, /*r1=*/4);
  EXPECT_EQ(result, 7u);
}

TEST(HleRuntime, RegisteredFunctionReceivesArgsAndSetsReturnValue) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);

  bool called = false;
  uint32_t sentinel = hle.Register([&](zeebulator::IArmCore& core) {
    called = true;
    core.SetRegister(kR0, core.GetRegister(kR0) * 2);
  });

  // Simulate real compiled code loading a vtable function pointer and
  // calling through it -- including saving/restoring LR around the
  // nested call the way real functions do (BLX clobbers LR with its own
  // return address, so a naive "just BX LR at the end" would jump back
  // into itself instead of to the original caller):
  //   MOV R3, LR ; MOV R0,#5 ; LDR R2,[PC,#8] ; BLX R2 ;
  //   MOV LR,R3 ; BX LR ; <literal>
  cpu.GetMemory().Write32(0x1000, 0xE1A0300E);  // MOV R3, LR
  cpu.GetMemory().Write32(0x1004, 0xE3A00005);  // MOV R0, #5
  cpu.GetMemory().Write32(0x1008, 0xE59F2008);  // LDR R2, [PC, #8]
  cpu.GetMemory().Write32(0x100C, 0xE12FFF32);  // BLX R2
  cpu.GetMemory().Write32(0x1010, 0xE1A0E003);  // MOV LR, R3
  cpu.GetMemory().Write32(0x1014, 0xE12FFF1E);  // BX LR
  cpu.GetMemory().Write32(0x1018, sentinel);    // literal pool

  uint32_t result = hle.CallArmFunction(0x1000);
  EXPECT_TRUE(called);
  EXPECT_EQ(result, 10u);
}

TEST(HleRuntime, MultipleRegisteredFunctionsGetDistinctAddresses) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);

  uint32_t a = hle.Register([](zeebulator::IArmCore& core) {
    core.SetRegister(kR0, 111);
  });
  uint32_t b = hle.Register([](zeebulator::IArmCore& core) {
    core.SetRegister(kR0, 222);
  });
  EXPECT_NE(a, b);

  cpu.GetMemory().Write32(0x1000, 0xE12FFF1E);  // BX LR (unused directly)
  EXPECT_EQ(hle.CallArmFunction(a), 111u);
  EXPECT_EQ(hle.CallArmFunction(b), 222u);
}

TEST(HleRuntime, ReadStackArgReadsBeyondFirstFourRegisters) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  cpu.SetRegister(zeebulator::kSP, 0x2000);
  cpu.GetMemory().Write32(0x2000, 0xAAAAAAAA);
  cpu.GetMemory().Write32(0x2004, 0xBBBBBBBB);

  EXPECT_EQ(HleRuntime::ReadStackArg(cpu, 0), 0xAAAAAAAAu);
  EXPECT_EQ(HleRuntime::ReadStackArg(cpu, 1), 0xBBBBBBBBu);
}
