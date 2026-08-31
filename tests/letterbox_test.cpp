#include "frontends/standalone/letterbox.h"

#include <gtest/gtest.h>

using zeebulator::ComputeLetterboxedViewport;

TEST(Letterbox, ExactAspectRatioMatchFillsTheWholeWindow) {
  auto rect = ComputeLetterboxedViewport(1280, 960, 640, 480);  // both 4:3, window is exactly 2x
  EXPECT_EQ(rect.x, 0);
  EXPECT_EQ(rect.y, 0);
  EXPECT_EQ(rect.width, 1280);
  EXPECT_EQ(rect.height, 960);
}

TEST(Letterbox, OneXIsAnExactAspectRatioMatchToo) {
  auto rect = ComputeLetterboxedViewport(640, 480, 640, 480);
  EXPECT_EQ(rect.x, 0);
  EXPECT_EQ(rect.y, 0);
  EXPECT_EQ(rect.width, 640);
  EXPECT_EQ(rect.height, 480);
}

TEST(Letterbox, WiderWindowThanLogicalPillarboxesWithBarsOnTheSides) {
  // A 16:9 window showing a 4:3 logical image -- full height, centered
  // horizontally, bars left/right.
  auto rect = ComputeLetterboxedViewport(1920, 1080, 640, 480);
  EXPECT_EQ(rect.height, 1080);
  EXPECT_EQ(rect.width, 1440);  // 1080 * 4/3
  EXPECT_EQ(rect.y, 0);
  EXPECT_EQ(rect.x, (1920 - 1440) / 2);
}

TEST(Letterbox, TallerWindowThanLogicalLetterboxesWithBarsTopAndBottom) {
  // A tall/narrow window showing a 4:3 logical image -- full width,
  // centered vertically, bars top/bottom.
  auto rect = ComputeLetterboxedViewport(640, 960, 640, 480);
  EXPECT_EQ(rect.width, 640);
  EXPECT_EQ(rect.height, 480);  // 640 / (4/3)
  EXPECT_EQ(rect.x, 0);
  EXPECT_EQ(rect.y, (960 - 480) / 2);
}

TEST(Letterbox, ResultingRectNeverExceedsTheWindowBounds) {
  for (int w = 100; w <= 2000; w += 137) {
    for (int h = 100; h <= 2000; h += 149) {
      auto rect = ComputeLetterboxedViewport(w, h, 640, 480);
      EXPECT_GE(rect.x, 0);
      EXPECT_GE(rect.y, 0);
      EXPECT_LE(rect.x + rect.width, w);
      EXPECT_LE(rect.y + rect.height, h);
    }
  }
}

TEST(Letterbox, DegenerateWindowSizeReturnsAnEmptyRectRatherThanDividingByZero) {
  EXPECT_EQ(ComputeLetterboxedViewport(0, 480, 640, 480).width, 0);
  EXPECT_EQ(ComputeLetterboxedViewport(640, 0, 640, 480).width, 0);
  EXPECT_EQ(ComputeLetterboxedViewport(640, 480, 0, 480).width, 0);
  EXPECT_EQ(ComputeLetterboxedViewport(-1, 480, 640, 480).width, 0);
}
