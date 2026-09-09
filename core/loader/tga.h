#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

// Decodes a Truevision TGA (Targa) file: uncompressed (type 2) or
// RLE-compressed (type 10) truecolor, 24 or 32 bits per pixel -- the
// format Pac-Mania's loose sibling assets (`bg_tex.tga`, `load_tex.tga`,
// `logo_tex.tga`, `namco_tex.tga`) turn out to use for the port's own
// full-screen backgrounds (confirmed directly against the real file
// bytes: signature-less TGA has no magic number, but the 18-byte header
// field `image_type` at offset 2 reads 2 for every sample found here).
// Palette (type 1/9) and black-and-white (type 3/11) TGA are out of
// scope -- no real sample uses them; returns nullopt for anything else.
//
// Returns RGBA8, row-major, top-down (row 0 = top of image), 4
// bytes/pixel (alpha forced to 255 for a 24bpp source) via
// `out_width`/`out_height` -- the same layout `DecodePng` and
// `DecodeAtitc` already use, so callers don't need a format-specific
// code path once decoded.
std::optional<std::vector<uint8_t>> DecodeTga(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height);

}  // namespace zeebulator
