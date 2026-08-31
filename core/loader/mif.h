#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zeebulator {

// Extracts human-readable strings from a MIF (Module Information File).
//
// MIF's full binary structure (resource tables, class IDs, privilege
// bits) is NOT understood yet -- there's no public spec, and reverse
// engineering it fully is deferred (see TASKS.md Phase 2). What IS
// confirmed, cross-checked against real SDK samples and a real
// commercial game's MIF: human-readable metadata (app name, publisher,
// version, ...) is stored as UTF-16LE strings, each prefixed with an
// 0xFFFE BOM marker, ending at either a null code unit or the next BOM
// (strings can sit back-to-back with no separator). That's independently
// extractable without solving the rest of the format, which is all a
// game-library UI actually needs.
struct MifString {
  uint32_t offset;
  std::string text;
};

// Scans for BOM-prefixed UTF-16LE strings. Sequences containing any
// non-printable-ASCII code unit are dropped rather than returned with
// placeholder characters -- in practice these come from the BOM byte
// pattern (0xFF 0xFE) occurring by coincidence inside binary icon data
// elsewhere in the file, not from real text.
std::vector<MifString> ExtractMifStrings(const uint8_t* data, size_t size);

}  // namespace zeebulator
