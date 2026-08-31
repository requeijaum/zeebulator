#pragma once

#include <SDL.h>

#include "core/backend.h"

namespace zeebulator {

// Real SDL2 implementation of the Backend Abstraction Interface: video
// via a streaming SDL_Texture (as before), audio via a real
// SDL_AudioDevice fed through SDL_QueueAudio, input via a real
// SDL_GameController (falling back to a fixed keyboard layout when none
// is connected) mapped onto ZPadState -- see ARCHITECTURE.md 3.7's "sane
// default matching a standard Xbox-layout controller" and core/
// backend.h's ZPadState bit layout doc comment for the exact mapping.
//
// Requires the caller to have initialized SDL with
// SDL_INIT_GAMECONTROLLER (in addition to VIDEO/AUDIO) for real gamepad
// detection to work; keyboard fallback works regardless.
//
// The audio device is opened once, at construction, for a fixed
// (sample_rate, 2-channel, S16) spec -- matching Mixer's own "one fixed
// output rate, no resampling" design (see core/audio/mixer.h). A
// PushAudioSamples call with a different sample_rate than what the
// device was opened for would play at the wrong pitch/speed; not
// currently possible since this frontend only ever constructs one Mixer
// at one fixed rate.
class Sdl2Backend : public Backend {
 public:
  Sdl2Backend(SDL_Renderer* renderer, int width, int height, int audio_sample_rate);
  ~Sdl2Backend() override;

  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override;
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                         int sample_rate) override;
  ZPadState PollInput() override;

 private:
  ZPadState PollController();
  ZPadState PollKeyboard();

  SDL_Renderer* renderer_;
  SDL_Texture* texture_;
  SDL_AudioDeviceID audio_device_ = 0;
  int audio_sample_rate_;
  SDL_GameController* controller_ = nullptr;
};

}  // namespace zeebulator
