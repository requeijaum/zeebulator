#pragma once

#include <memory>

#include "core/cpu/arm_core.h"
#include "core/cpu/arm_interpreter.h"

namespace zeebulator {

// Phase 9a, Phase 0 (ROADMAP.md 9a): skeleton adapter that will eventually
// host a dynarmic A32 JIT as an alternative IArmCore. In this phase it owns
// an ArmInterpreter internally and forwards EVERY IArmCore method to it, so
// selecting it changes nothing observable — the point is to establish the
// seam (a second IArmCore impl behind a runtime flag) with the 446-test suite
// staying green before any real JIT block executes.
//
// Why forward instead of subclass: keeping ArmInterpreter as a held member
// (not a base) lets a later phase run the JIT and the interpreter side by
// side for lockstep differential testing (design doc: 3-layer harness) without
// reshaping this class. When the real dynarmic backend lands, the interpreter
// stays as the reference oracle reachable from here.
class DynarmicArmCore : public IArmCore {
 public:
  DynarmicArmCore();

  void Reset() override;
  void Step() override;
  uint64_t Run(uint64_t max_instructions) override;

  uint32_t GetRegister(int index) const override;
  void SetRegister(int index, uint32_t value) override;

  uint32_t GetCpsr() const override;
  void SetCpsr(uint32_t value) override;

  Memory& GetMemory() override;

  void SetCallOutRange(uint32_t base, uint32_t size) override;
  void SetCallOutHandler(CallOutHandler handler) override;

  // Exposed so a later differential phase can reach the reference interpreter
  // directly (lockstep compare) without going through the IArmCore surface.
  ArmInterpreter& reference_interpreter() { return interpreter_; }

 private:
  ArmInterpreter interpreter_;
};

// CPU-backend selection. kInterpreter is the always-safe reference core;
// kDynarmic selects the DynarmicArmCore adapter (interpreter-forwarding in
// Phase 0). Chosen at runtime so any regression is one flag away from revert
// (design doc: ZEEB_CPU=interp|jit).
enum class CpuBackend { kInterpreter, kDynarmic };

// Reads the ZEEB_CPU environment variable: "jit"/"dynarmic" -> kDynarmic,
// anything else (incl. unset) -> kInterpreter. Kept as a free function so
// harnesses and tests resolve the backend identically.
CpuBackend SelectCpuBackendFromEnv();

// Constructs the requested IArmCore implementation.
std::unique_ptr<IArmCore> MakeArmCore(CpuBackend backend);

}  // namespace zeebulator
