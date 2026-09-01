#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds a BREW interface object in emulated memory: a vtable array of
// HLE sentinel addresses (one per interface method, in real ABI order)
// placed at `vtable_address`, plus a single-word object header at
// `object_address` pointing at that vtable. This is the
// "OBJECT(X) { AEEVTBL(X) *pvt; ... }" pattern every real BREW interface
// follows (verified against Qualcomm's own AEEIShell.h/AEEIDisplay.h --
// see TASKS.md Phase 3). Returns `object_address`, the pointer value the
// app should receive (e.g. as its IShell*/IDisplay* argument).
uint32_t BuildInterfaceObject(Memory& memory, HleRuntime& hle,
                               uint32_t vtable_address, uint32_t object_address,
                               const std::vector<HleRuntime::HleFunction>& methods);

// Same, but labels each slot for the per-slot NID logger (Phase 9b/9c):
// slot i is registered as "<interface_name>::slot<i>[ <method_names[i]>]".
// `method_names` may be shorter than `methods` (or empty) -- missing
// entries just omit the trailing method name. Purely diagnostic: dispatch
// behavior is identical to the unlabeled overload.
uint32_t BuildInterfaceObjectLabeled(
    Memory& memory, HleRuntime& hle, uint32_t vtable_address,
    uint32_t object_address,
    const std::vector<HleRuntime::HleFunction>& methods,
    const std::string& interface_name,
    const std::vector<const char*>& method_names = {});

}  // namespace zeebulator
