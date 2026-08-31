#include "core/cpu/arm_interpreter.h"

#include <sstream>

#include <gtest/gtest.h>

using zeebulator::ArmInterpreter;
using zeebulator::UnimplementedInstruction;
using zeebulator::kCpsrC;
using zeebulator::kCpsrN;
using zeebulator::kCpsrV;
using zeebulator::kCpsrZ;
using zeebulator::kLR;
using zeebulator::kPC;
using zeebulator::kR0;
using zeebulator::kR1;
using zeebulator::kR2;
using zeebulator::kR3;
using zeebulator::kR4;
using zeebulator::kSP;

namespace {

bool Flag(const ArmInterpreter& cpu, uint32_t bit) {
  return (cpu.GetCpsr() >> bit) & 1;
}

}  // namespace

// --- Data processing: immediate operand2 ---

TEST(Cpu, MovImmediate) {
  ArmInterpreter cpu;
  cpu.GetMemory().Write32(0, 0xE3A00005);  // MOV R0, #5
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 5u);
  EXPECT_EQ(cpu.GetRegister(kPC), 4u);  // advanced by 4, no branch
}

TEST(Cpu, ConditionalInstructionSkippedWhenConditionFails) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 42);
  cpu.SetCpsr(0);  // Z=0, so EQ fails
  cpu.GetMemory().Write32(0, 0x03A00063);  // MOVEQ R0, #99
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 42u) << "MOVEQ should not execute when Z=0";
  EXPECT_EQ(cpu.GetRegister(kPC), 4u) << "PC still advances on a skipped instruction";
}

// --- Data processing: register operand2, arithmetic, flags ---

TEST(Cpu, AddRegisterForm) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 5);
  cpu.SetRegister(kR1, 10);
  cpu.GetMemory().Write32(0, 0xE0802001);  // ADD R2, R0, R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 15u);
}

TEST(Cpu, SubsSetsCarryWhenNoBorrow) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 5);
  cpu.SetRegister(kR2, 3);
  cpu.GetMemory().Write32(0, 0xE0513002);  // SUBS R3, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 2u);
  EXPECT_TRUE(Flag(cpu, kCpsrC)) << "no borrow: 5 >= 3";
  EXPECT_FALSE(Flag(cpu, kCpsrZ));
  EXPECT_FALSE(Flag(cpu, kCpsrN));
}

TEST(Cpu, SubsClearsCarryOnBorrowAndSetsNegative) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 3);
  cpu.SetRegister(kR2, 5);
  cpu.GetMemory().Write32(0, 0xE0513002);  // SUBS R3, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0xFFFFFFFEu);  // 3 - 5 = -2
  EXPECT_FALSE(Flag(cpu, kCpsrC)) << "borrow occurred: 3 < 5";
  EXPECT_TRUE(Flag(cpu, kCpsrN));
  EXPECT_FALSE(Flag(cpu, kCpsrV)) << "no signed overflow for 3 - 5";
}

TEST(Cpu, SubsZeroResultSetsZeroAndCarry) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 5);
  cpu.SetRegister(kR2, 5);
  cpu.GetMemory().Write32(0, 0xE0513002);  // SUBS R3, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0u);
  EXPECT_TRUE(Flag(cpu, kCpsrZ));
  EXPECT_TRUE(Flag(cpu, kCpsrC));
}

TEST(Cpu, AndOrrMvnBic) {
  ArmInterpreter cpu;

  cpu.SetRegister(kR1, 0xFF00);
  cpu.SetRegister(kR2, 0x0FF0);
  cpu.GetMemory().Write32(0, 0xE0110002);  // ANDS R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x0F00u);
  EXPECT_FALSE(Flag(cpu, kCpsrZ));

  cpu.SetRegister(kR1, 0xF0);
  cpu.SetRegister(kR2, 0x0F);
  cpu.GetMemory().Write32(4, 0xE1813002);  // ORR R3, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0xFFu);

  cpu.SetRegister(kR1, 0);
  cpu.GetMemory().Write32(8, 0xE1E00001);  // MVN R0, R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xFFFFFFFFu);

  cpu.SetRegister(kR1, 0xFF);
  cpu.SetRegister(kR2, 0x0F);
  cpu.GetMemory().Write32(12, 0xE1C10002);  // BIC R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xF0u);
}

TEST(Cpu, CmpDoesNotWriteDestinationRegister) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0xDEAD);  // sentinel — CMP must not touch R0
  cpu.SetRegister(kR1, 5);
  cpu.SetRegister(kR2, 5);
  cpu.GetMemory().Write32(0, 0xE1510002);  // CMP R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xDEADu);
  EXPECT_TRUE(Flag(cpu, kCpsrZ));
  EXPECT_TRUE(Flag(cpu, kCpsrC));
}

// --- Data processing: shifted register operand2 ---

TEST(Cpu, MovWithImmediateShift) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 3);
  cpu.GetMemory().Write32(0, 0xE1A00101);  // MOV R0, R1, LSL #2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 12u);
}

TEST(Cpu, MovWithRegisterSpecifiedShift) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 1);
  cpu.SetRegister(kR2, 4);
  cpu.GetMemory().Write32(0, 0xE1A00211);  // MOV R0, R1, LSL R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 16u);
}

// --- PC-as-operand semantics ---

TEST(Cpu, ReadingPcAsOperandYieldsInstructionAddressPlusEight) {
  ArmInterpreter cpu;
  cpu.SetRegister(kPC, 0x8000);
  cpu.GetMemory().Write32(0x8000, 0xE28F0000);  // ADD R0, PC, #0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x8008u);
  EXPECT_EQ(cpu.GetRegister(kPC), 0x8004u);
}

// --- Branch ---

TEST(Cpu, BranchForward) {
  ArmInterpreter cpu;
  cpu.SetRegister(kPC, 0);
  cpu.GetMemory().Write32(0, 0xEA00003E);  // B #0x100 (target = 0 + 8 + 248)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x100u);
}

TEST(Cpu, BranchWithLinkSetsLr) {
  ArmInterpreter cpu;
  cpu.SetRegister(kPC, 0x1000);
  cpu.GetMemory().Write32(0x1000, 0xEB000000);  // BL #0 (target = pc + 8)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kLR), 0x1004u) << "LR = address of next instruction";
  EXPECT_EQ(cpu.GetRegister(kPC), 0x1008u);
}

// --- Single data transfer ---

TEST(Cpu, StrThenLdrWordRoundTrip) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.SetRegister(kR1, 0xDEADBEEF);
  cpu.GetMemory().Write32(0, 0xE5801004);  // STR R1, [R0, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x2004), 0xDEADBEEFu);
  EXPECT_EQ(cpu.GetRegister(kR0), 0x2000u) << "no writeback on plain offset addressing";

  cpu.GetMemory().Write32(4, 0xE5902004);  // LDR R2, [R0, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0xDEADBEEFu);
}

TEST(Cpu, StrbStoresOnlyLowByte) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x3000);
  cpu.SetRegister(kR1, 0x1234ABCD);
  cpu.GetMemory().Write32(0, 0xE5C01000);  // STRB R1, [R0]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read8(0x3000), 0xCD);
  EXPECT_EQ(cpu.GetMemory().Read8(0x3001), 0);
}

TEST(Cpu, PostIndexedStoreWritesBackBase) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x4000);
  cpu.SetRegister(kR1, 0x99);
  cpu.GetMemory().Write32(0, 0xE4801004);  // STR R1, [R0], #4
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x4000), 0x99u);
  EXPECT_EQ(cpu.GetRegister(kR0), 0x4004u) << "post-indexed always writes back";
}

// --- Call-out trap hook (ARCHITECTURE.md 3.4) ---

TEST(Cpu, CallOutRangeTrapsInsteadOfExecuting) {
  ArmInterpreter cpu;
  cpu.SetCallOutRange(0x9000000, 0x1000);
  uint32_t trapped_address = 0;
  int call_count = 0;
  cpu.SetCallOutHandler([&](zeebulator::IArmCore&, uint32_t address) {
    trapped_address = address;
    ++call_count;
  });
  cpu.SetRegister(kPC, 0x9000000);
  cpu.Step();
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(trapped_address, 0x9000000u);
}

TEST(Cpu, RunStopsEarlyOnCallOutTrap) {
  ArmInterpreter cpu;
  cpu.SetCallOutRange(0x9000000, 0x1000);
  int call_count = 0;
  cpu.SetCallOutHandler(
      [&](zeebulator::IArmCore&, uint32_t) { ++call_count; });
  // Simulate having already branched into the trap range (e.g. a BREW
  // vtable call landing on a sentinel address) rather than contriving a
  // reachable branch instruction into it.
  cpu.SetRegister(kPC, 0x9000000);
  uint64_t executed = cpu.Run(10);
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(executed, 1u) << "Run() must stop immediately on a call-out trap";
}

// --- Halfword / signed transfer ---

TEST(Cpu, StrhThenLdrhRoundTrip) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.SetRegister(kR1, 0xBEEF1234);
  cpu.GetMemory().Write32(0, 0xE1C010B4);  // STRH R1, [R0, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read16(0x2004), 0x1234u)
      << "STRH stores only the low halfword";

  cpu.GetMemory().Write32(4, 0xE1D020B4);  // LDRH R2, [R0, #4]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0x1234u) << "LDRH zero-extends";
}

TEST(Cpu, LdrsbSignExtendsNegativeByte) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.GetMemory().Write8(0x2000, 0xFF);  // -1 as a signed byte
  cpu.GetMemory().Write32(0, 0xE1D030D0);  // LDRSB R3, [R0]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR3), 0xFFFFFFFFu);
}

TEST(Cpu, LdrshSignExtendsNegativeHalfword) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.GetMemory().Write16(0x2000, 0x8000);  // negative as a signed halfword
  cpu.GetMemory().Write32(0, 0xE1D040F0);  // LDRSH R4, [R0]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR4), 0xFFFF8000u);
}

TEST(Cpu, LdrdLoadsTwoConsecutiveRegisters) {
  // LDRD shares the halfword-transfer store opcode space (L=0), with
  // SH=10 marking it as the real paired-register load instead -- real
  // encoding confirmed by assembling `ldrd r8, [sp, #32]` and reading
  // the actual bytes back (0xE1CD82D0), not guessed. Found live: real
  // Zeebo Sports Tênis code uses this exact shape.
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.GetMemory().Write32(0x2008, 0x11111111);
  cpu.GetMemory().Write32(0x200C, 0x22222222);
  cpu.GetMemory().Write32(0, 0xE1C020D8);  // LDRD R2, [R0, #8]
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0x11111111u);
  EXPECT_EQ(cpu.GetRegister(kR3), 0x22222222u);
}

TEST(Cpu, StrdStoresTwoConsecutiveRegisters) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x2000);
  cpu.SetRegister(kR2, 0x33333333);
  cpu.SetRegister(kR3, 0x44444444);
  cpu.GetMemory().Write32(0, 0xE1C020F8);  // STRD R2, [R0, #8]
  cpu.Step();
  EXPECT_EQ(cpu.GetMemory().Read32(0x2008), 0x33333333u);
  EXPECT_EQ(cpu.GetMemory().Read32(0x200C), 0x44444444u);
}

// --- Unimplemented instruction classes are rejected, not mis-executed ---

TEST(Cpu, SwapInstructionIsUnimplemented) {
  ArmInterpreter cpu;
  cpu.GetMemory().Write32(0, 0xE1020091);  // SWP R0, R1, [R2]
  EXPECT_THROW(cpu.Step(), UnimplementedInstruction);
}

// --- Multiply / multiply-accumulate (MUL/MLA, UMULL/UMLAL/SMULL/SMLAL) ---

TEST(Cpu, MulComputesProduct) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 6);
  cpu.SetRegister(kR2, 7);
  cpu.GetMemory().Write32(0, 0xE0000291);  // MUL R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 42u);
}

TEST(Cpu, MulsSetsZeroFlagOnZeroResult) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0);
  cpu.SetRegister(kR2, 99);
  cpu.GetMemory().Write32(0, 0xE0100291);  // MULS R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0u);
  EXPECT_TRUE(Flag(cpu, kCpsrZ));
  EXPECT_FALSE(Flag(cpu, kCpsrN));
}

TEST(Cpu, MulsSetsNegativeFlagWhenResultBit31Set) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x10000);
  cpu.SetRegister(kR2, 0x8001);
  cpu.GetMemory().Write32(0, 0xE0100291);  // MULS R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x80010000u);
  EXPECT_TRUE(Flag(cpu, kCpsrN));
  EXPECT_FALSE(Flag(cpu, kCpsrZ));
}

TEST(Cpu, MlaAddsAccumulator) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 6);
  cpu.SetRegister(kR2, 7);
  cpu.SetRegister(kR3, 100);
  cpu.GetMemory().Write32(0, 0xE0203291);  // MLA R0, R1, R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 142u);  // 6*7 + 100
}

TEST(Cpu, UmullComputesFullUnsigned64BitProduct) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR2, 0xFFFFFFFFu);  // Rm
  cpu.SetRegister(kR3, 0xFFFFFFFFu);  // Rs
  cpu.GetMemory().Write32(0, 0xE0810392);  // UMULL R0, R1, R2, R3 (RdLo=R0, RdHi=R1)
  cpu.Step();
  // 0xFFFFFFFF^2 = 0xFFFFFFFE00000001 -- exercises the full 64-bit
  // width, not just what fits in one 32-bit register.
  EXPECT_EQ(cpu.GetRegister(kR0), 0x00000001u);
  EXPECT_EQ(cpu.GetRegister(kR1), 0xFFFFFFFEu);
}

TEST(Cpu, UmlalAccumulatesWithCarryFromLowToHighWord) {
  ArmInterpreter cpu;
  // Pre-existing 64-bit accumulator = 0x00000000FFFFFFFF.
  cpu.SetRegister(kR0, 0xFFFFFFFFu);  // RdLo
  cpu.SetRegister(kR1, 0x00000000u);  // RdHi
  cpu.SetRegister(kR2, 1);            // Rm
  cpu.SetRegister(kR3, 1);            // Rs
  cpu.GetMemory().Write32(0, 0xE0A10392);  // UMLAL R0, R1, R2, R3
  cpu.Step();
  // 0xFFFFFFFF + (1*1) = 0x100000000 -- the carry must propagate into
  // RdHi, not just wrap RdLo silently.
  EXPECT_EQ(cpu.GetRegister(kR0), 0u);
  EXPECT_EQ(cpu.GetRegister(kR1), 1u);
}

TEST(Cpu, SmullComputesSignedProductCorrectly) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR2, static_cast<uint32_t>(-5));  // Rm
  cpu.SetRegister(kR3, 10);                          // Rs
  cpu.GetMemory().Write32(0, 0xE0C10392);  // SMULL R0, R1, R2, R3
  cpu.Step();
  // -5 * 10 = -50 = 0xFFFFFFFFFFFFFFCE as a 64-bit two's-complement value.
  EXPECT_EQ(cpu.GetRegister(kR0), 0xFFFFFFCEu);
  EXPECT_EQ(cpu.GetRegister(kR1), 0xFFFFFFFFu);
}

TEST(Cpu, SmullsSetsNegativeFlagFromBit63) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR2, static_cast<uint32_t>(-5));
  cpu.SetRegister(kR3, 10);
  cpu.GetMemory().Write32(0, 0xE0D10392);  // SMULLS R0, R1, R2, R3
  cpu.Step();
  EXPECT_TRUE(Flag(cpu, kCpsrN)) << "bit63 of a negative 64-bit result must be reflected in N";
  EXPECT_FALSE(Flag(cpu, kCpsrZ));
}

TEST(Cpu, BranchExchangeJumpsToArmTarget) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x2000);  // bit 0 clear -> stay in ARM state
  cpu.GetMemory().Write32(0, 0xE12FFF11);  // BX R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u);
}

TEST(Cpu, BranchExchangeToThumbTargetSwitchesState) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x2001);  // bit 0 set -> requests Thumb state
  cpu.GetMemory().Write32(0, 0xE12FFF11);  // BX R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u) << "bit 0 cleared from the target";
  EXPECT_TRUE(Flag(cpu, zeebulator::kCpsrT));
}

TEST(Cpu, ClzCountsLeadingZeroBits) {
  // CLZ shares the same "miscellaneous instructions" encoding space as
  // MRS/MSR/BX/BLX (cond 0001 0110 1111 Rd 1111 0001 Rm) -- real
  // ARMv5T+ instruction, found live in Zeebo Sports Tênis soft-float
  // normalization code.
  ArmInterpreter cpu;
  cpu.SetRegister(kR3, 0x00010000);  // highest set bit at position 16
  cpu.GetMemory().Write32(0, 0xE16F2F13);  // CLZ R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 15u);
}

TEST(Cpu, ClzOfZeroReturnsThirtyTwo) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR3, 0);
  cpu.GetMemory().Write32(0, 0xE16F2F13);  // CLZ R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 32u);
}

TEST(Cpu, RevByteReversesWord) {
  // REV shares the same "media instructions" encoding space as the
  // Extend family (cond 0110 1011 1111 Rd 1111 0011 Rm, bits[27:20]=0x6B
  // same as UXTB/UXTAB, disambiguated by bits[9:4]=0b110011) -- real
  // ARMv6 instruction, found live in Heavy Weapon's own real endian-swap
  // loop over a loaded data block.
  ArmInterpreter cpu;
  cpu.SetRegister(kR3, 0x12345678);
  cpu.GetMemory().Write32(0, 0xE6BF2F33);  // REV R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0x78563412u);
}

TEST(Cpu, RevOfZeroIsZero) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR3, 0);
  cpu.GetMemory().Write32(0, 0xE6BF2F33);  // REV R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0u);
}

TEST(Cpu, Rev16SwapsBytesWithinEachHalfwordIndependently) {
  // REV16's sibling encoding (bits[7:4]=1011 vs. REV's 0011) -- found
  // live immediately after REV in Heavy Weapon's own real endian-swap
  // code, same loop shape but over 16-bit halfwords.
  ArmInterpreter cpu;
  cpu.SetRegister(kR3, 0x12345678);
  cpu.GetMemory().Write32(0, 0xE6BF2FB3);  // REV16 R2, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0x34127856u);
}

TEST(Cpu, SmulbbMultipliesBottomHalfwordsSignExtended) {
  // SMULxy shares the same "miscellaneous instructions" encoding space
  // as MRS/MSR/BX/BLX/CLZ (cond 0001 0110 Rd 0000 Rs 1yx0 Rm) -- real
  // ARMv5TE DSP instruction, found live in Zeeboids' own per-entity
  // update code. SMULBB (y=0, x=0): both operands' bottom 16 bits.
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0xFFFF8000);  // low half = -32768
  cpu.SetRegister(kR3, 0x00000002);  // low half = 2
  cpu.GetMemory().Write32(0, 0xE1620381);  // SMULBB R2, R1, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0xFFFF0000u) << "-32768 * 2 = -65536";
}

TEST(Cpu, SmultbMultipliesTopRmByBottomRs) {
  // SMULBT (y=0, x=1): Rm's top halfword, Rs's bottom halfword.
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0xFFFF0000);  // top half = -1
  cpu.SetRegister(kR3, 0x00000005);  // low half = 5
  cpu.GetMemory().Write32(0, 0xE16203A1);  // SMULBT R2, R1, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0xFFFFFFFBu) << "-1 * 5 = -5";
}

TEST(Cpu, SmulttMultipliesTopHalfwordsSignExtended) {
  // SMULTT (y=1, x=1): both operands' top 16 bits.
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x80000000);  // top half = -32768
  cpu.SetRegister(kR3, 0x00020000);  // top half = 2
  cpu.GetMemory().Write32(0, 0xE16203E1);  // SMULTT R2, R1, R3
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR2), 0xFFFF0000u) << "-32768 * 2 = -65536";
}

TEST(Cpu, BranchWithLinkExchangeSetsLrAndJumps) {
  ArmInterpreter cpu;
  cpu.SetRegister(kPC, 0x1000);
  cpu.SetRegister(kR1, 0x2000);
  cpu.GetMemory().Write32(0x1000, 0xE12FFF31);  // BLX R1
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kPC), 0x2000u);
  EXPECT_EQ(cpu.GetRegister(kLR), 0x1004u) << "LR = address of next instruction";
}

TEST(Cpu, BlockDataTransferWithUserBankBitIsUnimplemented) {
  ArmInterpreter cpu;
  cpu.GetMemory().Write32(0, 0xE8FD0001);  // LDM with S=1 set (bit 22)
  EXPECT_THROW(cpu.Step(), UnimplementedInstruction);
}

TEST(Cpu, PushThenPopRoundTripsMultipleRegisters) {
  ArmInterpreter cpu;
  cpu.SetRegister(kSP, 0x5000);
  cpu.SetRegister(kR0, 0x11111111);
  cpu.SetRegister(kR1, 0x22222222);

  cpu.GetMemory().Write32(0, 0xE92D0003);  // PUSH {R0, R1}  (STMDB SP!)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kSP), 0x4FF8u) << "SP decremented by 2 words";
  // Lower register number lands at the lower address, regardless of
  // push/pop direction.
  EXPECT_EQ(cpu.GetMemory().Read32(0x4FF8), 0x11111111u);
  EXPECT_EQ(cpu.GetMemory().Read32(0x4FFC), 0x22222222u);

  cpu.SetRegister(kR0, 0xDEAD);  // clobber before popping back
  cpu.SetRegister(kR1, 0xBEEF);
  cpu.GetMemory().Write32(4, 0xE8BD0003);  // POP {R0, R1}  (LDMIA SP!)
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x11111111u);
  EXPECT_EQ(cpu.GetRegister(kR1), 0x22222222u);
  EXPECT_EQ(cpu.GetRegister(kSP), 0x5000u) << "SP restored to original";
}

// --- Extend (and add): SXTB/SXTH/UXTB/UXTH and accumulate forms ---
// Encodings confirmed by assembling the real mnemonics with
// arm-none-eabi-as and reading back the actual bytes (see
// ArmInterpreter::ExecuteExtend's doc comment) -- not hand-derived.

TEST(Cpu, UxthZeroExtendsLowHalfword) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0xABCD1234);
  cpu.GetMemory().Write32(0, 0xE6FF0070);  // UXTH R0, R0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x00001234u);
}

TEST(Cpu, UxtbZeroExtendsLowByte) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0xABCD1234);
  cpu.GetMemory().Write32(0, 0xE6EF0070);  // UXTB R0, R0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x00000034u);
}

TEST(Cpu, SxtbSignExtendsNegativeByte) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x00000080);  // low byte 0x80 -- negative as int8
  cpu.GetMemory().Write32(0, 0xE6AF0070);  // SXTB R0, R0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xFFFFFF80u);
}

TEST(Cpu, SxthSignExtendsNegativeHalfword) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x00008000);  // low halfword 0x8000 -- negative as int16
  cpu.GetMemory().Write32(0, 0xE6BF0070);  // SXTH R0, R0
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0xFFFF8000u);
}

TEST(Cpu, UxtbRotatesSourceBeforeExtracting) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR2, 0x12345678);
  cpu.GetMemory().Write32(0, 0xE6EF1472);  // UXTB R1, R2, ROR #8
  cpu.Step();
  // ROR #8 of 0x12345678 is 0x78123456; low byte is 0x56.
  EXPECT_EQ(cpu.GetRegister(kR1), 0x00000056u);
}

TEST(Cpu, UxthRotatesSourceBeforeExtracting) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR4, 0x12345678);
  cpu.GetMemory().Write32(0, 0xE6FF3874);  // UXTH R3, R4, ROR #16
  cpu.Step();
  // ROR #16 of 0x12345678 is 0x56781234; low halfword is 0x1234.
  EXPECT_EQ(cpu.GetRegister(kR3), 0x00001234u);
}

TEST(Cpu, UxtabAddsExtendedByteToAccumulator) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x00000100);
  cpu.SetRegister(kR2, 0x000000FF);
  cpu.GetMemory().Write32(0, 0xE6E10072);  // UXTAB R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x000001FFu);
}

TEST(Cpu, UxtahAddsExtendedHalfwordToAccumulator) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR1, 0x00001000);
  cpu.SetRegister(kR2, 0x0000FFFF);
  cpu.GetMemory().Write32(0, 0xE6F10072);  // UXTAH R0, R1, R2
  cpu.Step();
  EXPECT_EQ(cpu.GetRegister(kR0), 0x00010FFFu);
}

TEST(Cpu, OtherMediaInstructionSpaceStillUnimplemented) {
  // REVSH R0, R0 -- REV/REV16's sibling (bits[27:20]=0x6F rather than
  // their 0x6B), confirming neither the Extend-family check nor the
  // REV/REV16 checks over-match neighboring, still-unimplemented media
  // encodings.
  ArmInterpreter cpu;
  cpu.GetMemory().Write32(0, 0xE6FF0FB0);  // REVSH R0, R0
  EXPECT_THROW(cpu.Step(), UnimplementedInstruction);
}

TEST(Cpu, SerializeThenDeserializeRoundTripsRegistersCpsrAndMemory) {
  ArmInterpreter cpu;
  cpu.SetRegister(kR0, 0x11111111);
  cpu.SetRegister(kR4, 0x44444444);
  cpu.SetRegister(kSP, 0x80300000);
  cpu.SetCpsr(0xF0000010);
  cpu.GetMemory().Write32(0x1000, 0xCAFEBABE);

  std::stringstream stream;
  ASSERT_TRUE(cpu.Serialize(stream));

  ArmInterpreter restored;
  restored.SetRegister(kR0, 0xDEADDEAD);  // must be overwritten by Deserialize
  ASSERT_TRUE(restored.Deserialize(stream));

  EXPECT_EQ(restored.GetRegister(kR0), 0x11111111u);
  EXPECT_EQ(restored.GetRegister(kR4), 0x44444444u);
  EXPECT_EQ(restored.GetRegister(kSP), 0x80300000u);
  EXPECT_EQ(restored.GetCpsr(), 0xF0000010u);
  EXPECT_EQ(restored.GetMemory().Read32(0x1000), 0xCAFEBABEu);
}

TEST(Cpu, DeserializeFromAnEmptyStreamFailsWithoutCrashing) {
  ArmInterpreter cpu;
  std::stringstream empty_stream;
  EXPECT_FALSE(cpu.Deserialize(empty_stream));
}
