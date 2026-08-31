#include "core/loader/png.h"

#include <cstring>

#include <zlib.h>

namespace zeebulator {

namespace {

uint32_t ReadU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

uint8_t PaethPredictor(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

// Reverses the real per-scanline PNG filter in place. `bpp` is real
// bytes-per-pixel (3 for RGB, 4 for RGBA, both 8-bit -- the only real
// color types this decoder supports), needed because the real Sub/
// Average/Paeth filters reference the real *previous pixel*, not just
// the real previous byte.
void UnfilterScanline(uint8_t filter_type, uint8_t* row, const uint8_t* prev_row, int row_bytes,
                       int bpp) {
  for (int i = 0; i < row_bytes; ++i) {
    int a = (i >= bpp) ? row[i - bpp] : 0;
    int b = prev_row ? prev_row[i] : 0;
    int c = (prev_row && i >= bpp) ? prev_row[i - bpp] : 0;
    switch (filter_type) {
      case 0:  // None
        break;
      case 1:  // Sub
        row[i] = static_cast<uint8_t>(row[i] + a);
        break;
      case 2:  // Up
        row[i] = static_cast<uint8_t>(row[i] + b);
        break;
      case 3:  // Average
        row[i] = static_cast<uint8_t>(row[i] + ((a + b) / 2));
        break;
      case 4:  // Paeth
        row[i] = static_cast<uint8_t>(row[i] + PaethPredictor(a, b, c));
        break;
      default:
        break;  // unknown filter type -- leave bytes as-is, caller's own checksum-free format
    }
  }
}

}  // namespace

std::optional<std::vector<uint8_t>> DecodePng(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height) {
  if (size < 8 || std::memcmp(data, kPngSignature, 8) != 0) return std::nullopt;

  int width = 0, height = 0, bit_depth = 0, color_type = 0, interlace = 0;
  bool have_ihdr = false;
  std::vector<uint8_t> idat;

  size_t pos = 8;
  while (pos + 8 <= size) {
    uint32_t chunk_len = ReadU32BE(data + pos);
    const uint8_t* chunk_type = data + pos + 4;
    if (pos + 8 + static_cast<uint64_t>(chunk_len) + 4 > size) return std::nullopt;
    const uint8_t* chunk_data = data + pos + 8;

    if (std::memcmp(chunk_type, "IHDR", 4) == 0) {
      if (chunk_len < 13) return std::nullopt;
      width = static_cast<int>(ReadU32BE(chunk_data + 0));
      height = static_cast<int>(ReadU32BE(chunk_data + 4));
      bit_depth = chunk_data[8];
      color_type = chunk_data[9];
      uint8_t compression_method = chunk_data[10];
      uint8_t filter_method = chunk_data[11];
      interlace = chunk_data[12];
      if (compression_method != 0 || filter_method != 0) return std::nullopt;
      have_ihdr = true;
    } else if (std::memcmp(chunk_type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), chunk_data, chunk_data + chunk_len);
    } else if (std::memcmp(chunk_type, "IEND", 4) == 0) {
      break;
    }
    pos += 8 + chunk_len + 4;
  }

  if (!have_ihdr || width <= 0 || height <= 0) return std::nullopt;
  // Only the real color types/depths this project has ever actually
  // seen a real sample of (TASKS.md Phase 8) -- palette (3), <8-bit,
  // and interlaced PNGs all return nullopt rather than guess.
  if (bit_depth != 8 || interlace != 0) return std::nullopt;
  int channels;
  if (color_type == 2) {
    channels = 3;  // RGB
  } else if (color_type == 6) {
    channels = 4;  // RGBA
  } else {
    return std::nullopt;
  }
  if (idat.empty()) return std::nullopt;

  int bpp = channels;  // 8-bit depth, so bytes-per-pixel == channel count
  size_t row_bytes = static_cast<size_t>(width) * bpp;
  size_t raw_size = (row_bytes + 1) * static_cast<size_t>(height);  // +1 filter-type byte/row

  std::vector<uint8_t> raw(raw_size);
  z_stream strm{};
  if (inflateInit(&strm) != Z_OK) return std::nullopt;
  strm.next_in = const_cast<Bytef*>(idat.data());
  strm.avail_in = static_cast<uInt>(idat.size());
  strm.next_out = raw.data();
  strm.avail_out = static_cast<uInt>(raw.size());
  int ret = inflate(&strm, Z_FINISH);
  size_t produced = raw.size() - strm.avail_out;
  inflateEnd(&strm);
  if (ret != Z_STREAM_END || produced != raw_size) return std::nullopt;

  std::vector<uint8_t> pixels(row_bytes * static_cast<size_t>(height));
  const uint8_t* prev_row = nullptr;
  for (int y = 0; y < height; ++y) {
    uint8_t filter_type = raw[y * (row_bytes + 1)];
    uint8_t* row = raw.data() + y * (row_bytes + 1) + 1;
    UnfilterScanline(filter_type, row, prev_row, static_cast<int>(row_bytes), bpp);
    std::memcpy(pixels.data() + static_cast<size_t>(y) * row_bytes, row, row_bytes);
    prev_row = row;
  }

  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p) {
    if (channels == 4) {
      std::memcpy(out.data() + p * 4, pixels.data() + p * 4, 4);
    } else {
      out[p * 4 + 0] = pixels[p * 3 + 0];
      out[p * 4 + 1] = pixels[p * 3 + 1];
      out[p * 4 + 2] = pixels[p * 3 + 2];
      out[p * 4 + 3] = 255;
    }
  }

  out_width = width;
  out_height = height;
  return out;
}

}  // namespace zeebulator
