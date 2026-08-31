#include "core/brew/font5x7.h"

#include <gtest/gtest.h>

using zeebulator::GetGlyph5x7;

TEST(Font5x7, SpaceReturnsNullptr) {
  EXPECT_EQ(GetGlyph5x7(' '), nullptr);
}

TEST(Font5x7, UnmappedCharacterReturnsANonNullFallback) {
  const uint8_t* fallback = GetGlyph5x7('!');
  ASSERT_NE(fallback, nullptr);
  const uint8_t* fallback_again = GetGlyph5x7('#');
  ASSERT_NE(fallback_again, nullptr);
  for (int row = 0; row < 7; ++row) {
    EXPECT_EQ(fallback[row], fallback_again[row]) << "row " << row;
  }
}

TEST(Font5x7, DigitGlyphsAreAllDistinctAndFiveBitsWide) {
  const uint8_t* prev = nullptr;
  for (char c = '0'; c <= '9'; ++c) {
    const uint8_t* glyph = GetGlyph5x7(c);
    ASSERT_NE(glyph, nullptr) << "digit " << c;
    for (int row = 0; row < 7; ++row) {
      EXPECT_EQ(glyph[row] & ~0x1Fu, 0u) << "digit " << c << " row " << row << " has stray bits";
    }
    if (prev != nullptr) {
      bool identical = true;
      for (int row = 0; row < 7; ++row) {
        if (glyph[row] != prev[row]) identical = false;
      }
      EXPECT_FALSE(identical) << "digit " << c << " is identical to the previous digit";
    }
    prev = glyph;
  }
}

TEST(Font5x7, LetterHIsSymmetricLeftRightAndHasAFullCrossbar) {
  const uint8_t* h = GetGlyph5x7('H');
  ASSERT_NE(h, nullptr);
  // Top row: #...# -- leftmost and rightmost columns set, middle clear.
  EXPECT_EQ(h[0], 0b10001);
  // Middle row: crossbar spans the full width.
  EXPECT_EQ(h[3], 0b11111);
}

TEST(Font5x7, LowercaseIsNotDirectlyMappedByFont5x7Itself) {
  // Font5x7 only defines uppercase -- folding lowercase to uppercase is
  // IDisplayHle::DrawText's job, not this module's, so a raw lowercase
  // query falls back to the generic box rather than a letter shape.
  const uint8_t* lower_a = GetGlyph5x7('a');
  const uint8_t* upper_a = GetGlyph5x7('A');
  ASSERT_NE(lower_a, nullptr);
  ASSERT_NE(upper_a, nullptr);
  bool identical = true;
  for (int row = 0; row < 7; ++row) {
    if (lower_a[row] != upper_a[row]) identical = false;
  }
  EXPECT_FALSE(identical);
}
