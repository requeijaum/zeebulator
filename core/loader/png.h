#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

// Decodes a standard PNG file (the real format `data.bar`'s own image
// entries turn out to use for this project's own real UI/splash assets --
// found live tracing Alien Breaker Deluxe's real ISHELL_LoadResDataEx
// calls, TASKS.md Phase 8: several real resource IDs this project
// already resolves correctly via `BarArchive::Find` return real bytes
// starting with the real PNG signature `\x89PNG\r\n\x1a\n`, not this
// project's own already-understood ATITC format -- confirmed directly
// against the real bytes, not guessed. Every real sample found so far
// is 8-bit, non-interlaced, non-palette (`color_type` 2/RGB or 6/RGBA)
// -- that's what this decoder supports; anything else (palette,
// interlaced, <8-bit) returns nullopt rather than silently mis-decode.
//
// Real zlib/DEFLATE decompression (the real PNG `IDAT` payload) reuses
// this project's own already-vendored zlib (see the top-level
// CMakeLists.txt, already linked in for GGZ's own real gzip streams) --
// no new compression code, only the real PNG-specific scanline filter
// reconstruction (the one real part of the format zlib doesn't already
// handle), implemented directly from the real PNG spec's own well-known
// filter algorithms (None/Sub/Up/Average/Paeth).
//
// Returns RGBA8, row-major, 4 bytes/pixel (opaque, alpha=255, for a
// real RGB-only source) via `out_width`/`out_height` -- the same real
// layout `IDisplayHle::BlitRgba` and `DecodeAtitc` already use, so
// callers don't need a real format-specific code path once decoded.
std::optional<std::vector<uint8_t>> DecodePng(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height);

}  // namespace zeebulator
