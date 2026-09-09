#include "core/loader/tga.h"

#include <gtest/gtest.h>

using zeebulator::DecodeTga;

namespace {

// Builds a minimal 18-byte TGA header for a truecolor image.
std::vector<uint8_t> MakeHeader(uint8_t image_type, uint16_t width, uint16_t height, uint8_t bpp,
                                 uint8_t descriptor) {
  std::vector<uint8_t> h(18, 0);
  h[2] = image_type;
  h[12] = width & 0xff;
  h[13] = (width >> 8) & 0xff;
  h[14] = height & 0xff;
  h[15] = (height >> 8) & 0xff;
  h[16] = bpp;
  h[17] = descriptor;
  return h;
}

}  // namespace

// Real header/pixel-layout parameters (type=2 uncompressed truecolor,
// bottom-left origin) match the real `bg_tex.tga` / `namco_tex.tga`
// samples shipped alongside Pac-Mania's `.mod` (confirmed directly:
// `id_length=0 color_map_type=0 image_type=2 ... bpp=32`, this
// project's own `find /home/rafaelfrequiao/projects/zeebo-lab/games/
// brew/mod/276212` -- zeebx's own TODO.md independently diagnosed this
// exact gap: "um recurso pedido por LoadResObject não é um PNG que
// saibamos ler ... identificou esses recursos como .tga").
TEST(Tga, DecodesUncompressed32BppBottomLeftOrigin) {
  // 2x1 image, 32bpp BGRA, bottom-left origin (descriptor=0).
  auto data = MakeHeader(2, 2, 1, 32, 0);
  // Pixel 0: BGRA = (0,0,255,255) -> red. Pixel 1: (255,0,0,255) -> blue.
  data.insert(data.end(), {0, 0, 255, 255, 255, 0, 0, 255});

  int w = 0, h = 0;
  auto decoded = DecodeTga(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(h, 1);
  auto& px = *decoded;
  EXPECT_EQ(px[0], 255); EXPECT_EQ(px[1], 0); EXPECT_EQ(px[2], 0); EXPECT_EQ(px[3], 255);
  EXPECT_EQ(px[4], 0); EXPECT_EQ(px[5], 0); EXPECT_EQ(px[6], 255); EXPECT_EQ(px[7], 255);
}

TEST(Tga, DecodesUncompressed24BppTopDownOrigin) {
  // 1x2 image, 24bpp BGR, top-left origin (descriptor bit 5 set).
  auto data = MakeHeader(2, 1, 2, 24, 0x20);
  // Row 0 (top, stored first since top_down): BGR (0,255,0) -> green.
  // Row 1: BGR (0,0,255) -> red.
  data.insert(data.end(), {0, 255, 0, 0, 0, 255});

  int w = 0, h = 0;
  auto decoded = DecodeTga(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 1);
  EXPECT_EQ(h, 2);
  auto& px = *decoded;
  EXPECT_EQ(px[0], 0); EXPECT_EQ(px[1], 255); EXPECT_EQ(px[2], 0); EXPECT_EQ(px[3], 255);
  EXPECT_EQ(px[4], 255); EXPECT_EQ(px[5], 0); EXPECT_EQ(px[6], 0); EXPECT_EQ(px[7], 255);
}

TEST(Tga, DecodesRleCompressedRunPacket) {
  // 4x1 image, 24bpp BGR, RLE. One run packet: 4 repeats of (0,0,255)=red.
  auto data = MakeHeader(10, 4, 1, 24, 0);
  data.push_back(0x80 | 3);  // run packet, count-1=3 -> 4 pixels
  data.insert(data.end(), {0, 0, 255});

  int w = 0, h = 0;
  auto decoded = DecodeTga(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 4);
  EXPECT_EQ(h, 1);
  auto& px = *decoded;
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(px[i * 4 + 0], 255);
    EXPECT_EQ(px[i * 4 + 1], 0);
    EXPECT_EQ(px[i * 4 + 2], 0);
    EXPECT_EQ(px[i * 4 + 3], 255);
  }
}

TEST(Tga, RejectsColorMappedImages) {
  auto data = MakeHeader(1, 2, 2, 8, 0);
  data[1] = 1;  // color_map_type = 1 (palette present)
  int w = 0, h = 0;
  auto decoded = DecodeTga(data.data(), data.size(), w, h);
  EXPECT_FALSE(decoded.has_value());
}

TEST(Tga, RejectsTruncatedHeader) {
  std::vector<uint8_t> data(10, 0);  // shorter than the 18-byte header
  int w = 0, h = 0;
  auto decoded = DecodeTga(data.data(), data.size(), w, h);
  EXPECT_FALSE(decoded.has_value());
}
