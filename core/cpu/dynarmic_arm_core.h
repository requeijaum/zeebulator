#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "core/cpu/arm_core.h"
#include "core/cpu/arm_interpreter.h"
#include "core/memory/memory.h"

// Phase 9a, Phase 1 (ROADMAP.md 9a): DynarmicArmCore now hosts a REAL
// dynarmic A32 JIT (yuzu-mirror/dynarmic, ArchVersion v6K = ARM1136J-S) as
// an alternative IArmCore. The interpreter stays as the reference oracle,
// still held here so a differential harness can lockstep-compare the two.
//
// Correctness-first design (see research/sources/2026-09-01_dynarmic-jit-design.md,
// "central risk = call-out trap vs JIT blocks"): we drive the JIT one
// instruction at a time via Jit::Step() and perform the call-out trap check
// OURSELVES before each fetch, byte-for-byte mirroring ArmInterpreter::Step
// (arm_interpreter.cpp:1005-1013) and ::Run (:1217-1228). This makes the
// trap a pure control-flow decision in our own code, never a JIT block
// boundary concern, and yields exact lockstep with the interpreter. Turning
// per-instruction stepping into whole-block Jit::Run() execution is a
// deliberate Phase 4 (performance) task, gated behind the differential
// harness proving the JIT matches first.

namespace Dynarmic::A32 {
class Jit;
}

namespace zeebulator {

class DynarmicArmCore : public IArmCore {
 public:
  DynarmicArmCore();
  ~DynarmicArmCore() override;

  void Reset() override;
  void Step() override;
  uint64_t Run(uint64_t max_instructions) override;

  uint32_t GetRegister(int index) const override;
  void SetRegister(int index, uint32_t value) override;

  uint32_t GetCpsr() const override;
  void SetCpsr(uint32_t value) override;

  Memory& GetMemory() override { return memory_; }

  void SetCallOutRange(uint32_t base, uint32_t size) override;
  void SetCallOutHandler(CallOutHandler handler) override;

  // Exposed so a differential phase can reach the reference interpreter
  // directly (lockstep compare) without going through the IArmCore surface.
  ArmInterpreter& reference_interpreter() { return interpreter_; }

 private:
  // dynarmic UserCallbacks bridging the JIT to our sparse Memory. Defined in
  // the .cpp so this header doesn't pull in the dynarmic interface headers.
  struct Callbacks;

  // True when `addr` falls inside the currently-armed call-out trap range.
  bool IsCallOutAddress(uint32_t addr) const {
    return call_out_size_ != 0 && addr >= call_out_base_ &&
           addr < call_out_base_ + call_out_size_;
  }

  Memory memory_;
  ArmInterpreter interpreter_;  // reference oracle (unused for execution here)

  std::unique_ptr<Callbacks> callbacks_;
  std::unique_ptr<Dynarmic::A32::Jit> jit_;

  uint32_t call_out_base_ = 0;
  uint32_t call_out_size_ = 0;
  CallOutHandler call_out_handler_;
};

// CPU-backend selection. kInterpreter is the always-safe reference core;
// kDynarmic selects the DynarmicArmCore adapter (real JIT since Phase 1).
// Chosen at runtime so any regression is one flag away from revert
// (design doc: ZEEB_CPU=interp|jit).
enum class CpuBackend { kInterpreter, kDynarmic };

// Reads the ZEEB_CPU environment variable: "jit"/"dynarmic" -> kDynarmic,
// anything else (incl. unset) -> kInterpreter. Kept as a free function so
// harnesses and tests resolve the backend identically.
CpuBackend SelectCpuBackendFromEnv();

// Constructs the requested IArmCore implementation.
std::unique_ptr<IArmCore> MakeArmCore(CpuBackend backend);

}  // namespace zeebulator
