#pragma once

#include <cstdint>

namespace zeebulator {

// A small, self-authored 5x7 monospace bitmap font (uppercase Latin
// letters, digits, and space) -- not extracted from any real game or
// copied from a third-party font table, consistent with this project's
// clean-room policy (CONTRIBUTING.md). Covers enough of printable ASCII
// to render real BREW HUD/menu text legibly instead of the placeholder
// solid blocks IDisplayHle::DrawText used before (see PHASE8_LOG.md).
//
// Each glyph is 7 rows of 5 bits; bit 4 is the leftmost column, bit 0
// the rightmost. Characters outside the covered set (lowercase,
// punctuation, ...) fall back to a small filled box so they're still
// visible rather than invisible.
//
// Returns a pointer to 7 row bytes for `c`, or nullptr for a blank
// glyph (used for the ASCII space and any other unmapped whitespace).
const uint8_t* GetGlyph5x7(char c);

}  // namespace zeebulator
