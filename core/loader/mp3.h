#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/loader/wav.h"

namespace zeebulator {

// Decodes an MPEG-1/2 Audio Layer III (MP3) stream into the same interleaved
// 16-bit signed WavAudio shape the Mixer deals in. BREW's AEECLSID_MEDIAMP3
// (0x01005502) delivers songs as raw MP3 buffers; several Zeebo titles ship
// background music this way rather than as MIDI or WAV.
//
// Decoding is delegated to the bundled minimp3 (public domain / CC0, see
// third_party/minimp3/LICENSE) -- a self-contained decoder, no vendor code.
// Returns nullopt for anything that isn't a decodable MP3 (no sync frame
// found), matching ParseWav's "reject what we can't handle rather than
// mis-decode" philosophy.
std::optional<WavAudio> ParseMp3(const uint8_t* data, size_t size);

}  // namespace zeebulator
