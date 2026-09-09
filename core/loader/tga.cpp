#include "core/loader/tga.h"

#include <cstring>

namespace zeebulator {

std::optional<std::vector<uint8_t>> DecodeTga(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height) {
  if (size < 18) return std::nullopt;

  const uint8_t id_length = data[0];
  const uint8_t color_map_type = data[1];
  const uint8_t image_type = data[2];
  // Only handle uncompressed (2) and RLE (10) truecolor; no color map.
  if (color_map_type != 0 || (image_type != 2 && image_type != 10)) return std::nullopt;

  const int width = data[12] | (data[13] << 8);
  const int height = data[14] | (data[15] << 8);
  const uint8_t bpp = data[16];
  const uint8_t descriptor = data[17];
  if (width <= 0 || height <= 0) return std::nullopt;
  if (bpp != 24 && bpp != 32) return std::nullopt;

  const int bytes_per_pixel = bpp / 8;
  size_t offset = 18 + id_length;
  if (offset > size) return std::nullopt;

  // Bit 5 of the descriptor byte: 0 = origin bottom-left, 1 = origin
  // top-left. Real Zeebo TGA samples found so far are all bottom-left
  // (the common TGA default), so this is exercised, not assumed.
  const bool top_down = (descriptor & 0x20) != 0;

  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * bytes_per_pixel);

  if (image_type == 2) {
    const size_t needed = pixels.size();
    if (offset + needed > size) return std::nullopt;
    std::memcpy(pixels.data(), data + offset, needed);
  } else {
    // RLE (type 10): a stream of packets. High bit of the packet header
    // set = run-length packet (header & 0x7f + 1 repeats of one pixel);
    // clear = raw packet (header + 1 literal pixels follow).
    size_t pixel_index = 0;
    const size_t total_pixels = static_cast<size_t>(width) * height;
    while (pixel_index < total_pixels) {
      if (offset >= size) return std::nullopt;
      const uint8_t header = data[offset++];
      const size_t count = (header & 0x7f) + 1;
      if (header & 0x80) {
        if (offset + bytes_per_pixel > size) return std::nullopt;
        for (size_t i = 0; i < count && pixel_index < total_pixels; ++i, ++pixel_index) {
          std::memcpy(&pixels[pixel_index * bytes_per_pixel], data + offset, bytes_per_pixel);
        }
        offset += bytes_per_pixel;
      } else {
        const size_t needed = count * bytes_per_pixel;
        if (offset + needed > size) return std::nullopt;
        for (size_t i = 0; i < count && pixel_index < total_pixels; ++i, ++pixel_index) {
          std::memcpy(&pixels[pixel_index * bytes_per_pixel], data + offset + i * bytes_per_pixel,
                      bytes_per_pixel);
        }
        offset += needed;
      }
    }
  }

  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    // TGA pixel data is stored BGR(A), not RGB(A).
    const int src_row = top_down ? y : (height - 1 - y);
    for (int x = 0; x < width; ++x) {
      const size_t src = (static_cast<size_t>(src_row) * width + x) * bytes_per_pixel;
      const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
      rgba[dst + 0] = pixels[src + 2];  // R <- B
      rgba[dst + 1] = pixels[src + 1];  // G
      rgba[dst + 2] = pixels[src + 0];  // B <- R
      rgba[dst + 3] = (bytes_per_pixel == 4) ? pixels[src + 3] : 255;
    }
  }

  out_width = width;
  out_height = height;
  return rgba;
}

}  // namespace zeebulator
