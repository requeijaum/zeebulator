#include "core/loader/obm1.h"

#include <stdexcept>

namespace zeebulator {

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// RGB565 -> RGB888, matching the channel widths (5/6/5 bits) real BREW
// GLES devices of this era used for 16-bit color.
void Rgb565To888(uint16_t v, uint8_t* out) {
  uint32_t r5 = (v >> 11) & 0x1F;
  uint32_t g6 = (v >> 5) & 0x3F;
  uint32_t b5 = v & 0x1F;
  out[0] = static_cast<uint8_t>(r5 * 255 / 31);
  out[1] = static_cast<uint8_t>(g6 * 255 / 63);
  out[2] = static_cast<uint8_t>(b5 * 255 / 31);
}

}  // namespace

DecodedImage Obm1Image::Decode(const std::vector<uint8_t>& data) {
  if (data.size() < 8) throw std::runtime_error("OBM1: file too small for a header");
  if (data[0] != 'O' || data[1] != 'I') throw std::runtime_error("OBM1: bad magic");
  if (data[2] != 0x04) {
    throw std::runtime_error("OBM1: unexpected header byte 2 (only 0x04 confirmed)");
  }
  uint32_t bpp = data[3];
  if (bpp != 4 && bpp != 8) {
    throw std::runtime_error("OBM1: unsupported bits-per-pixel (only 4 and 8 confirmed)");
  }

  DecodedImage image;
  image.width = ReadU16LE(&data[4]);
  image.height = ReadU16LE(&data[6]);

  uint32_t palette_entries = 1u << bpp;
  uint64_t palette_bytes = static_cast<uint64_t>(palette_entries) * 2;
  uint64_t header_size = 8 + palette_bytes;
  if (data.size() < header_size) throw std::runtime_error("OBM1: truncated palette");

  std::vector<uint8_t> palette_rgb(static_cast<size_t>(palette_entries) * 3);
  for (uint32_t i = 0; i < palette_entries; ++i) {
    uint16_t v = ReadU16LE(&data[8 + i * 2]);
    Rgb565To888(v, &palette_rgb[static_cast<size_t>(i) * 3]);
  }

  uint64_t pixel_count = static_cast<uint64_t>(image.width) * image.height;
  uint64_t pixel_data_bytes = (pixel_count * bpp + 7) / 8;
  if (data.size() < header_size + pixel_data_bytes) {
    throw std::runtime_error("OBM1: truncated pixel data");
  }

  const uint8_t* pixel_data = &data[static_cast<size_t>(header_size)];
  uint32_t pixels_per_byte = 8 / bpp;
  uint32_t mask = (1u << bpp) - 1;

  image.rgb.resize(static_cast<size_t>(pixel_count) * 3);
  image.alpha.resize(static_cast<size_t>(pixel_count));
  for (uint64_t i = 0; i < pixel_count; ++i) {
    uint8_t byte = pixel_data[i / pixels_per_byte];
    uint32_t slot_in_byte = static_cast<uint32_t>(i % pixels_per_byte);
    uint32_t shift = 8 - bpp - slot_in_byte * bpp;  // most-significant-first
    uint32_t index = (byte >> shift) & mask;
    const uint8_t* color = &palette_rgb[static_cast<size_t>(index) * 3];
    image.rgb[static_cast<size_t>(i) * 3 + 0] = color[0];
    image.rgb[static_cast<size_t>(i) * 3 + 1] = color[1];
    image.rgb[static_cast<size_t>(i) * 3 + 2] = color[2];
    // Real color-key convention (see this class's own doc comment):
    // palette index 0 is always the real transparency signal.
    image.alpha[static_cast<size_t>(i)] = (index == 0) ? 0 : 255;
  }

  return image;
}

}  // namespace zeebulator
