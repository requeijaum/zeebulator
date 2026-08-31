#include "core/loader/atitc.h"

namespace zeebulator {

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t Scale5(uint8_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t Scale6(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

struct Rgb {
  uint8_t r, g, b;
};

Rgb DecodeRgb555(uint16_t v) {
  return {Scale5(static_cast<uint8_t>((v >> 10) & 0x1F)), Scale5(static_cast<uint8_t>((v >> 5) & 0x1F)),
          Scale5(static_cast<uint8_t>(v & 0x1F))};
}

Rgb DecodeRgb565(uint16_t v) {
  return {Scale5(static_cast<uint8_t>((v >> 11) & 0x1F)), Scale6(static_cast<uint8_t>((v >> 5) & 0x3F)),
          Scale5(static_cast<uint8_t>(v & 0x1F))};
}

// See atitc.h's doc comment: empirically-derived, not evenly-spaced.
uint8_t Interpolate(int code, uint8_t c0, uint8_t c1) {
  switch (code) {
    case 0:
      return c0;
    case 1:
      return static_cast<uint8_t>((2 * c0 + c1) / 3);
    case 2:
      return static_cast<uint8_t>((3 * c0 + 5 * c1) / 8);
    case 3:
    default:
      return c1;
  }
}

}  // namespace

std::optional<std::vector<uint8_t>> DecodeAtitc(const uint8_t* data, size_t size, int width,
                                                 int height, AtitcFormat format) {
  if (width <= 0 || height <= 0) return std::nullopt;

  const int blocks_x = (width + 3) / 4;
  const int blocks_y = (height + 3) / 4;
  const size_t bytes_per_block = (format == AtitcFormat::kRgba) ? 16 : 8;
  const size_t required = static_cast<size_t>(blocks_x) * static_cast<size_t>(blocks_y) * bytes_per_block;
  if (size < required) return std::nullopt;

  std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

  for (int by = 0; by < blocks_y; ++by) {
    for (int bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = data + (static_cast<size_t>(by) * blocks_x + bx) * bytes_per_block;

      uint8_t alpha[16];
      const uint8_t* color_block = block;
      if (format == AtitcFormat::kRgba) {
        for (int i = 0; i < 8; ++i) {
          alpha[2 * i] = static_cast<uint8_t>((block[i] & 0xF) * 17);
          alpha[2 * i + 1] = static_cast<uint8_t>((block[i] >> 4) * 17);
        }
        color_block = block + 8;
      }

      const Rgb c0 = DecodeRgb555(ReadU16LE(color_block));
      const Rgb c1 = DecodeRgb565(ReadU16LE(color_block + 2));
      const uint32_t selector = ReadU32LE(color_block + 4);

      for (int p = 0; p < 16; ++p) {
        const int x = p % 4;
        const int y = p / 4;
        const int px = bx * 4 + x;
        const int py = by * 4 + y;
        if (px >= width || py >= height) continue;

        const int code = static_cast<int>((selector >> (2 * p)) & 3);
        const size_t out_off = (static_cast<size_t>(py) * width + px) * 4;
        out[out_off + 0] = Interpolate(code, c0.r, c1.r);
        out[out_off + 1] = Interpolate(code, c0.g, c1.g);
        out[out_off + 2] = Interpolate(code, c0.b, c1.b);
        out[out_off + 3] = (format == AtitcFormat::kRgba) ? alpha[p] : 255;
      }
    }
  }

  return out;
}

}  // namespace zeebulator
