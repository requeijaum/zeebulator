#include "core/cpu/dynarmic_arm_core.h"

#include <cstdlib>
#include <memory>

#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::CpuBackend;
using zeebulator::DynarmicArmCore;
using zeebulator::IArmCore;
using zeebulator::MakeArmCore;
using zeebulator::SelectCpuBackendFromEnv;
using zeebulator::kPC;
using zeebulator::kR0;
using zeebulator::kR1;
using zeebulator::kR2;

namespace {

// A short, deterministic ARM program exercising data processing, a
// memory store, and a branch — enough that a real JIT backend diverging
// from the interpreter would show up in registers/flags/memory.
//   MOV R0, #5          E3A00005
//   MOV R1, #7          E3A01007
//   ADD R2, R0, R1      E0802001
//   STR R2, [R0]        E5802000  (writes 12 to address 5? -> addr = R0 = 5)
//   MOV R0, #0          E3A00000
void LoadProgram(IArmCore& core) {
  core.Reset();
  core.GetMemory().Write32(0x00, 0xE3A00005);
  core.GetMemory().Write32(0x04, 0xE3A01007);
  core.GetMemory().Write32(0x08, 0xE0802001);
  core.GetMemory().Write32(0x0C, 0xE5802000);
  core.GetMemory().Write32(0x10, 0xE3A00000);
}

}  // namespace

// Phase 0 core guarantee: the skeleton adapter is observably identical to the
// interpreter. Runs the same program through both and compares full state.
TEST(DynarmicArmCore, MatchesInterpreterLockstep) {
  ArmInterpreter interp;
  DynarmicArmCore jit;
  LoadProgram(interp);
  LoadProgram(jit);

  for (int i = 0; i < 5; ++i) {
    interp.Step();
    jit.Step();
    for (int r = 0; r <= 15; ++r) {
      EXPECT_EQ(interp.GetRegister(r), jit.GetRegister(r))
          << "register R" << r << " diverged after step " << i;
    }
    EXPECT_EQ(interp.GetCpsr(), jit.GetCpsr()) << "CPSR diverged after step " << i;
  }
  // The STR wrote R2 (=12) to address R0 (=5) at the point STR executed.
  EXPECT_EQ(interp.GetMemory().Read32(5), jit.GetMemory().Read32(5));
  EXPECT_EQ(jit.GetMemory().Read32(5), 12u);
}

TEST(DynarmicArmCore, RunMatchesInterpreter) {
  ArmInterpreter interp;
  DynarmicArmCore jit;
  LoadProgram(interp);
  LoadProgram(jit);
  uint64_t a = interp.Run(5);
  uint64_t b = jit.Run(5);
  EXPECT_EQ(a, b);
  EXPECT_EQ(interp.GetRegister(kR2), jit.GetRegister(kR2));
  EXPECT_EQ(jit.GetRegister(kR2), 12u);
}

TEST(DynarmicArmCore, SemihostingSvcMatchesInterpreter) {
  constexpr uint32_t kSvc0 = 0xEF000000;  // ARM SVC #0
  ArmInterpreter interp;
  DynarmicArmCore jit;
  for (IArmCore* core : {static_cast<IArmCore*>(&interp), static_cast<IArmCore*>(&jit)}) {
    core->Reset();
    core->GetMemory().Write32(0, kSvc0);
    core->GetMemory().Write8(0x100, 'O');
    core->GetMemory().Write8(0x101, 'K');
    core->GetMemory().Write8(0x102, 0);
    core->SetRegister(kR0, 0x04);  // SYS_WRITE0
    core->SetRegister(kR1, 0x100);
    core->SetRegister(kPC, 0);
  }
  EXPECT_NO_THROW(interp.Step());
  EXPECT_NO_THROW(jit.Step());
  EXPECT_EQ(interp.GetRegister(kR0), 0u);
  EXPECT_EQ(jit.GetRegister(kR0), 0u);
  EXPECT_EQ(interp.GetRegister(kPC), jit.GetRegister(kPC));
  EXPECT_EQ(jit.GetRegister(kPC), 4u);
}

TEST(DynarmicArmCore, Cp15CpuIdMatchesInterpreter) {
  constexpr uint32_t kMrcP15CpuId = 0xEE100F10;  // MRC p15,0,R0,c0,c0,0
  ArmInterpreter interp;
  DynarmicArmCore jit;
  for (IArmCore* core : {static_cast<IArmCore*>(&interp), static_cast<IArmCore*>(&jit)}) {
    core->Reset();
    core->GetMemory().Write32(0, kMrcP15CpuId);
    core->SetRegister(kPC, 0);
  }
  EXPECT_NO_THROW(interp.Step());
  EXPECT_NO_THROW(jit.Step());
  // MIDR medido no Zeebo real (log de boot do Linux 2.6.29-zeebo, pastebin
  // pdVwuLUV): ARM1136 r1p2. Ver o comentario em arm_interpreter.cpp.
  EXPECT_EQ(interp.GetRegister(kR0), 0x4117b362u);
  EXPECT_EQ(jit.GetRegister(kR0), interp.GetRegister(kR0));
  EXPECT_EQ(jit.GetRegister(kPC), interp.GetRegister(kPC));
}

TEST(DynarmicArmCore, UndefinedInstructionFailsLikeInterpreter) {
  constexpr uint32_t kUdf = 0xE7F000F0;  // ARMv6 UDF #0
  ArmInterpreter interp;
  DynarmicArmCore jit;
  for (IArmCore* core : {static_cast<IArmCore*>(&interp), static_cast<IArmCore*>(&jit)}) {
    core->Reset();
    core->GetMemory().Write32(0, kUdf);
    core->SetRegister(kPC, 0);
  }
  EXPECT_THROW(interp.Step(), zeebulator::UnimplementedInstruction);
  EXPECT_THROW(jit.Step(), zeebulator::UnimplementedInstruction);
}

// Central-risk feature (design doc): the call-out trap must be a pure
// control-flow decision in the adapter, never entangled with a JIT block.
// A branch into the trap range must fire the handler with the trapped
// address and stop, exactly like the interpreter — not decode/execute
// whatever bytes happen to sit there.
//   0x00: MOV R0, #1     E3A00001
//   0x04: B   0xF0000000 (link range) -> encoded relative branch to trap
// We instead just set PC directly into the trap range for determinism.
TEST(DynarmicArmCore, CallOutTrapFiresHandlerAndStopsLikeInterpreter) {
  const uint32_t kTrapBase = 0xF0000000u;
  const uint32_t kTrapSize = 0x00010000u;
  const uint32_t kTrapAddr = kTrapBase + 0x40u;

  auto run_case = [&](IArmCore& core) {
    core.Reset();
    uint32_t seen_addr = 0;
    int hits = 0;
    core.SetCallOutRange(kTrapBase, kTrapSize);
    core.SetCallOutHandler([&](IArmCore& c, uint32_t addr) {
      seen_addr = addr;
      ++hits;
      // Emulate the real HLE returning from a call-out: advance PC past it
      // so the next step resumes normal execution (here: nothing more).
      c.SetRegister(kPC, addr + 4);
    });
    core.SetRegister(kPC, kTrapAddr);
    core.Step();
    return std::pair<uint32_t, int>{seen_addr, hits};
  };

  ArmInterpreter interp;
  DynarmicArmCore jit;
  auto [interp_addr, interp_hits] = run_case(interp);
  auto [jit_addr, jit_hits] = run_case(jit);

  EXPECT_EQ(interp_hits, 1);
  EXPECT_EQ(jit_hits, 1);
  EXPECT_EQ(interp_addr, kTrapAddr);
  EXPECT_EQ(jit_addr, kTrapAddr);
  EXPECT_EQ(interp.GetRegister(kPC), jit.GetRegister(kPC));
  EXPECT_EQ(jit.GetRegister(kPC), kTrapAddr + 4);
}

// A program that runs real instructions, then branches into the trap range:
// Run() must execute the real instructions, then trap once and stop, with
// the instruction count and final state matching the interpreter.
TEST(DynarmicArmCore, RunExecutesThenTrapsInLockstep) {
  const uint32_t kTrapBase = 0xF0000000u;
  const uint32_t kTrapSize = 0x00010000u;

  auto run_case = [&](IArmCore& core) {
    core.Reset();
    // MOV R0,#5 ; MOV R1,#7 ; LDR PC,[PC,#-4]@0x0C holding the trap target.
    //   0x00 MOV R0,#5      E3A00005
    //   0x04 MOV R1,#7      E3A01007
    //   0x08 LDR PC,[PC,#-4] E51FF004  (loads word at 0x0C into PC)
    //   0x0C .word 0xF0000010 (trap target)
    core.GetMemory().Write32(0x00, 0xE3A00005);
    core.GetMemory().Write32(0x04, 0xE3A01007);
    core.GetMemory().Write32(0x08, 0xE51FF004);
    core.GetMemory().Write32(0x0C, 0xF0000010);
    int hits = 0;
    core.SetCallOutRange(kTrapBase, kTrapSize);
    core.SetCallOutHandler([&](IArmCore&, uint32_t) { ++hits; });
    core.SetRegister(kPC, 0x00);
    uint64_t executed = core.Run(100);
    return std::tuple<uint64_t, int, uint32_t>{executed, hits, core.GetRegister(kPC)};
  };

  ArmInterpreter interp;
  DynarmicArmCore jit;
  auto [i_exec, i_hits, i_pc] = run_case(interp);
  auto [j_exec, j_hits, j_pc] = run_case(jit);

  EXPECT_EQ(i_hits, 1);
  EXPECT_EQ(j_hits, 1);
  EXPECT_EQ(i_exec, j_exec) << "instruction count diverged";
  EXPECT_EQ(i_pc, j_pc) << "PC at trap diverged";
  EXPECT_EQ(j_pc, 0xF0000010u);
  EXPECT_EQ(interp.GetRegister(kR0), jit.GetRegister(kR0));
  EXPECT_EQ(interp.GetRegister(kR1), jit.GetRegister(kR1));
  EXPECT_EQ(jit.GetRegister(kR1), 7u);
}

TEST(DynarmicArmCore, FactoryReturnsRequestedBackend) {
  auto interp = MakeArmCore(CpuBackend::kInterpreter);
  auto jit = MakeArmCore(CpuBackend::kDynarmic);
  ASSERT_NE(interp, nullptr);
  ASSERT_NE(jit, nullptr);
  EXPECT_NE(dynamic_cast<ArmInterpreter*>(interp.get()), nullptr);
  EXPECT_NE(dynamic_cast<DynarmicArmCore*>(jit.get()), nullptr);
}

TEST(DynarmicArmCore, EnvSelectionDefaultsToInterpreter) {
  unsetenv("ZEEB_CPU");
  EXPECT_EQ(SelectCpuBackendFromEnv(), CpuBackend::kInterpreter);
  setenv("ZEEB_CPU", "interp", 1);
  EXPECT_EQ(SelectCpuBackendFromEnv(), CpuBackend::kInterpreter);
  setenv("ZEEB_CPU", "jit", 1);
  EXPECT_EQ(SelectCpuBackendFromEnv(), CpuBackend::kDynarmic);
  setenv("ZEEB_CPU", "dynarmic", 1);
  EXPECT_EQ(SelectCpuBackendFromEnv(), CpuBackend::kDynarmic);
  unsetenv("ZEEB_CPU");
}
