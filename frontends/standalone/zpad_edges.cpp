#include "frontends/standalone/zpad_edges.h"

namespace zeebulator {

namespace {

constexpr uint16_t kAllButtons[] = {
    ZPadState::kDpadUp,     ZPadState::kDpadDown,    ZPadState::kDpadLeft,
    ZPadState::kDpadRight,  ZPadState::kStartHome,   ZPadState::kShoulderL,
    ZPadState::kShoulderR,  ZPadState::kButtonWest,  ZPadState::kButtonSouth,
    ZPadState::kButtonNorth, ZPadState::kButtonEast,
};

// A real thumbstick idles a few hundred units off dead center even at
// rest (analog drift/noise), so this needs to be comfortably past that,
// not just nonzero -- ~24% of the real int16 range, a common real-world
// deadzone size for this class of stick.
constexpr int16_t kStickDeadzone = 8000;

}  // namespace

std::vector<ZPadButtonEdge> DiffZPadButtonEdges(uint16_t previous_buttons,
                                                 uint16_t current_buttons) {
  std::vector<ZPadButtonEdge> edges;
  uint16_t changed = previous_buttons ^ current_buttons;
  for (uint16_t button : kAllButtons) {
    if ((changed & button) != 0) {
      edges.push_back({button, (current_buttons & button) != 0});
    }
  }
  return edges;
}

uint16_t StickTiltToDpadBits(int16_t x, int16_t y) {
  uint16_t bits = 0;
  if (x <= -kStickDeadzone) bits |= ZPadState::kDpadLeft;
  else if (x >= kStickDeadzone) bits |= ZPadState::kDpadRight;
  if (y <= -kStickDeadzone) bits |= ZPadState::kDpadUp;
  else if (y >= kStickDeadzone) bits |= ZPadState::kDpadDown;
  return bits;
}

ZPadState NormalizeZPadState(const ZPadState& state) {
  ZPadState normalized = state;
  normalized.buttons |= StickTiltToDpadBits(state.left_stick_x, state.left_stick_y);
  return normalized;
}

}  // namespace zeebulator
