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
