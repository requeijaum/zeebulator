#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/cpu/arm_core.h"

namespace zeebulator {

// Bridges the CPU core's call-out trap mechanism (ARCHITECTURE.md 3.4) to
// C++ HLE function implementations, and provides the reverse direction
// too: calling INTO the app's own ARM code (AEEMod_Load, IModule's
// CreateInstance, an applet's HandleEvent, ...) and running until it
// returns -- exactly the way the real BREW OS drives an app through its
// lifecycle. See TASKS.md Phase 3 for the app-lifecycle contract this
// was reverse-engineered against (from the official AEEModGen.c/
// AEEAppGen.c reference sources, not guessed).
class HleRuntime {
 public:
  // Args/return value follow AAPCS: the first four 32-bit words in
  // R0-R3, the rest on the stack at [SP], [SP+4], ... HLE functions read
  // their arguments via core.GetRegister()/ReadStackArg() and set the
  // return value via core.SetRegister(kR0, ...) themselves -- this
  // matches exactly how the vtable function pointer's C signature would
  // have compiled.
  using HleFunction = std::function<void(IArmCore& core)>;

  HleRuntime(IArmCore& core, uint32_t trap_base, uint32_t trap_size);

  // Registers a function and returns the sentinel address a vtable slot
  // should point at.
  uint32_t Register(HleFunction fn);

  // Same, but attaches a human-readable label to the slot (e.g.
  // "AEECLSID_DISPLAY::slot18 SetClipRect" or
  // "ABD_RENDER_SCAFFOLD::slot33"). Purely diagnostic metadata: it does
  // NOT change dispatch, it only lets the unimplemented-slot logger name
  // what a title actually called instead of printing a bare trap index.
  // This is the Phase 9b/9c per-slot NID labeling that Phase 9d uses to
  // name the ABD wall.
  uint32_t RegisterLabeled(HleFunction fn, std::string label);

  // Returns the label attached to the trap at `sentinel_address` (the
  // value a vtable slot points at), or empty string if none/unknown.
  std::string LabelForAddress(uint32_t sentinel_address) const;

  // Returns the configured base address for call-out traps.
  uint32_t trap_base() const { return trap_base_; }

  // Calls into the app's own ARM code at `target` with up to 4 register
  // arguments, and runs the interpreter until that call returns. Returns
  // R0, the call's return value. Any UnimplementedInstruction the app
  // code hits propagates out as a C++ exception.
  uint32_t CallArmFunction(uint32_t target, uint32_t r0 = 0, uint32_t r1 = 0,
                            uint32_t r2 = 0, uint32_t r3 = 0);

  // Calls guest code from inside an HLE handler without destroying the outer
  // guest context. Memory side effects deliberately remain visible; only the
  // CPU register file/CPSR/PC is restored. Needed for synchronous guest
  // callbacks such as ISQL::Exec row callbacks.
  uint32_t CallArmFunctionPreservingContext(uint32_t target, uint32_t r0 = 0,
                                             uint32_t r1 = 0, uint32_t r2 = 0,
                                             uint32_t r3 = 0);

  // Reads a stack-passed argument (the 5th AAPCS argument and onward)
  // relative to the current SP, for use inside an HleFunction. index 0 =
  // the 5th argument overall (the first one that didn't fit in R0-R3).
  static uint32_t ReadStackArg(IArmCore& core, uint32_t index);

 private:
  void Dispatch(IArmCore& core, uint32_t address);

  IArmCore& core_;
  uint32_t trap_base_;
  // Index 0 is reserved as CallArmFunction's return sentinel and is
  // never dispatched to a real HLE function -- see Dispatch().
  std::vector<HleFunction> functions_;
  // Parallel to functions_: optional diagnostic label per slot (empty if
  // unlabeled). Never read by Dispatch's control flow -- logger only.
  std::vector<std::string> labels_;
};

}  // namespace zeebulator
