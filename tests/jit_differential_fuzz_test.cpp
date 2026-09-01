#include "core/cpu/arm_interpreter.h"
#include "core/cpu/dynarmic_arm_core.h"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

// Phase 9a Phase 2 (ROADMAP.md 9a): randomized differential fuzzer. For each
// generated instruction we seed the interpreter and the real dynarmic JIT with
// an IDENTICAL random register/CPSR state, execute exactly one instruction on
// each, and assert their full observable state (R0..R15, CPSR flags) matches.
// The interpreter is the reference oracle; the JIT must not diverge. Any
// encoding the interpreter itself rejects (UnimplementedInstruction) is skipped
// — that's an interpreter coverage gap, not a JIT bug, and is out of scope for
// "JIT == interpreter". Divergences that DO fire here are the Phase 2 catalogue
// (see research/sources/2026-09-01_jit-differential.md).

using zeebulator::ArmInterpreter;
using zeebulator::DynarmicArmCore;
using zeebulator::IArmCore;
using zeebulator::kCpsrC;
using zeebulator::kCpsrN;
using zeebulator::kCpsrV;
using zeebulator::kCpsrZ;
using zeebulator::kPC;

namespace {

constexpr uint32_t kCodeBase = 0x1000;

// Only the four condition flags are architecturally observable across a single
// data-processing/arithmetic step in a way both cores must agree on. (Mode/
// state bits below bit 5 aren't exercised by these encodings.)
uint32_t FlagsOnly(uint32_t cpsr) {
  const uint32_t mask = (1u << kCpsrN) | (1u << kCpsrZ) | (1u << kCpsrC) | (1u << kCpsrV);
  return cpsr & mask;
}

struct StepResult {
  bool interp_threw = false;
  std::array<uint32_t, 16> regs{};
  uint32_t flags = 0;
};

StepResult RunOne(IArmCore& core, uint32_t instr, const std::array<uint32_t, 16>& seed,
                  uint32_t seed_cpsr, bool& threw_out) {
  core.Reset();
  // Seed registers R0..R12 with the random state; keep PC at the code slot and
  // a sane SP/LR so stores/loads land in mapped scratch memory.
  for (int r = 0; r <= 12; ++r) core.SetRegister(r, seed[r]);
  core.SetRegister(13, 0x2000);  // SP -> scratch
  core.SetRegister(14, seed[14]);
  core.SetRegister(kPC, kCodeBase);
  core.SetCpsr(seed_cpsr & 0xF0000000u);  // only the NZCV flags, ARM state
  core.GetMemory().Write32(kCodeBase, instr);
  // Direct code poke: tell any block-caching core the code changed so it
  // doesn't re-run a stale JIT block for this address (real SMC through the
  // guest goes via MemoryWrite callbacks; harness pokes need this hook).
  core.NotifyCodeChanged(kCodeBase, 4);
  StepResult out;
  try {
    core.Step();
  } catch (const zeebulator::UnimplementedInstruction&) {
    threw_out = true;
    return out;
  }
  for (int r = 0; r <= 15; ++r) out.regs[r] = core.GetRegister(r);
  out.flags = FlagsOnly(core.GetCpsr());
  return out;
}

// A curated set of instruction TEMPLATES with placeholder register/immediate
// fields the fuzzer fills in. Each is a real encoding the interpreter is
// documented to implement. cond is fixed to AL (0xE) so the instruction always
// executes; flag inputs still matter for ADC/SBC/shifter-carry.
struct Template {
  const char* name;
  uint32_t base;   // encoding with Rd/Rn/Rm/imm holes to be OR'd in
  bool use_rd, use_rn, use_rm, use_imm8, set_s;
};

}  // namespace

TEST(JitDifferentialFuzz, ArmArithmeticAndLogicMatchInterpreter) {
  // Data-processing opcodes (bits 24-21), register form (I=0) and immediate
  // form. AND EOR SUB RSB ADD ADC SBC RSC TST TEQ CMP CMN ORR MOV BIC MVN.
  // Register form: cond(1110) 00 0 opcode S Rn Rd 00000000 Rm
  // Immediate form: cond(1110) 00 1 opcode S Rn Rd imm12
  std::vector<uint32_t> opcodes = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                                   0xC, 0xE};  // AND..RSC, ORR, BIC (skip TST/TEQ/CMP/CMN Rd-less handled below)
  std::vector<uint32_t> test_opcodes = {0x8, 0x9, 0xA, 0xB};  // TST TEQ CMP CMN

  std::mt19937 rng(0xC0FFEE);
  auto rnd = [&]() { return rng(); };

  ArmInterpreter interp;
  DynarmicArmCore jit;

  int compared = 0, skipped = 0;
  auto do_case = [&](uint32_t instr) {
    std::array<uint32_t, 16> seed{};
    for (auto& v : seed) v = rnd();
    uint32_t seed_cpsr = rnd();
    bool it = false, jt = false;
    StepResult ri = RunOne(interp, instr, seed, seed_cpsr, it);
    StepResult rj = RunOne(jit, instr, seed, seed_cpsr, jt);
    if (it) { ++skipped; return; }  // interpreter can't do it -> out of scope
    ++compared;
    for (int r = 0; r <= 15; ++r) {
      if (r == kPC) continue;  // PC advance semantics compared via flags+regs elsewhere
      ASSERT_EQ(ri.regs[r], rj.regs[r])
          << "instr=0x" << std::hex << instr << " R" << std::dec << r << " diverged";
    }
    ASSERT_EQ(ri.flags, rj.flags)
        << "instr=0x" << std::hex << instr << " NZCV diverged";
  };

  for (int iter = 0; iter < 4000; ++iter) {
    uint32_t rd = rnd() & 0xF, rn = rnd() & 0xF, rm = rnd() & 0xF;
    // Avoid PC (R15) as operand/dest so we compare pure ALU behaviour, not
    // the pipeline/interworking PC-read rules (those are covered by the
    // dedicated Thumb/branch tests already).
    if (rd == 15) rd = 0; if (rn == 15) rn = 1; if (rm == 15) rm = 2;
    uint32_t s = (rnd() & 1) << 20;
    uint32_t opc = opcodes[rnd() % opcodes.size()];
    // register form
    do_case(0xE0000000u | (opc << 21) | s | (rn << 16) | (rd << 12) | rm);
    // immediate form
    uint32_t imm = rnd() & 0xFF;
    uint32_t rot = (rnd() % 16) << 8;
    do_case(0xE2000000u | (opc << 21) | s | (rn << 16) | (rd << 12) | rot | imm);
    // test opcodes (S forced on, Rd ignored)
    uint32_t topc = test_opcodes[rnd() % test_opcodes.size()];
    do_case(0xE0100000u | (topc << 21) | (rn << 16) | rm);
  }
  // Sanity: the fuzzer actually exercised the JIT (not all-skipped).
  EXPECT_GT(compared, 3000) << "compared=" << compared << " skipped=" << skipped;
}

TEST(JitDifferentialFuzz, ArmMultiplyFamilyMatchesInterpreter) {
  // MUL/MLA (bits27-21 pattern) and long multiply UMULL/UMLAL/SMULL/SMLAL.
  //   MUL:   cond 0000000 S Rd 0000 Rs 1001 Rm
  //   MLA:   cond 0000001 S Rd Rn Rs 1001 Rm
  //   UMULL: cond 0000100 S RdHi RdLo Rs 1001 Rm  (and variants 100..111)
  std::mt19937 rng(0x1234);
  auto rnd = [&]() { return rng(); };
  ArmInterpreter interp;
  DynarmicArmCore jit;
  int compared = 0, skipped = 0;

  auto do_case = [&](uint32_t instr) {
    std::array<uint32_t, 16> seed{};
    for (auto& v : seed) v = rnd();
    uint32_t seed_cpsr = rnd();
    bool it = false, jt = false;
    StepResult ri = RunOne(interp, instr, seed, seed_cpsr, it);
    StepResult rj = RunOne(jit, instr, seed, seed_cpsr, jt);
    if (it) { ++skipped; return; }
    ++compared;
    for (int r = 0; r <= 14; ++r)
      ASSERT_EQ(ri.regs[r], rj.regs[r])
          << "instr=0x" << std::hex << instr << " R" << std::dec << r;
    ASSERT_EQ(ri.flags, rj.flags) << "instr=0x" << std::hex << instr << " NZCV";
  };

  for (int iter = 0; iter < 3000; ++iter) {
    uint32_t rd = 1 + (rnd() % 12);
    uint32_t rn = 1 + (rnd() % 12);
    uint32_t rs = 1 + (rnd() % 12);
    uint32_t rm = 1 + (rnd() % 12);
    // MUL/MLA: Rd (bits19-16 here) must differ from Rm (else UNPREDICTABLE
    // pre-v6). Keep them distinct so we only test defined encodings.
    while (rm == rd) rm = 1 + (rm % 12);
    uint32_t s = (rnd() & 1) << 20;
    // MUL
    do_case(0xE0000090u | s | (rd << 16) | (rs << 8) | rm);
    // MLA
    do_case(0xE0200090u | s | (rd << 16) | (rn << 12) | (rs << 8) | rm);
    // long multiply variants (UMULL/UMLAL/SMULL/SMLAL): bits24-21 in {4,5,6,7}
    // ARM ARM constraints (else UNPREDICTABLE, cores free to differ): RdHi,
    // RdLo, Rm all distinct, and (pre-v6) RdHi/RdLo != Rm. Enforce them so the
    // generator only emits architecturally-defined encodings.
    uint32_t which = 4 + (rnd() % 4);
    uint32_t rdhi = 1 + (rnd() % 12);
    uint32_t rdlo = 1 + (rnd() % 12);
    uint32_t lm_rm = 1 + (rnd() % 12);
    uint32_t lm_rs = 1 + (rnd() % 12);
    // Reassign to guarantee distinctness of {rdhi, rdlo, rm}.
    while (rdlo == rdhi) rdlo = 1 + (rdlo % 12);
    while (lm_rm == rdhi || lm_rm == rdlo) lm_rm = 1 + (lm_rm % 12);
    do_case(0xE0000090u | (which << 21) | s | (rdhi << 16) | (rdlo << 12) | (lm_rs << 8) | lm_rm);
  }
  EXPECT_GT(compared, 2000) << "compared=" << compared << " skipped=" << skipped;
}
