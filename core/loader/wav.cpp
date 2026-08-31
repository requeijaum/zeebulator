#include "core/loader/wav.h"

#include <cstring>

namespace zeebulator {

namespace {

constexpr uint16_t kWavFormatPcm = 1;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

}  // namespace

std::optional<WavAudio> ParseWav(const uint8_t* data, size_t size) {
  if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
    return std::nullopt;
  }

  bool have_fmt = false;
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  const uint8_t* data_chunk = nullptr;
  uint32_t data_size = 0;

  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t* chunk_id = data + pos;
    uint32_t chunk_size = ReadU32LE(data + pos + 4);
    size_t chunk_data_offset = pos + 8;
    if (chunk_data_offset + chunk_size > size) break;  // truncated/corrupt chunk

    if (std::memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
      const uint8_t* fmt = data + chunk_data_offset;
      audio_format = ReadU16LE(fmt + 0);
      channels = ReadU16LE(fmt + 2);
      sample_rate = ReadU32LE(fmt + 4);
      bits_per_sample = ReadU16LE(fmt + 14);
      have_fmt = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      data_chunk = data + chunk_data_offset;
      data_size = chunk_size;
    }

    pos = chunk_data_offset + chunk_size + (chunk_size & 1);  // chunks are word-aligned
  }

  if (!have_fmt || data_chunk == nullptr) return std::nullopt;
  if (audio_format != kWavFormatPcm) return std::nullopt;  // e.g. IMA-ADPCM -- not handled here
  if (channels != 1 && channels != 2) return std::nullopt;
  if (bits_per_sample != 8 && bits_per_sample != 16) return std::nullopt;

  WavAudio out;
  out.sample_rate = static_cast<int>(sample_rate);
  out.channels = channels;

  if (bits_per_sample == 16) {
    size_t sample_count = data_size / 2;
    out.samples.resize(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
      out.samples[i] = static_cast<int16_t>(ReadU16LE(data_chunk + i * 2));
    }
  } else {  // 8-bit PCM is unsigned, centered at 128 -- widen to signed 16-bit.
    size_t sample_count = data_size;
    out.samples.resize(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
      int unsigned_sample = data_chunk[i];
      out.samples[i] = static_cast<int16_t>((unsigned_sample - 128) * 256);
    }
  }

  return out;
}

}  // namespace zeebulator
