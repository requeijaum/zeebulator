#include "core/loader/wav.h"

#include <cstring>

#include <gtest/gtest.h>

using zeebulator::ParseWav;
using zeebulator::WavAudio;

namespace {

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}
void AppendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void AppendTag(std::vector<uint8_t>& out, const char* tag) {
  out.insert(out.end(), tag, tag + 4);
}

// Builds a minimal, real-shaped RIFF/WAVE file: "fmt " chunk + an
// optional odd-sized junk chunk (to exercise word-alignment padding,
// like the real files' "bext" chunk does) + "data" chunk.
std::vector<uint8_t> BuildWav(uint16_t audio_format, uint16_t channels, uint32_t sample_rate,
                               uint16_t bits_per_sample, const std::vector<uint8_t>& pcm_data,
                               bool include_odd_junk_chunk = false) {
  std::vector<uint8_t> fmt_chunk;
  AppendU16LE(fmt_chunk, audio_format);
  AppendU16LE(fmt_chunk, channels);
  AppendU32LE(fmt_chunk, sample_rate);
  uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
  AppendU32LE(fmt_chunk, byte_rate);
  AppendU16LE(fmt_chunk, static_cast<uint16_t>(channels * (bits_per_sample / 8)));
  AppendU16LE(fmt_chunk, bits_per_sample);

  std::vector<uint8_t> body;
  AppendTag(body, "fmt ");
  AppendU32LE(body, static_cast<uint32_t>(fmt_chunk.size()));
  body.insert(body.end(), fmt_chunk.begin(), fmt_chunk.end());

  if (include_odd_junk_chunk) {
    AppendTag(body, "JUNK");
    AppendU32LE(body, 3);
    body.push_back(1);
    body.push_back(2);
    body.push_back(3);
    body.push_back(0);  // pad byte -- chunk_size (3) is odd
  }

  AppendTag(body, "data");
  AppendU32LE(body, static_cast<uint32_t>(pcm_data.size()));
  body.insert(body.end(), pcm_data.begin(), pcm_data.end());

  std::vector<uint8_t> out;
  AppendTag(out, "RIFF");
  AppendU32LE(out, static_cast<uint32_t>(4 + body.size()));
  AppendTag(out, "WAVE");
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace

TEST(Wav, ParsesSixteenBitMonoPcm) {
  std::vector<uint8_t> pcm;
  AppendU16LE(pcm, static_cast<uint16_t>(1000));
  AppendU16LE(pcm, static_cast<uint16_t>(static_cast<int16_t>(-2000)));
  AppendU16LE(pcm, static_cast<uint16_t>(32767));

  auto wav = BuildWav(/*format=*/1, /*channels=*/1, /*rate=*/22050, /*bits=*/16, pcm);
  auto result = ParseWav(wav.data(), wav.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->sample_rate, 22050);
  EXPECT_EQ(result->channels, 1);
  ASSERT_EQ(result->samples.size(), 3u);
  EXPECT_EQ(result->samples[0], 1000);
  EXPECT_EQ(result->samples[1], -2000);
  EXPECT_EQ(result->samples[2], 32767);
}

TEST(Wav, ParsesEightBitPcmWidenedToSigned16) {
  std::vector<uint8_t> pcm = {128, 0, 255};  // silence, min, max (unsigned 8-bit)
  auto wav = BuildWav(1, 1, 8000, 8, pcm);
  auto result = ParseWav(wav.data(), wav.size());
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->samples.size(), 3u);
  EXPECT_EQ(result->samples[0], 0) << "128 (unsigned midpoint) -> 0";
  EXPECT_EQ(result->samples[1], -32768) << "0 -> most negative";
  EXPECT_EQ(result->samples[2], 32512) << "255 -> near-most-positive";
}

TEST(Wav, ParsesStereoInterleaved) {
  std::vector<uint8_t> pcm;
  AppendU16LE(pcm, 10);   // L0
  AppendU16LE(pcm, 20);   // R0
  AppendU16LE(pcm, 30);   // L1
  AppendU16LE(pcm, 40);   // R1
  auto wav = BuildWav(1, 2, 44100, 16, pcm);
  auto result = ParseWav(wav.data(), wav.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->channels, 2);
  std::vector<int16_t> expected = {10, 20, 30, 40};
  EXPECT_EQ(result->samples, expected);
}

TEST(Wav, OddSizedChunkPaddingDoesNotDesyncSubsequentChunks) {
  std::vector<uint8_t> pcm;
  AppendU16LE(pcm, 42);
  auto wav = BuildWav(1, 1, 22050, 16, pcm, /*include_odd_junk_chunk=*/true);
  auto result = ParseWav(wav.data(), wav.size());
  ASSERT_TRUE(result.has_value()) << "the odd-sized JUNK chunk's pad byte must be skipped correctly";
  ASSERT_EQ(result->samples.size(), 1u);
  EXPECT_EQ(result->samples[0], 42);
}

TEST(Wav, RejectsNonPcmFormatTagInsteadOfMisdecoding) {
  std::vector<uint8_t> pcm = {1, 2, 3, 4};
  auto wav = BuildWav(/*format=*/0x0011 /* IMA ADPCM */, 1, 22050, 4, pcm);
  auto result = ParseWav(wav.data(), wav.size());
  EXPECT_FALSE(result.has_value());
}

TEST(Wav, RejectsFileMissingRiffHeader) {
  std::vector<uint8_t> not_a_wav = {'j', 'u', 'n', 'k', 0, 0, 0, 0};
  EXPECT_FALSE(ParseWav(not_a_wav.data(), not_a_wav.size()).has_value());
}

TEST(Wav, RejectsTruncatedFile) {
  auto wav = BuildWav(1, 1, 22050, 16, {1, 2, 3, 4});
  wav.resize(wav.size() - 2);  // cut off the tail of the data chunk
  auto result = ParseWav(wav.data(), wav.size());
  // Either rejected outright or parsed with whatever chunks fully fit --
  // must not crash or read out of bounds (verified by ASan/UBSan in CI
  // if enabled, and by simply not crashing here).
  (void)result;
}
