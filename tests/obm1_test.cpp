// Tests use small, synthetic, hand-built ".obm1" byte arrays -- the real
// format was reverse-engineered against Double Dragon's real `data.ggz`
// (TASKS.md Phase 8), but real game assets are never committed to this
// repo (see CONTRIBUTING.md's clean-room policy).

#include "core/loader/obm1.h"

#include <gtest/gtest.h>

using zeebulator::DecodedImage;
using zeebulator::Obm1Image;

namespace {

void WriteU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// Builds a well-formed synthetic OBM1 image: `bpp` bits/pixel, a palette
// with `palette_entries` RGB565 entries (defaulting every unset entry to
// 0), and `pixel_indices.size()` pixels packed most-significant-bits-
// first, row-major per `width`/`height`.
std::vector<uint8_t> BuildObm1(uint16_t width, uint16_t height, uint8_t bpp,
                                const std::vector<uint16_t>& palette,
                                const std::vector<uint8_t>& pixel_indices) {
  std::vector<uint8_t> out;
  out.push_back('O');
  out.push_back('I');
  out.push_back(0x04);
  out.push_back(bpp);
  WriteU16LE(out, width);
  WriteU16LE(out, height);

  uint32_t palette_entries = 1u << bpp;
  for (uint32_t i = 0; i < palette_entries; ++i) {
    WriteU16LE(out, i < palette.size() ? palette[i] : 0);
  }

  uint32_t pixels_per_byte = 8 / bpp;
  uint32_t mask = (1u << bpp) - 1;
  for (size_t i = 0; i < pixel_indices.size(); i += pixels_per_byte) {
    uint8_t byte = 0;
    for (uint32_t slot = 0; slot < pixels_per_byte && i + slot < pixel_indices.size(); ++slot) {
      uint32_t shift = 8 - bpp - slot * bpp;
      byte |= static_cast<uint8_t>((pixel_indices[i + slot] & mask) << shift);
    }
    out.push_back(byte);
  }
  return out;
}

}  // namespace

TEST(Obm1, Decodes2x2FourBppImageWithCorrectColorsAndPixelOrder) {
  // Palette entry 0 = pure RGB565 red, entry 1 = pure RGB565 green.
  std::vector<uint16_t> palette(16, 0);
  palette[0] = 0xF800;  // R=31,G=0,B=0 -> (255,0,0)
  palette[1] = 0x07E0;  // R=0,G=63,B=0 -> (0,255,0)
  // Row-major: (red, green) / (green, red)
  auto data = BuildObm1(2, 2, 4, palette, {0, 1, 1, 0});

  DecodedImage image = Obm1Image::Decode(data);

  EXPECT_EQ(image.width, 2u);
  EXPECT_EQ(image.height, 2u);
  ASSERT_EQ(image.rgb.size(), 2u * 2u * 3u);
  auto PixelAt = [&](int x, int y) {
    size_t base = (static_cast<size_t>(y) * 2 + x) * 3;
    return std::vector<uint8_t>(image.rgb.begin() + base, image.rgb.begin() + base + 3);
  };
  EXPECT_EQ(PixelAt(0, 0), (std::vector<uint8_t>{255, 0, 0}));
  EXPECT_EQ(PixelAt(1, 0), (std::vector<uint8_t>{0, 255, 0}));
  EXPECT_EQ(PixelAt(0, 1), (std::vector<uint8_t>{0, 255, 0}));
  EXPECT_EQ(PixelAt(1, 1), (std::vector<uint8_t>{255, 0, 0}));
}

TEST(Obm1, Decodes1x2EightBppImageWithCorrectColors) {
  std::vector<uint16_t> palette(256, 0);
  palette[5] = 0x001F;    // pure blue -> (0,0,255)
  palette[200] = 0xFFFF;  // white -> (255,255,255)
  auto data = BuildObm1(1, 2, 8, palette, {5, 200});

  DecodedImage image = Obm1Image::Decode(data);

  ASSERT_EQ(image.rgb.size(), 1u * 2u * 3u);
  EXPECT_EQ((std::vector<uint8_t>(image.rgb.begin(), image.rgb.begin() + 3)),
            (std::vector<uint8_t>{0, 0, 255}));
  EXPECT_EQ((std::vector<uint8_t>(image.rgb.begin() + 3, image.rgb.begin() + 6)),
            (std::vector<uint8_t>{255, 255, 255}));
}

TEST(Obm1, PaletteIndexZeroIsAlwaysTransparentRegardlessOfItsStoredColor) {
  // Real convention (see Obm1Image::Decode's own doc comment): index 0
  // is always alpha=0, even though its real, always-observed stored
  // color is a specific magenta (0xF83E) -- this test uses a
  // deliberately different color at index 0 to prove the rule keys off
  // the *index*, not a color match.
  std::vector<uint16_t> palette(16, 0);
  palette[0] = 0xF800;  // pure red, not the real magenta value
  palette[1] = 0x07E0;  // pure green
  auto data = BuildObm1(2, 1, 4, palette, {0, 1});

  DecodedImage image = Obm1Image::Decode(data);

  ASSERT_EQ(image.alpha.size(), 2u);
  EXPECT_EQ(image.alpha[0], 0) << "index 0 is always transparent";
  EXPECT_EQ(image.alpha[1], 255) << "every other index is always opaque";
  // The real color still decodes normally into rgb -- alpha is a
  // separate, additional signal, not a replacement for it.
  EXPECT_EQ((std::vector<uint8_t>(image.rgb.begin(), image.rgb.begin() + 3)),
            (std::vector<uint8_t>{255, 0, 0}));
}

TEST(Obm1, RejectsFileTooSmallForAHeader) {
  std::vector<uint8_t> data = {'O', 'I', 0x04, 0x04, 0, 0};  // only 6 bytes
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}

TEST(Obm1, RejectsBadMagic) {
  auto data = BuildObm1(1, 1, 4, {0}, {0});
  data[0] = 'X';
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}

TEST(Obm1, RejectsUnexpectedHeaderByteTwo) {
  auto data = BuildObm1(1, 1, 4, {0}, {0});
  data[2] = 0x05;  // only 0x04 confirmed across every real sample seen
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}

TEST(Obm1, RejectsUnsupportedBitsPerPixel) {
  // BuildObm1 itself only handles the two confirmed depths cleanly, so
  // hand-construct an otherwise-well-formed 2bpp-labeled header instead.
  std::vector<uint8_t> data = {'O', 'I', 0x04, 0x02, 1, 0, 1, 0};
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}

TEST(Obm1, RejectsTruncatedPalette) {
  auto data = BuildObm1(1, 1, 4, {0}, {0});
  data.resize(data.size() - 5);  // cut into the palette
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}

TEST(Obm1, RejectsTruncatedPixelData) {
  auto data = BuildObm1(4, 4, 4, {0}, std::vector<uint8_t>(16, 0));
  data.pop_back();  // drop the last pixel-data byte
  EXPECT_THROW(Obm1Image::Decode(data), std::runtime_error);
}
