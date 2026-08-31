#pragma once

#include <vector>

#include "core/backend.h"

namespace zeebulator {

// One button transition between two consecutive `ZPadState` reads --
// `button` is one of `ZPadState::kDpadUp` et al. `PollController`'s real
// `SDL_GameController` state is level-triggered (polled once per tick),
// unlike the existing keyboard path (edge-triggered directly off real
// `SDL_KEYDOWN`/`SDL_KEYUP` events) -- diffing two consecutive polls this
// way produces the same shape of press/release events so both input
// sources can feed the same downstream HID-button injection code.
struct ZPadButtonEdge {
  uint16_t button = 0;
  bool pressed = false;
};

// Returns one edge per button bit that differs between `previous` and
// `current`, in `ZPadState`'s own bit order.
std::vector<ZPadButtonEdge> DiffZPadButtonEdges(uint16_t previous_buttons,
                                                 uint16_t current_buttons);

// Quantizes a real analog stick's tilt down to the D-pad bits it's
// nearest to, past a fixed deadzone -- Double Dragon's own recognized HID
// UID subset (see game_probe.cpp's own SdlKeyToHidButton doc comment)
// only understands the digital D-pad, not raw analog axes, so any real
// stick input has to become D-pad bits to be observable at all. `y`
// follows `ZPadState`/SDL's own convention: negative is up.
uint16_t StickTiltToDpadBits(int16_t x, int16_t y);

// Returns `state` with its D-pad bits augmented by whatever direction its
// left stick (if any) is tilted -- see `StickTiltToDpadBits`. The right
// stick isn't mapped: there's only one real D-pad's worth of directional
// HID input to feed.
ZPadState NormalizeZPadState(const ZPadState& state);

}  // namespace zeebulator
