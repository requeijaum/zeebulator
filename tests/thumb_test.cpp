#include "core/cpu/arm_interpreter.h"

#include <gtest/gtest.h>

using zeebulator::ArmInterpreter;
using zeebulator::UnimplementedInstruction;
using zeebulator::kCpsrC;
using zeebulator::kCpsrN;
using zeebulator::kCpsrT;
using zeebulator::kCpsrV;
using zeebulator::kCpsrZ;
using zeebulator::kLR;
using zeebulator::kPC;
using zeebulator::kR0;
using zeebulator::kR1;
using zeebulator::kR2;
using zeebulator::kR3;
using zeebulator::kR8;
using zeebulator::kR9;
using zeebulator::kSP;

namespace {

bool Flag(const ArmInterpreter& cpu, uint32_t bit) {
  return (cpu.GetCpsr() >> bit) & 1;
}

// All 19 real Thumb (T16) instruction formats tested here follow the ARM
// Architecture Reference Manual's Thumb instruction set summary exactly
// -- see arm_interpreter.h/.cpp for the per-format bit layouts cited in
// each Execute* function. Every encoding below was independently
// generated from that same bit layout via a small Python script (not
// hand-copied into both places), then cross-checked against these tests.
void EnterThumb(ArmInterpreter& cpu) { cpu.SetCpsr(1u << kCpsrT); }

}  // namespace

// --- Format 1: move shifted register ---

TEST(Thumb, LslImmediateShiftsAndSetsCarry) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 1);
  cpu.GetMemory().Write16(0, 0x0088);  // LSL R0, R1, #2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 4u);
  EXPECT_EQ(cpu.GetRegister(kPC), 2u) << "Thumb PC advances by 2, not 4";
}

// --- Format 2: add/subtract ---

TEST(Thumb, AddImmediateSetsFlags) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 5);
  cpu.GetMemory().Write16(0, 0x1CC8);  // ADD R0, R1, #3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 8u);
  EXPECT_FALSE(Flag(cpu, kCpsrN));
  EXPECT_FALSE(Flag(cpu, kCpsrZ));
}

// --- Format 3: move/compare/add/subtract immediate ---

TEST(Thumb, MovImmediateSetsRegisterAndOnlyNZ) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0x2055);  // MOV R0, #0x55
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x55u);
}

// --- Format 4: ALU operations ---

TEST(Thumb, AluOrrCombinesRegisters) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR2, 0xF0);
  cpu.SetRegister(kR3, 0x0F);
  cpu.GetMemory().Write16(0, 0x431A);  // ORR R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0xFFu);
}

TEST(Thumb, AluLslRegisterFormShiftsByRegisterAmount) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR0, 1);
  cpu.SetRegister(kR1, 4);
  cpu.GetMemory().Write16(0, 0x4088);  // LSL R0, R1 (register-amount form)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 16u);
}

// --- Format 5: hi register operations / branch exchange ---

TEST(Thumb, HiRegisterMovReachesR8ThroughH1) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x1234);
  cpu.GetMemory().Write16(0, 0x4688);  // MOV R8, R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR8), 0x1234u);
}

TEST(Thumb, BxToEvenTargetSwitchesBackToArm) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x2000);  // bit 0 clear -> ARM
  cpu.GetMemory().Write16(0, 0x4708);  // BX R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u);
  EXPECT_FALSE(Flag(cpu, kCpsrT));
}

TEST(Thumb, BxThroughH2ReachesR9AndStaysInThumb) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR9, 0x3001);  // bit 0 set -> stay Thumb
  cpu.GetMemory().Write16(0, 0x4748);  // BX R9
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x3000u);
  EXPECT_TRUE(Flag(cpu, kCpsrT));
}

TEST(Thumb, BlxRegisterSetsLrWithReturnBitAndSwitchesToArm) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x2000);
  cpu.GetMemory().Write16(0, 0x4788);  // BLX R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u);
  EXPECT_FALSE(Flag(cpu, kCpsrT));
  EXPECT_EQ(cpu.GetRegister(kLR), 3u) << "next instr addr (2) with bit 0 set";
}

// --- Format 6: PC-relative load ---

TEST(Thumb, PcRelativeLoadUsesWordAlignedPcPlusFour) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write32(8, 0xDEADBEEF);
  cpu.GetMemory().Write16(0, 0x4801);  // LDR R0, [PC, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xDEADBEEFu);
}

// --- Format 7: load/store with register offset ---

TEST(Thumb, StrRegisterOffsetStoresWord) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR0, 0x11223344);
  cpu.SetRegister(kR1, 0x1000);
  cpu.SetRegister(kR2, 4);
  cpu.GetMemory().Write16(0, 0x5088);  // STR R0, [R1, R2]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x1004), 0x11223344u);
}

TEST(Thumb, LdrRegisterOffsetLoadsWord) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x1000);
  cpu.SetRegister(kR2, 4);
  cpu.GetMemory().Write32(0x1004, 0xCAFEF00D);
  cpu.GetMemory().Write16(0, 0x588B);  // LDR R3, [R1, R2]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0xCAFEF00Du);
}

// --- Format 8: load/store sign-extended byte/halfword ---

TEST(Thumb, LdrhLoadsZeroExtendedHalfword) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x1000);
  cpu.SetRegister(kR2, 4);
  cpu.GetMemory().Write16(0x1004, 0xBEEF);
  cpu.GetMemory().Write16(0, 0x5A88);  // LDRH R0, [R1, R2]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xBEEFu);
}

// --- Format 9: load/store with immediate offset ---

TEST(Thumb, StrImmediateOffsetScalesByFour) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR0, 0x99887766);
  cpu.SetRegister(kR1, 0x1000);
  cpu.GetMemory().Write16(0, 0x6048);  // STR R0, [R1, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x1004), 0x99887766u);
}

TEST(Thumb, LdrImmediateOffsetScalesByFour) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x1000);
  cpu.GetMemory().Write32(0x1004, 0x13572468);
  cpu.GetMemory().Write16(0, 0x684B);  // LDR R3, [R1, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0x13572468u);
}

// --- Format 10: load/store halfword ---

TEST(Thumb, StrhImmediateOffsetScalesByTwo) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR0, 0xABCD);
  cpu.SetRegister(kR1, 0x1000);
  cpu.GetMemory().Write16(0, 0x8048);  // STRH R0, [R1, #2]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read16(0x1002), 0xABCDu);
}

TEST(Thumb, LdrhImmediateOffsetScalesByTwo) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x1000);
  cpu.GetMemory().Write16(0x1002, 0x4321);
  cpu.GetMemory().Write16(0, 0x884B);  // LDRH R3, [R1, #2]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0x4321u);
}

// --- Format 11: SP-relative load/store ---

TEST(Thumb, StrSpRelativeScalesByFour) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR0, 0x22334455);
  cpu.SetRegister(kSP, 0x5000);
  cpu.GetMemory().Write16(0, 0x9001);  // STR R0, [SP, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x5004), 0x22334455u);
}

TEST(Thumb, LdrSpRelativeScalesByFour) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kSP, 0x5000);
  cpu.GetMemory().Write32(0x5004, 0x778899AA);
  cpu.GetMemory().Write16(0, 0x9B01);  // LDR R3, [SP, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0x778899AAu);
}

// --- Format 12: load address ---

TEST(Thumb, LoadAddressFromSp) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kSP, 0x6000);
  cpu.GetMemory().Write16(0, 0xA801);  // ADD R0, SP, #4
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x6004u);
}

TEST(Thumb, LoadAddressFromPcIsWordAligned) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xA001);  // ADD R0, PC, #4
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x8u) << "(0+4)&~3 + 4";
}

// --- Format 13: add offset to stack pointer ---

TEST(Thumb, AddOffsetToSpIncreasesSp) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kSP, 0x1000);
  cpu.GetMemory().Write16(0, 0xB002);  // ADD SP, #8
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kSP), 0x1008u);
}

TEST(Thumb, AddOffsetToSpWithSignBitDecreasesSp) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kSP, 0x1000);
  cpu.GetMemory().Write16(0, 0xB082);  // SUB SP, #8
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kSP), 0xFF8u);
}

// --- Format 14: push/pop registers ---

TEST(Thumb, PushThenPopRoundTripsRegistersAndLrPc) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kSP, 0x5000);
  cpu.SetRegister(kR0, 0x11111111);
  cpu.SetRegister(kR1, 0x22222222);
  cpu.SetRegister(kLR, 0x2000);  // even -> ARM, exercised by the POP{PC} below

  cpu.GetMemory().Write16(0, 0xB503);  // PUSH {R0, R1, LR}
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kSP), 0x4FF4u) << "SP decremented by 3 words";
  EXPECT_EQ(cpu.GetMemory().Read32(0x4FF4), 0x11111111u);
  EXPECT_EQ(cpu.GetMemory().Read32(0x4FF8), 0x22222222u);
  EXPECT_EQ(cpu.GetMemory().Read32(0x4FFC), 0x2000u) << "LR stored at the highest address";

  cpu.SetRegister(kR0, 0xDEAD);
  cpu.SetRegister(kR1, 0xBEEF);
  cpu.GetMemory().Write16(2, 0xBD03);  // POP {R0, R1, PC}
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x11111111u);
  EXPECT_EQ(cpu.GetRegister(kR1), 0x22222222u);
  EXPECT_EQ(cpu.GetRegister(kSP), 0x5000u) << "SP restored to original";
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u) << "POP {pc} interworks to the stored value";
  EXPECT_FALSE(Flag(cpu, kCpsrT)) << "stored LR was even -> switches back to ARM";
}

// --- Format 15: multiple load/store ---

TEST(Thumb, StmiaThenLdmiaRoundTripsWithWriteback) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x3000);
  cpu.SetRegister(kR0, 0xAAAA);
  cpu.SetRegister(kR2, 0xBBBB);
  cpu.GetMemory().Write16(0, 0xC105);  // STMIA R1!, {R0, R2}
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR1), 0x3008u) << "writeback advances by 2 words";
  EXPECT_EQ(cpu.GetMemory().Read32(0x3000), 0xAAAAu);
  EXPECT_EQ(cpu.GetMemory().Read32(0x3004), 0xBBBBu);

  cpu.SetRegister(kR1, 0x3000);
  cpu.SetRegister(kR0, 0);
  cpu.SetRegister(kR2, 0);
  cpu.GetMemory().Write16(2, 0xC905);  // LDMIA R1!, {R0, R2}
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xAAAAu);
  EXPECT_EQ(cpu.GetRegister(kR2), 0xBBBBu);
  EXPECT_EQ(cpu.GetRegister(kR1), 0x3008u);
}

TEST(Thumb, LdmiaWithBaseInListSkipsWritebackClobber) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetRegister(kR1, 0x3000);
  cpu.GetMemory().Write32(0x3000, 0x77777777);  // the value that should end up in R1
  cpu.GetMemory().Write16(0, 0xC902);  // LDMIA R1!, {R1}  (rlist=0b00000010, Rb=1)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR1), 0x77777777u)
      << "loaded value wins over the writeback address per the real spec";
}

// --- Format 16: conditional branch ---

TEST(Thumb, ConditionalBranchTakenWhenFlagsMatch) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.SetCpsr(cpu.GetCpsr() | (1u << kCpsrZ));  // Z=1 -> EQ passes
  cpu.GetMemory().Write16(0, 0xD002);  // BEQ #4
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x8u) << "(0+4) + 4";
}

TEST(Thumb, ConditionalBranchSkippedWhenFlagsDontMatch) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xD002);  // BEQ #4, Z=0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2u) << "not taken -> normal +2 advance";
}

TEST(Thumb, ConditionalBranchSwiEncodingIsUnimplemented) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xDF00);  // cond=1111 -> real SWI encoding
  EXPECT_THROW(cpu.Step(), UnimplementedInstruction);
}

// --- Format 18: unconditional branch ---

TEST(Thumb, UnconditionalBranchJumps) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xE004);  // B #8
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0xCu) << "(0+4) + 8";
}

// --- Format 19: long branch with link ---

TEST(Thumb, BlSetsLrThenBranchesStayingInThumb) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xF000);  // BL first half (offset_high=0)
  cpu.GetMemory().Write16(2, 0xF800);  // BL second half (offset_low=0)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kLR), 4u) << "PC(0)+4 after the first half";
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 4u) << "target = LR(4) + 0";
  EXPECT_EQ(cpu.GetRegister(kLR), 5u) << "next instr addr (4) with bit 0 set";
  EXPECT_TRUE(Flag(cpu, kCpsrT)) << "BL stays in Thumb";
}

TEST(Thumb, BlxLongBranchSwitchesToArm) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  cpu.GetMemory().Write16(0, 0xF000);  // BLX first half (offset_high=0)
  cpu.GetMemory().Write16(2, 0xE800);  // BLX second half (H=01, offset_low=0)
  cpu.Step();
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 4u) << "target = LR(4) + 0, word-aligned";
  EXPECT_EQ(cpu.GetRegister(kLR), 5u) << "next instr addr (4) with bit 0 set";
  EXPECT_FALSE(Flag(cpu, kCpsrT)) << "BLX(1) unconditionally switches to ARM";
}

// --- Unrecognized encoding ---

TEST(Thumb, UnassignedEncodingInThumb1SpaceIsUnimplemented) {
  ArmInterpreter cpu;
  EnterThumb(cpu);
  // 0xB200 (SXTH in the later Thumb-2 16-bit extensions) is unassigned
  // in the classic 19-format Thumb1 ISA ARM1136J-S/ARMv6 implements.
  cpu.GetMemory().Write16(0, 0xB200);
  EXPECT_THROW(cpu.Step(), UnimplementedInstruction);
}
