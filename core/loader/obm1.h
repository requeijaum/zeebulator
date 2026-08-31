#pragma once

#include <cstdint>
#include <vector>

namespace zeebulator {

// A decoded, palette-resolved image: width*height RGB888 triples,
// row-major, top-to-bottom, left-to-right, plus a parallel one-byte-
// per-pixel alpha channel (255 opaque, 0 transparent -- see
// `Obm1Image::Decode`'s own doc comment for the real color-key
// convention this is derived from).
struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;    // width * height * 3 bytes
  std::vector<uint8_t> alpha;  // width * height bytes
};

// Decodes a real Zeebo/BREW ".obm1" sprite/texture asset. Reverse-
// engineered from Double Dragon's real `data.ggz` (TASKS.md Phase 8):
// all 89 real assets in that archive share this exact layout, cross-
// checked byte-for-byte against each file's real size --
//
//   offset 0: magic "OI" (2 bytes)
//   offset 2: 0x04 in every real sample seen so far -- meaning
//             unconfirmed (possibly a format version); validated as a
//             constant, not assumed to vary
//   offset 3: bits per pixel -- only 4 and 8 confirmed (16- and
//             256-entry palettes respectively); every real asset in
//             this game's archive uses one or the other
//   offset 4: width, uint16 little-endian
//   offset 6: height, uint16 little-endian
//   offset 8: palette, (1 << bpp) entries, 2 bytes each, RGB565
//             little-endian (confirmed by decoding real assets to
//             images and visually verifying recognizable, correctly-
//             colored content -- a readable ASCII font sheet among
//             them, the strongest possible confirmation of both the
//             pixel unpacking order and the color channel layout)
//   after palette: packed palette-index pixel data, `bpp` bits per
//             pixel, most-significant-bits-first within each byte,
//             row-major
//
// The real transparency color-key convention is now confirmed (TASKS.md/
// PHASE8_LOG.md Phase 8): real palette index 0 is *always* the exact
// RGB565 value 0xF83E (R=31,G=1,B=30 -> real, near-pure magenta,
// (255,4,246) in RGB888) -- checked across every real OBM1 asset this
// project has observed a real width/height/palette for, zero
// exceptions, regardless of image size or bpp. Confirmed as the real
// transparency signal, not just a coincidental magenta color, by real
// disassembly of Double Dragon's own rendering setup: it enables real
// `GL_ALPHA_TEST` with `glAlphaFuncx(GL_NOTEQUAL, 0.0)` (discard exactly
// alpha==0 pixels) alongside real `GL_BLEND`/`glBlendFunc(GL_SRC_ALPHA,
// GL_ONE_MINUS_SRC_ALPHA)` before drawing these sprites -- the standard
// real GLES1.x sprite-transparency combination. `Decode` outputs
// `alpha[i] = 0` for every pixel whose palette index is 0, `255`
// otherwise; real index-0 pixels' *color* (magenta) is decoded into
// `rgb` as normal too, for callers that don't care about transparency.
class Obm1Image {
 public:
  // Throws std::runtime_error on a malformed image (bad magic, an
  // unsupported bpp, a file too short for its own declared
  // width/height/bpp).
  static DecodedImage Decode(const std::vector<uint8_t>& data);
};

}  // namespace zeebulator
