#include "frontends/standalone/zpad_edges.h"

#include <gtest/gtest.h>

using zeebulator::DiffZPadButtonEdges;
using zeebulator::NormalizeZPadState;
using zeebulator::StickTiltToDpadBits;
using zeebulator::ZPadState;

TEST(ZPadEdges, NoChangeProducesNoEdges) {
  EXPECT_TRUE(DiffZPadButtonEdges(ZPadState::kDpadUp, ZPadState::kDpadUp).empty());
}

TEST(ZPadEdges, NewlyPressedButtonProducesAPressEdge) {
  auto edges = DiffZPadButtonEdges(0, ZPadState::kButtonSouth);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].button, ZPadState::kButtonSouth);
  EXPECT_TRUE(edges[0].pressed);
}

TEST(ZPadEdges, ReleasedButtonProducesAReleaseEdge) {
  auto edges = DiffZPadButtonEdges(ZPadState::kShoulderL, 0);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].button, ZPadState::kShoulderL);
  EXPECT_FALSE(edges[0].pressed);
}

TEST(ZPadEdges, MultipleSimultaneousChangesEachProduceTheirOwnEdge) {
  uint16_t previous = ZPadState::kDpadUp | ZPadState::kButtonWest;
  uint16_t current = ZPadState::kDpadUp | ZPadState::kDpadRight;  // West released, Right pressed
  auto edges = DiffZPadButtonEdges(previous, current);
  ASSERT_EQ(edges.size(), 2u);
  EXPECT_EQ(edges[0].button, ZPadState::kDpadRight);
  EXPECT_TRUE(edges[0].pressed);
  EXPECT_EQ(edges[1].button, ZPadState::kButtonWest);
  EXPECT_FALSE(edges[1].pressed);
}

TEST(ZPadEdges, UnchangedButtonsHeldAcrossBothStatesProduceNoEdge) {
  uint16_t previous = ZPadState::kDpadUp | ZPadState::kButtonSouth;
  uint16_t current = ZPadState::kDpadUp;  // South released, Up still held
  auto edges = DiffZPadButtonEdges(previous, current);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].button, ZPadState::kButtonSouth);
  EXPECT_FALSE(edges[0].pressed);
}

TEST(ZPadEdges, StickWithinDeadzoneProducesNoDpadBits) {
  EXPECT_EQ(StickTiltToDpadBits(100, -100), 0);
  EXPECT_EQ(StickTiltToDpadBits(0, 0), 0);
}

TEST(ZPadEdges, StickPastDeadzoneMapsToTheNearestDpadDirection) {
  EXPECT_EQ(StickTiltToDpadBits(-20000, 0), ZPadState::kDpadLeft);
  EXPECT_EQ(StickTiltToDpadBits(20000, 0), ZPadState::kDpadRight);
  EXPECT_EQ(StickTiltToDpadBits(0, -20000), ZPadState::kDpadUp);
  EXPECT_EQ(StickTiltToDpadBits(0, 20000), ZPadState::kDpadDown);
}

TEST(ZPadEdges, DiagonalStickTiltMapsToTwoDpadBits) {
  EXPECT_EQ(StickTiltToDpadBits(20000, -20000), ZPadState::kDpadRight | ZPadState::kDpadUp);
}

TEST(ZPadEdges, NormalizeZPadStateMergesStickIntoDpadButtonsWithoutLosingRealButtons) {
  ZPadState state;
  state.buttons = ZPadState::kShoulderR;
  state.left_stick_x = -20000;
  ZPadState normalized = NormalizeZPadState(state);
  EXPECT_EQ(normalized.buttons, ZPadState::kShoulderR | ZPadState::kDpadLeft);
}
