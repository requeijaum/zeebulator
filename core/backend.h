#pragma once

#include <cstddef>
#include <cstdint>

namespace zeebulator {

// Zeebo's Z-Pad: D-pad, two analog sticks, 7 buttons, ZL/ZR, Home.
// Populated by the frontend (libretro or standalone) on each PollInput call.
//
// `buttons` bit layout below is this project's own convention (not a
// literal real hardware register layout -- no evidence either way on
// that), but every bit corresponds to a real, physical Z-Pad button
// whose real BREW `IHIDDevice` UID is confirmed against the bundled
// real `AEEHIDButtons.h` header (TASKS.md Phase 8; see
// core/brew/hid_hle.h for the UID values themselves). `core/brew/
// hid_hle.cpp` is the only place this bit layout needs to be kept in
// sync with.
struct ZPadState {
  static constexpr uint16_t kDpadUp = 1u << 0;
  static constexpr uint16_t kDpadDown = 1u << 1;
  static constexpr uint16_t kDpadLeft = 1u << 2;
  static constexpr uint16_t kDpadRight = 1u << 3;
  static constexpr uint16_t kStartHome = 1u << 4;
  static constexpr uint16_t kShoulderL = 1u << 5;
  static constexpr uint16_t kShoulderR = 1u << 6;
  static constexpr uint16_t kButtonWest = 1u << 7;   // real UID Button_1
  static constexpr uint16_t kButtonSouth = 1u << 8;  // real UID Button_2
  static constexpr uint16_t kButtonNorth = 1u << 9;  // real UID Button_3
  static constexpr uint16_t kButtonEast = 1u << 10;  // real UID Button_4

  uint16_t buttons = 0;
  int16_t left_stick_x = 0;
  int16_t left_stick_y = 0;
  int16_t right_stick_x = 0;
  int16_t right_stick_y = 0;
};

enum class PixelFormat { kRGB565, kXRGB8888 };

// The seam between the emulation core and the outside world. Both the
// libretro core shim and the standalone frontend implement this; the core
// never knows which one it's talking to. See ARCHITECTURE.md 3.8.
class Backend {
 public:
  virtual ~Backend() = default;

  virtual void PushVideoFrame(const void* framebuffer, int width, int height,
                               PixelFormat format) = 0;
  // `sample_rate` travels with every push rather than being fixed
  // per-Backend or negotiated once: real Double Dragon audio assets are
  // uniformly 22050Hz (confirmed against the actual game data -- see
  // TASKS.md Phase 6), but nothing about the interface should assume
  // every title/asset shares one rate. The Mixer (core/audio/mixer.h)
  // is the only thing that calls this today, and it always passes
  // whatever rate its voices were actually recorded at.
  virtual void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                 int sample_rate) = 0;
  virtual ZPadState PollInput() = 0;
};

}  // namespace zeebulator
