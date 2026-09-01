#pragma once

#include <cstdint>
#include <string>

namespace zeebulator {

// Declarative BREW class-ID (AEECLSID) name table, Vita3K/Ryujinx style.
//
// Why this exists: every BREW interface a title asks for via
// ISHELL_CreateInstance is identified by a 32-bit AEECLSID. Historically
// this project scattered those magic numbers across game_probe.cpp as
// hand-written `RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, ...)`
// comments. That means (a) a raw clsid in a log is opaque, and (b) the
// per-slot unimplemented-method logger prints a bare trap index with no
// idea WHICH interface/object it belongs to.
//
// This table turns the known class IDs into DATA. It is *metadata only* --
// it changes NO dispatch behavior; it only lets the CreateInstance logger
// and the per-slot logger name what they see, which is what drives
// "implement the next real handler from real demand" (Phase 9b/9c) and,
// concretely, names which slot of which object the ABD wall hammers
// (Phase 9d).
//
// Clean-room note: these AEECLSID values are the ones this project already
// recovered by tracing real CreateInstance call sites / disassembly (see
// tools/game_probe.cpp history and research/sources/*). The symbolic names
// mirror the public AEECLSID_* macro spellings, which are interface
// identifiers, not copied implementation.

// Returns a stable symbolic name for a known BREW class id (e.g.
// "AEECLSID_DISPLAY"), or nullptr if the id is not in the table. The
// returned pointer is to a string literal with static lifetime.
const char* KnownClassName(uint32_t clsid);

// Convenience: "AEECLSID_DISPLAY (0x01001001)" for a known id, or
// "UNKNOWN (0x........)" for an unrecognized one. Handy for one-line logs.
std::string DescribeClsid(uint32_t clsid);

}  // namespace zeebulator
