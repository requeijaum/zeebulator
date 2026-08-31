#pragma once

#include <cstdint>
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

}  // namespace zeebulator
