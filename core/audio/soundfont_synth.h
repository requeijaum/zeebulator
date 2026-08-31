#pragma once

#include <cstdint>
#include <vector>

#include "core/loader/midi.h"

struct tsf;  // defined by tsf.h -- only included in soundfont_synth.cpp

namespace zeebulator {

// Real General MIDI wavetable synthesis via TinySoundFont and a real,
// CMake-fetched, GM-compliant SoundFont (see the top-level
// CMakeLists.txt's own doc comment for the real evidence this matches
// the real Zeebo hardware's own Qualcomm CMX synthesizer's
// architecture -- real wavetable/sample-based GM1/GM2, not FM
// synthesis or a hand-tuned waveform approximation). Not thread-safe
// across concurrent calls to RenderMidi on the same instance (matches
// this project's own real usage -- MediaHle renders one clip at a
// time), but one process-lifetime instance is enough; loading the real
// ~32MB soundfont file is real, measurable work not worth repeating
// per clip.
class SoundFontSynth {
 public:
  // Loads the real soundfont bundled with this build (see
  // soundfont_path.h.in). `IsLoaded()` reflects whether that real load
  // actually succeeded -- a missing/corrupt file at build time leaves
  // this false rather than crashing.
  SoundFontSynth();
  ~SoundFontSynth();

  SoundFontSynth(const SoundFontSynth&) = delete;
  SoundFontSynth& operator=(const SoundFontSynth&) = delete;

  bool IsLoaded() const { return synth_ != nullptr; }

  // Renders a full real MIDI performance to mono PCM at `sample_rate`,
  // driving real note-on/note-off/program-change events through the
  // real soundfont in the same absolute-time order they occur in
  // `midi`. Real GM channel-10 percussion (see `MidiNote::channel`) is
  // handled by the real soundfont's own real drum kit (bank 128), not
  // core/loader/midi.cpp's own hand-rolled percussion classification --
  // that logic belongs to `RenderMidiToPcm`'s own fallback synth, and
  // is unused on this path. Returns a single silent sample (never an
  // empty buffer) if `midi` has no notes, `!IsLoaded()`, or every real
  // note has non-positive duration.
  std::vector<int16_t> RenderMidi(const MidiFile& midi, int sample_rate);

 private:
  tsf* synth_ = nullptr;
};

}  // namespace zeebulator
