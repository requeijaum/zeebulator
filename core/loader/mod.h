#pragma once

#include <cstdint>
#include <vector>

#include "core/cpu/arm_core.h"

namespace zeebulator {

// Loads a .mod image into `core`'s memory at `base_address` and points
// the entry point (PC) there.
//
// As far as verified so far, .mod is a flat, headerless,
// position-independent ARM binary: raw code+data starting directly at
// file offset 0, loadable at any base address, with no relocation table
// to process. See TASKS.md Phase 2 for how this was confirmed -- a real
// Double Dragon .mod was loaded at an arbitrary address and single-
// stepped through the actual interpreter, executing 23 real instructions
// (including two full nested calls with correct prologue/epilogue/BX-LR
// return behavior) with no header-parsing of any kind.
//
// This is intentionally thin: there's no evidence yet of a header or
// relocation table to parse. If some other title's .mod needs one, this
// is the place it gets added.
void LoadMod(IArmCore& core, const std::vector<uint8_t>& mod_data,
             uint32_t base_address);

}  // namespace zeebulator
