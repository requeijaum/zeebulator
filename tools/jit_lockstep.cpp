// Phase 9a Phase 3: real-module lockstep differential harness.
//
// Loads a REAL .mod guest image into BOTH the reference interpreter and the
// dynarmic JIT at the same base, seeds identical registers, and single-steps
// the two cores in parallel over the ACTUAL guest instruction stream (ARM +
// Thumb, real branches/loads/stores/interworking), asserting full observable
// state (R0..R15 + CPSR) matches at every step. The interpreter is the oracle.
//
// This is intentionally NOT the full game HLE (that lives in game_probe.cpp and
// is bound to a concrete ArmInterpreter). We don't need the game to "work" to
// prove the JIT executes the same instructions as the interpreter: whenever PC
// enters the call-out trap range, we apply an IDENTICAL trivial effect to both
// cores (r0=0, return to LR) and continue. Any CPU-level divergence (a flag, a
// register, an interworking mistake, a stale block) shows up as the first
// mismatch, with the offending PC/instruction printed.
//
// Usage: zeebulator_jit_lockstep <file.mod> [max_steps]
// Exit 0 = cores stayed in lockstep to the stop condition; 1 = divergence.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "core/cpu/arm_interpreter.h"
#include "core/cpu/dynarmic_arm_core.h"
#include "core/loader/mod.h"

using namespace zeebulator;

namespace {

constexpr uint32_t kBase = 0x00100000;
constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x00010000;

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// Seed a core to the same initial state game_probe uses for a fresh call:
// mod at kBase, ROPI static base at kBase-4, a high SP, LR pointing into the
// trap range so the top-level return is observable, PC at the module entry.
void Seed(IArmCore& core, const std::vector<uint8_t>& mod, uint32_t entry) {
  core.Reset();
  LoadMod(core, mod, kBase);
  // ARM RVCT ROPI convention: static base pointer stored just below the load
  // address (see mod_runtime.h / PHASE8_LOG.md).
  core.GetMemory().Write32(kBase - 4, kBase);
  core.SetRegister(kSP, kBase + static_cast<uint32_t>(mod.size()) + 0x00200000);
  core.SetRegister(kLR, kTrapBase);
  core.SetRegister(kPC, entry);
  core.SetCallOutRange(kTrapBase, kTrapSize);
  // Identical trivial call-out on both cores: satisfy the call (r0=0) and
  // return to LR. Enough to keep both stepping the same real code path.
  core.SetCallOutHandler([](IArmCore& c, uint32_t /*addr*/) {
    c.SetRegister(kR0, 0);
    c.SetRegister(kPC, c.GetRegister(kLR));
  });
}

struct Snapshot { uint32_t r[16]; uint32_t cpsr; };
Snapshot Capture(IArmCore& c) {
  Snapshot s;
  for (int i = 0; i < 16; ++i) s.r[i] = c.GetRegister(i);
  s.cpsr = c.GetCpsr();
  return s;
}

bool Same(const Snapshot& a, const Snapshot& b, int& bad_reg, bool& cpsr_bad) {
  bad_reg = -1; cpsr_bad = false;
  for (int i = 0; i < 16; ++i) if (a.r[i] != b.r[i]) { bad_reg = i; return false; }
  // Compare only the architecturally-defined NZCVQ + T + I/F flags region we
  // both model; mode bits below aren't exercised by user code here.
  const uint32_t mask = 0xF80000DF;  // NZCVQ, IF, T, mode
  if ((a.cpsr & mask) != (b.cpsr & mask)) { cpsr_bad = true; return false; }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <file.mod> [max_steps]\n", argv[0]); return 2; }
  const uint64_t max_steps = (argc >= 3) ? std::strtoull(argv[2], nullptr, 0) : 200000ull;
  std::vector<uint8_t> mod = ReadFile(argv[1]);
  const uint32_t entry = kBase;  // .mod is flat PIC, entry at base

  ArmInterpreter interp;
  DynarmicArmCore jit;
  Seed(interp, mod, entry);
  Seed(jit, mod, entry);

  uint64_t steps = 0, callouts = 0;
  uint32_t last_pc = entry;
  for (; steps < max_steps; ++steps) {
    // Compare BEFORE each step so a divergence is attributed to the prior
    // instruction, and print the PC/opcode about to run.
    Snapshot a = Capture(interp), b = Capture(jit);
    int bad_reg; bool cpsr_bad;
    if (!Same(a, b, bad_reg, cpsr_bad)) {
      uint32_t opcode = interp.GetMemory().Read32(last_pc);
      std::fprintf(stderr,
          "DIVERGENCE at step %llu, after PC=0x%08x (opcode=0x%08x)\n",
          (unsigned long long)steps, last_pc, opcode);
      if (bad_reg >= 0)
        std::fprintf(stderr, "  R%d: interp=0x%08x jit=0x%08x\n",
                     bad_reg, a.r[bad_reg], b.r[bad_reg]);
      if (cpsr_bad)
        std::fprintf(stderr, "  CPSR: interp=0x%08x jit=0x%08x\n", a.cpsr, b.cpsr);
      std::fprintf(stderr, "  interp PC=0x%08x  jit PC=0x%08x\n", a.r[15], b.r[15]);
      return 1;
    }
    uint32_t pc = interp.GetRegister(kPC);
    // Top-level return (guest branched to LR==trap base and the handler set
    // PC back to trap base repeatedly) — treat a settle at trap base as done.
    if (pc == kTrapBase && steps > 0) {
      std::fprintf(stderr, "reached top-level return (PC=trap base) at step %llu\n",
                   (unsigned long long)steps);
      break;
    }
    if (pc >= kTrapBase && pc < kTrapBase + kTrapSize) ++callouts;
    last_pc = pc;
    interp.Step();
    jit.Step();
  }

  std::fprintf(stderr,
      "LOCKSTEP OK: %llu steps, %llu call-outs, no divergence (interp==jit)\n",
      (unsigned long long)steps, (unsigned long long)callouts);
  return 0;
}
