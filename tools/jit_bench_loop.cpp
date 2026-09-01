// Phase 9a Phase 4: steady-state throughput microbench. A tight ARM loop runs
// for a fixed instruction budget on each core; we report best-of-N MIPS so the
// JIT's compiled steady state (not one-shot compile cost) is what's measured.
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include "core/cpu/arm_interpreter.h"
#include "core/cpu/dynarmic_arm_core.h"
using namespace zeebulator;

// Loop body at 0x100: counts down R1, does some ALU, branches back until R1==0.
//   0x100: SUBS R1,R1,#1     E2511001
//   0x104: ADD  R2,R2,R0     E0822000
//   0x108: EOR  R3,R3,R2     E0233002
//   0x10C: BNE  0x100        1AFFFFFB
static void load(IArmCore& c) {
  c.Reset();
  c.GetMemory().Write32(0x100, 0xE2511001);
  c.GetMemory().Write32(0x104, 0xE0822000);
  c.GetMemory().Write32(0x108, 0xE0233002);
  c.GetMemory().Write32(0x10C, 0x1AFFFFFB);
  // Fall-through target once R1 hits 0: jump to the call-out trap to stop.
  c.GetMemory().Write32(0x110, 0xE59FF000);  // LDR PC,[PC,#0] -> word at 0x118
  c.GetMemory().Write32(0x114, 0xE1A00000);  // NOP (MOV R0,R0) padding
  c.GetMemory().Write32(0x118, 0xF0000000);  // trap address
  c.NotifyCodeChanged(0x100, 0x20);
  c.SetCallOutRange(0xF0000000, 0x10000);
  c.SetCallOutHandler([](IArmCore& x, uint32_t) { x.SetRegister(kR1, 0); });
}
static double once(IArmCore& c, uint32_t iters, uint64_t& insns) {
  load(c);
  c.SetRegister(0, 3);
  c.SetRegister(1, iters);
  c.SetRegister(2, 0);
  c.SetRegister(3, 0);
  c.SetRegister(kPC, 0x100);
  auto t0 = std::chrono::steady_clock::now();
  uint64_t done = 0;
  // Run until the loop drains and control reaches the trap (PC parked there).
  int guard = 0;
  while (c.GetRegister(kPC) != 0xF0000000 && ++guard < 100000) {
    done += c.Run(1u << 20);
  }
  auto t1 = std::chrono::steady_clock::now();
  insns = done;
  return std::chrono::duration<double>(t1 - t0).count();
}
static double best_mips(IArmCore& c, uint32_t iters, int reps, uint64_t& insns) {
  double best = 0;
  for (int r = 0; r < reps; ++r) {
    uint64_t n = 0;
    double t = once(c, iters, n);
    double m = n / t / 1e6;
    if (m > best) { best = m; insns = n; }
  }
  return best;
}
int main(int argc, char** argv) {
  uint32_t iters = argc > 1 ? uint32_t(strtoul(argv[1], 0, 0)) : 3000000;
  int reps = argc > 2 ? atoi(argv[2]) : 5;
  ArmInterpreter I; DynarmicArmCore J;
  uint64_t ni = 0, nj = 0;
  double mi = best_mips(I, iters, reps, ni);
  double mj = best_mips(J, iters, reps, nj);
  printf("tight loop, %u iters x 4 insn, best-of-%d:\n", iters, reps);
  printf("interp: %.2f MIPS (%llu insn)\n", mi, (unsigned long long)ni);
  printf("jit   : %.2f MIPS (%llu insn)\n", mj, (unsigned long long)nj);
  printf("speedup(jit/interp) = %.2fx\n", mi > 0 ? mj / mi : 0.0);
  return 0;
}
