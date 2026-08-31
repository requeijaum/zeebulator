#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

// Decoded PCM audio: always 16-bit signed, interleaved if stereo --
// whatever the source WAV's actual bit depth was, this is the common
// shape the Mixer/Backend Abstraction Interface deal in.
struct WavAudio {
  int sample_rate = 0;
  int channels = 0;  // 1 (mono) or 2 (stereo)
  std::vector<int16_t> samples;
};

// Parses a RIFF/WAVE container holding uncompressed PCM audio (8-bit
// unsigned or 16-bit signed, mono or stereo -- confirmed via
// zeebulator_ggz_inspector to be exactly what every real Double Dragon
// sound.ggz asset actually is, not assumed -- see TASKS.md Phase 6).
// Returns nullopt for anything this doesn't handle: a malformed file, or
// -- deliberately, not a silent mis-decode -- a compressed format tag
// (e.g. IMA-ADPCM) in the `fmt ` chunk. Same "reject what's not handled
// yet rather than guess" philosophy as the CPU interpreter's
// UnimplementedInstruction.
std::optional<WavAudio> ParseWav(const uint8_t* data, size_t size);

}  // namespace zeebulator
