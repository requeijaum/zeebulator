#include "core/loader/atitc.h"

#include <gtest/gtest.h>

using zeebulator::AtitcFormat;
using zeebulator::DecodeAtitc;

namespace {

// Real 8-byte RGB block, byte-for-byte lifted from this project's own
// bundled real Qualcomm sample (research/samples/.../simple_atitc/data/
// texture_rgb.atitc.org, block (26,18)) -- not synthetic. Expected
// pixels below are what this project's derived formula (see atitc.h's
// doc comment) produces for these exact real bytes, cross-checked in
// Python against the same real file's real, independently-rendered
// texture_rgb.tga during derivation (TASKS.md/PHASE8_LOG.md Phase 8);
// this test just pins the C++ implementation to that already-verified
// result so a regression here is caught mechanically.
constexpr uint8_t kRealRgbBlock[8] = {0xb3, 0x2d, 0xd3, 0xa4, 0x1f, 0xa9, 0xe7, 0x44};
constexpr uint8_t kRealRgbExpected[16][3] = {
    {165, 154, 156}, {165, 154, 156}, {115, 122, 156}, {90, 107, 156},
    {115, 122, 156}, {136, 136, 156}, {136, 136, 156}, {136, 136, 156},
    {165, 154, 156}, {115, 122, 156}, {136, 136, 156}, {165, 154, 156},
    {90, 107, 156},  {115, 122, 156}, {90, 107, 156},  {115, 122, 156},
};

// Real 16-byte RGBA block (same source, texture_rgba.atitc, block
// (17,22)) -- chosen for having the full 0-15 alpha-nibble range in one
// block, exercising every alpha code, not just near-opaque ones.
constexpr uint8_t kRealRgbaBlock[16] = {0xff, 0xff, 0xff, 0xff, 0xbd, 0x89, 0x00, 0x00,
                                         0xf5, 0x35, 0xff, 0xef, 0x00, 0x00, 0x95, 0xff};
constexpr uint8_t kRealRgbaExpected[16][4] = {
    {107, 123, 173, 255}, {107, 123, 173, 255}, {107, 123, 173, 255}, {107, 123, 173, 255},
    {107, 123, 173, 255}, {107, 123, 173, 255}, {107, 123, 173, 255}, {107, 123, 173, 255},
    {151, 167, 200, 221}, {151, 167, 200, 187}, {151, 167, 200, 153}, {189, 205, 224, 136},
    {239, 255, 255, 0},   {239, 255, 255, 0},   {239, 255, 255, 0},   {239, 255, 255, 0},
};

TEST(Atitc, DecodesARealRgbBlockToTheDerivedFormulaResult) {
  auto result = DecodeAtitc(kRealRgbBlock, sizeof(kRealRgbBlock), 4, 4, AtitcFormat::kRgb);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 4u * 4u * 4u);
  for (int p = 0; p < 16; ++p) {
    SCOPED_TRACE(p);
    const uint8_t* px = result->data() + p * 4;
    EXPECT_EQ(px[0], kRealRgbExpected[p][0]);
    EXPECT_EQ(px[1], kRealRgbExpected[p][1]);
    EXPECT_EQ(px[2], kRealRgbExpected[p][2]);
    EXPECT_EQ(px[3], 255) << "kRgb blocks are always fully opaque";
  }
}

TEST(Atitc, DecodesARealRgbaBlockIncludingFullAlphaRange) {
  auto result = DecodeAtitc(kRealRgbaBlock, sizeof(kRealRgbaBlock), 4, 4, AtitcFormat::kRgba);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 4u * 4u * 4u);
  for (int p = 0; p < 16; ++p) {
    SCOPED_TRACE(p);
    const uint8_t* px = result->data() + p * 4;
    EXPECT_EQ(px[0], kRealRgbaExpected[p][0]);
    EXPECT_EQ(px[1], kRealRgbaExpected[p][1]);
    EXPECT_EQ(px[2], kRealRgbaExpected[p][2]);
    EXPECT_EQ(px[3], kRealRgbaExpected[p][3]);
  }
}

TEST(Atitc, RejectsDataTooSmallForTheClaimedDimensions) {
  // A single kRgb block is 8 bytes -- claiming an 8x4 (2-block-wide)
  // image needs 16.
  EXPECT_FALSE(DecodeAtitc(kRealRgbBlock, sizeof(kRealRgbBlock), 8, 4, AtitcFormat::kRgb));
  // kRgba needs 16 bytes/block, not 8.
  EXPECT_FALSE(DecodeAtitc(kRealRgbBlock, sizeof(kRealRgbBlock), 4, 4, AtitcFormat::kRgba));
}

TEST(Atitc, CropsNonBlockAlignedDimensionsFromAFullyDecodedBlock) {
  // Same real block, but claiming only a 2x3 image out of its 4x4 --
  // the decoder must still consume/require a full block's worth of
  // input, just crop the output.
  auto result = DecodeAtitc(kRealRgbBlock, sizeof(kRealRgbBlock), 2, 3, AtitcFormat::kRgb);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 2u * 3u * 4u);
  // Row-major crop of the block's top-left 2x3 corner (p=0,1 / 4,5 / 8,9).
  const int kExpectedTexels[6] = {0, 1, 4, 5, 8, 9};
  for (int i = 0; i < 6; ++i) {
    SCOPED_TRACE(i);
    const uint8_t* px = result->data() + i * 4;
    int p = kExpectedTexels[i];
    EXPECT_EQ(px[0], kRealRgbExpected[p][0]);
    EXPECT_EQ(px[1], kRealRgbExpected[p][1]);
    EXPECT_EQ(px[2], kRealRgbExpected[p][2]);
  }
}

}  // namespace
