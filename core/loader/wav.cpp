#include "core/loader/wav.h"

#include <algorithm>
#include <cstring>

namespace zeebulator {

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

constexpr uint16_t kWavFormatPcm = 1;
constexpr uint16_t kWavFormatImaAdpcm = 17;

// IMA/DVI ADPCM decode tables (public standard: IMA Digital Audio spec).
// Mirrors the algorithm in zeebx src/wav.rs (RE oracle); the tables are the
// canonical IMA constants, no code copied.
constexpr int8_t kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                        -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int32_t kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

struct ImaChannel {
  int32_t predictor = 0;
  int32_t index = 0;

  int16_t Decode(uint8_t nibble) {
    int32_t step = kImaStepTable[index < 0 ? 0 : (index > 88 ? 88 : index)];
    int32_t magnitude = nibble & 7;
    int32_t delta = step >> 3;
    if (magnitude & 4) delta += step;
    if (magnitude & 2) delta += step >> 1;
    if (magnitude & 1) delta += step >> 2;
    predictor += (nibble & 8) ? -delta : delta;
    if (predictor < -32768) predictor = -32768;
    if (predictor > 32767) predictor = 32767;
    index += kImaIndexTable[nibble & 0xf];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    return static_cast<int16_t>(predictor);
  }
};

// Decodes IMA-ADPCM data blocks into interleaved 16-bit PCM. Each block starts
// with a 4-byte header per channel (initial predictor + step index), then the
// nibble body; in stereo, bytes come in groups of 4 per channel.
std::vector<int16_t> DecodeImaAdpcm(const uint8_t* payload, uint32_t payload_size,
                                    int channels, int block_align) {
  std::vector<int16_t> out;
  if (channels <= 0 || block_align < 4 * channels) return out;
  for (uint32_t base = 0; base < payload_size; base += block_align) {
    uint32_t block_len = payload_size - base;
    if (block_len > static_cast<uint32_t>(block_align)) block_len = block_align;
    if (block_len < static_cast<uint32_t>(4 * channels)) break;
    const uint8_t* block = payload + base;

    std::vector<ImaChannel> state(channels);
    for (int ch = 0; ch < channels; ++ch) {
      int at = ch * 4;
      state[ch].predictor = static_cast<int16_t>(ReadU16LE(block + at));
      state[ch].index = block[at + 2];
    }
    // The header sample is the first output frame.
    for (int ch = 0; ch < channels; ++ch) out.push_back(static_cast<int16_t>(state[ch].predictor));

    uint32_t body_off = 4 * channels;
    uint32_t group = 4 * channels;
    for (uint32_t g = body_off; g < block_len; g += group) {
      uint32_t glen = block_len - g;
      if (glen > group) glen = group;
      std::vector<std::vector<int16_t>> decoded(channels);
      for (int ch = 0; ch < channels; ++ch) {
        uint32_t lane_off = g + static_cast<uint32_t>(ch * 4);
        for (int b = 0; b < 4; ++b) {
          if (lane_off + b >= g + glen) break;
          uint8_t byte = block[lane_off + b];
          decoded[ch].push_back(state[ch].Decode(byte & 0xf));
          decoded[ch].push_back(state[ch].Decode(byte >> 4));
        }
      }
      size_t frames = decoded[0].size();
      for (int ch = 1; ch < channels; ++ch) frames = std::min(frames, decoded[ch].size());
      for (size_t frame = 0; frame < frames; ++frame)
        for (int ch = 0; ch < channels; ++ch) out.push_back(decoded[ch][frame]);
    }
  }
  return out;
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
  uint16_t block_align = 0;
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
      block_align = ReadU16LE(fmt + 12);
      bits_per_sample = ReadU16LE(fmt + 14);
      have_fmt = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      data_chunk = data + chunk_data_offset;
      data_size = chunk_size;
    }

    pos = chunk_data_offset + chunk_size + (chunk_size & 1);  // chunks are word-aligned
  }

  if (!have_fmt || data_chunk == nullptr) return std::nullopt;
  if (channels != 1 && channels != 2) return std::nullopt;
  // sample_rate=0 produz step=0 no mixer: a voz repete a primeira amostra para
  // sempre e nunca emite DONE. Limite superior evita conversao int/abuso.
  if (sample_rate == 0 || sample_rate > 384000) return std::nullopt;

  if (audio_format == kWavFormatImaAdpcm) {
    WavAudio out;
    out.sample_rate = static_cast<int>(sample_rate);
    out.channels = channels;
    out.samples = DecodeImaAdpcm(data_chunk, data_size, channels, block_align);
    if (out.samples.empty()) return std::nullopt;
    return out;
  }

  if (audio_format != kWavFormatPcm) return std::nullopt;  // e.g. MP3 -- not handled here
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
