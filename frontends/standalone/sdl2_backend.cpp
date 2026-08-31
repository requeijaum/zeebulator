#include "frontends/standalone/sdl2_backend.h"

#include <cstdio>

namespace zeebulator {

Sdl2Backend::Sdl2Backend(SDL_Renderer* renderer, int width, int height, int audio_sample_rate)
    : renderer_(renderer),
      texture_(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                  width, height)),
      audio_sample_rate_(audio_sample_rate) {
  SDL_AudioSpec desired{};
  desired.freq = audio_sample_rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  desired.samples = 1024;

  SDL_AudioSpec obtained{};
  audio_device_ = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &desired, &obtained,
                                       /*allowed_changes=*/0);
  if (audio_device_ == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(audio_device_, 0);  // start the device unpaused

  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      controller_ = SDL_GameControllerOpen(i);
      if (controller_ != nullptr) break;
    }
  }
}

Sdl2Backend::~Sdl2Backend() {
  if (controller_ != nullptr) SDL_GameControllerClose(controller_);
  if (audio_device_ != 0) SDL_CloseAudioDevice(audio_device_);
  SDL_DestroyTexture(texture_);
}

void Sdl2Backend::PushVideoFrame(const void* framebuffer, int width, int height,
                                  PixelFormat format) {
  (void)format;  // IDisplayHle's framebuffer is always RGB565 for now.
  (void)height;  // SDL_UpdateTexture(..., nullptr, ...) updates the whole
                 // fixed-size texture_ created in the constructor.
  SDL_UpdateTexture(texture_, nullptr, framebuffer, width * 2);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void Sdl2Backend::PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                    int sample_rate) {
  if (audio_device_ == 0 || sample_rate != audio_sample_rate_) return;
  SDL_QueueAudio(audio_device_, interleaved_stereo,
                  static_cast<uint32_t>(frame_count * 2 * sizeof(int16_t)));
}

// Standard SDL_GameController button/axis naming already matches an
// Xbox-layout controller's own physical layout (A=bottom, B=right,
// X=left, Y=top), so this mapping *is* the "sane default matching a
// standard Xbox-layout controller" ARCHITECTURE.md 3.7 calls for, not
// an arbitrary choice on top of it.
ZPadState Sdl2Backend::PollController() {
  ZPadState state;
  auto Set = [&](SDL_GameControllerButton button, uint16_t mask) {
    if (SDL_GameControllerGetButton(controller_, button)) state.buttons |= mask;
  };
  Set(SDL_CONTROLLER_BUTTON_DPAD_UP, ZPadState::kDpadUp);
  Set(SDL_CONTROLLER_BUTTON_DPAD_DOWN, ZPadState::kDpadDown);
  Set(SDL_CONTROLLER_BUTTON_DPAD_LEFT, ZPadState::kDpadLeft);
  Set(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ZPadState::kDpadRight);
  Set(SDL_CONTROLLER_BUTTON_START, ZPadState::kStartHome);
  Set(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ZPadState::kShoulderL);
  Set(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ZPadState::kShoulderR);
  Set(SDL_CONTROLLER_BUTTON_X, ZPadState::kButtonWest);
  Set(SDL_CONTROLLER_BUTTON_A, ZPadState::kButtonSouth);
  Set(SDL_CONTROLLER_BUTTON_Y, ZPadState::kButtonNorth);
  Set(SDL_CONTROLLER_BUTTON_B, ZPadState::kButtonEast);

  // SDL's axis range (-32768..32767) already matches ZPadState's
  // int16_t sticks directly -- no rescaling needed.
  state.left_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
  state.left_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
  state.right_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
  state.right_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
  return state;
}

// Fallback default when no real gamepad is connected: arrow keys for
// the D-pad, Z/X/A/S for the four face buttons (a common non-gamepad
// convention, e.g. RetroArch's own default keyboard binds), Q/E for the
// shoulder buttons, Return for Start/Home. Not derived from any real
// Zeebo device (there's no physical keyboard on one) -- purely this
// frontend's own dev/debug convenience, unlike the controller mapping
// above.
ZPadState Sdl2Backend::PollKeyboard() {
  ZPadState state;
  const uint8_t* keys = SDL_GetKeyboardState(nullptr);
  auto Set = [&](SDL_Scancode key, uint16_t mask) {
    if (keys[key]) state.buttons |= mask;
  };
  Set(SDL_SCANCODE_UP, ZPadState::kDpadUp);
  Set(SDL_SCANCODE_DOWN, ZPadState::kDpadDown);
  Set(SDL_SCANCODE_LEFT, ZPadState::kDpadLeft);
  Set(SDL_SCANCODE_RIGHT, ZPadState::kDpadRight);
  Set(SDL_SCANCODE_RETURN, ZPadState::kStartHome);
  Set(SDL_SCANCODE_Q, ZPadState::kShoulderL);
  Set(SDL_SCANCODE_E, ZPadState::kShoulderR);
  Set(SDL_SCANCODE_A, ZPadState::kButtonWest);
  Set(SDL_SCANCODE_Z, ZPadState::kButtonSouth);
  Set(SDL_SCANCODE_S, ZPadState::kButtonNorth);
  Set(SDL_SCANCODE_X, ZPadState::kButtonEast);
  return state;
}

ZPadState Sdl2Backend::PollInput() {
  return controller_ != nullptr ? PollController() : PollKeyboard();
}

}  // namespace zeebulator
