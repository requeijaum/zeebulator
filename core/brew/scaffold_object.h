#pragma once

#include <cstddef>
#include <cstdint>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds a generic BREW interface object with `slot_count` vtable slots,
// every one of which just sets r0=0 and returns. Real disassembly of
// Double Dragon (PHASE8_LOG.md) shows it calling
// ISHELL_CreateInstance for real BREW classes we haven't identified
// (e.g. ClsId 0x01002001) and then unconditionally invoking specific
// vtable slots on the result (e.g. slot 33) with no way to know the
// real interface's shape without guessing. Rather than guess a real
// interface (risking silently-wrong behavior) or leave CreateInstance
// failing (the already-diagnosed "memory insufficient" dead end), this
// gives the app a harmless object that satisfies "CreateInstance
// succeeded, calling any of its methods is safe" -- used to empirically
// observe what the game does next, then replaced with real behavior for
// whichever slots turn out to matter.
uint32_t BuildGenericStubObject(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                 uint32_t object_address, size_t slot_count);

// Same as BuildGenericStubObject, but with one slot replaced by a real
// implementation. Real disassembly of Double Dragon (PHASE8_LOG.md)
// shows the object IDisplay::GetDeviceBitmap returns being used for
// exactly one meaningful thing -- a "QueryInterface"-shaped call at
// slot 2 (`obj->vtable[2](obj, clsid, &ppo)`, same calling convention
// as ISHELL_CreateInstance) -- while every other slot is unused. This
// lets that one slot get a real (if still generic) implementation
// without guessing the rest of the interface's shape.
uint32_t BuildStubObjectWithOverride(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                      uint32_t object_address, size_t slot_count,
                                      size_t override_slot, HleRuntime::HleFunction override_fn);

// Same as BuildGenericStubObject, but for a real, different real ABI
// this session found probing a second game (Peggle, `peggle.mod` --
// see PHASE8_LOG.md): real disassembly of a virtual call through a
// still-unidentified field on the shared "app context" struct (offset
// 0x2c on the struct ModRuntime's confirmed offset-0xc0 slot returns --
// see mod_runtime.h) shows the call resolving its target as `vtable_
// base + *(vtable_base + slot*4)` -- a relative offset from the
// vtable's own address, not a plain absolute function pointer -- rather
// than the ordinary `*(vtable_base + slot*4)` every other confirmed
// real BREW interface in this codebase uses. This matches ARM RVCT's
// documented "ROPI" (Read-Only Position-Independent) C++ virtual
// function table convention: vtable entries store offsets so the
// vtable itself never contains an absolute, load-address-dependent
// value. Every slot stores an HLE sentinel address encoded the same way
// (`sentinel - vtable_address`) so a caller computing the real ABI's
// relative-offset formula lands back on the real sentinel regardless of
// where `vtable_address` is placed. The real interface's identity and
// what any of its methods actually do are still unknown -- this exists
// purely so calling through it resolves to a harmless no-op instead of
// wandering into unmapped memory, the same "observe, then replace with
// real behavior" role BuildGenericStubObject plays for absolute-vtable
// interfaces.
uint32_t BuildGenericRelativeVtableStubObject(Memory& memory, HleRuntime& hle,
                                               uint32_t vtable_address,
                                               uint32_t object_address, size_t slot_count);

}  // namespace zeebulator
