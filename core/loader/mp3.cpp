#include "core/loader/mp3.h"

#include <cstdint>
#include <vector>

// minimp3 is a single-header decoder; define the implementation exactly once,
// here, so its object code lives in this translation unit only. Public domain
// (CC0) -- see third_party/minimp3/LICENSE.
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

namespace zeebulator {

std::optional<WavAudio> ParseMp3(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 4) return std::nullopt;

  mp3dec_t dec;
  mp3dec_init(&dec);

  WavAudio out;
  int channels = 0;
  int sample_rate = 0;
  bool decoded_any = false;

  mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  const uint8_t* cursor = data;
  int remaining = (size > static_cast<size_t>(INT32_MAX)) ? INT32_MAX : static_cast<int>(size);

  while (remaining > 0) {
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec, cursor, remaining, frame_pcm, &info);

    // No usable frame at this position: if the decoder consumed bytes (junk /
    // ID3 skip) keep scanning; otherwise we're stuck and give up.
    if (info.frame_bytes <= 0) break;

    if (samples > 0) {
      if (!decoded_any) {
        channels = info.channels;
        sample_rate = info.hz;
        decoded_any = true;
      }
      // minimp3 emits `samples` frames of `info.channels` interleaved values.
      int values = samples * info.channels;
      out.samples.insert(out.samples.end(), frame_pcm, frame_pcm + values);
    }

    cursor += info.frame_bytes;
    remaining -= info.frame_bytes;
  }

  if (!decoded_any || out.samples.empty()) return std::nullopt;
  out.channels = (channels == 2) ? 2 : 1;
  out.sample_rate = sample_rate;
  return out;
}

}  // namespace zeebulator
