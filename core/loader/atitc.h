#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

enum class AtitcFormat { kRgb, kRgba };

// Decodes a raw ATITC (real Qualcomm/AMD "ATI Texture Compression",
// GL_COMPRESSED_RGB_ATI_TC / GL_COMPRESSED_RGBA_ATI_TC) compressed block
// stream into plain RGBA8, row-major, 4 bytes/pixel, `width`*`height`*4
// bytes total (opaque -- alpha=255 -- for kRgb).
//
// `data`/`size` is just the compressed block stream, NOT a real ATITC
// file: real app code reads its own ATITC_HEADER (signature/width/
// height/flags/dataOffset, all uint32 LE) itself and passes only the
// bytes after it to glCompressedTexImage2D -- see
// research/samples/.../simple_atitc/simple_atitc.c's LoadATITCFromFile,
// the real reference this project's own IFile-based loading already
// matches.
//
// The block layout and, critically, the exact (non-linear!) 4-color
// interpolation weights were NOT taken from any public spec text -- they
// were derived empirically against this project's own bundled real
// compressed/uncompressed sample pairs (texture_rgb.atitc.org/
// texture_rgb.tga and texture_rgba.atitc/texture_rgba.tga, same
// directory as simple_atitc.c) via least-squares regression over every
// real texel in both images. See TASKS.md/PHASE8_LOG.md Phase 8 for the
// derivation. Confirmed layout:
//   RGB block (8 bytes):  color0 (RGB555, LE u16), color1 (RGB565, LE
//     u16), then a 32-bit (LE) 2-bits/texel selector, LSB first, texels
//     in row-major order within the 4x4 block. Per-texel weights (not
//     evenly spaced, unlike DXT1): code0=color0, code1=(2*c0+c1)/3,
//     code2=(3*c0+5*c1)/8, code3=color1.
//   RGBA block (16 bytes): 8 bytes of explicit 4-bit-per-texel alpha
//     (DXT3-style: byte i's low nibble is texel 2i, high nibble texel
//     2i+1, both scaled by 17 to 0-255; row-major, same order as the
//     color selector) followed by an 8-byte RGB block as above.
// Real average reconstruction error against the bundled ground truth:
// ~4.6/channel for color, ~0.4 for alpha (4-bit quantization noise) --
// not exact (this is real, genuinely lossy compression) but the same
// order of magnitude as the real format's own inherent quantization,
// not a wrong-formula artifact.
//
// `width`/`height` need not be multiples of 4 -- the last partial
// row/column of blocks is decoded in full then cropped, same handling
// every other real block compression format needs. Returns nullopt if
// `data` is too small for `width`x`height` at `format`'s real bytes/
// block (8 for kRgb, 16 for kRgba) -- truncated/invalid input, not
// silently mis-decoded (same philosophy as ParseWav).
std::optional<std::vector<uint8_t>> DecodeAtitc(const uint8_t* data, size_t size, int width,
                                                 int height, AtitcFormat format);

}  // namespace zeebulator
