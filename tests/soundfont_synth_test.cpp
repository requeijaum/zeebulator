#include "core/audio/soundfont_synth.h"

#include <cmath>

#include <gtest/gtest.h>

#include "core/loader/midi.h"

using zeebulator::MidiFile;
using zeebulator::ParseMidi;
using zeebulator::SoundFontSynth;

namespace {

void AppendU32BE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}
void AppendU16BE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}
void AppendVlq(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t buf[4];
  int n = 0;
  buf[n++] = static_cast<uint8_t>(value & 0x7F);
  value >>= 7;
  while (value > 0) {
    buf[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}

// Builds a single-track (format 0) MIDI file from a flat list of already-
// encoded track event bytes (delta-time VLQ + event bytes, repeated).
std::vector<uint8_t> BuildMidi(uint16_t division, const std::vector<uint8_t>& track_events) {
  std::vector<uint8_t> track = track_events;
  AppendVlq(track, 0);
  track.push_back(0xFF);
  track.push_back(0x2F);
  track.push_back(0x00);

  std::vector<uint8_t> out;
  out.insert(out.end(), {'M', 'T', 'h', 'd'});
  AppendU32BE(out, 6);
  AppendU16BE(out, 0);
  AppendU16BE(out, 1);
  AppendU16BE(out, division);
  out.insert(out.end(), {'M', 'T', 'r', 'k'});
  AppendU32BE(out, static_cast<uint32_t>(track.size()));
  out.insert(out.end(), track.begin(), track.end());
  return out;
}

double Rms(const std::vector<int16_t>& samples) {
  double sum_sq = 0.0;
  for (int16_t s : samples) sum_sq += static_cast<double>(s) * s;
  return samples.empty() ? 0.0 : std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

}  // namespace

// This project's own real, CMake-fetched GM soundfont (see the
// top-level CMakeLists.txt's own doc comment) -- not a mock, the
// actual bundled build dependency. If this ever fails to load, every
// test below fails loudly instead of silently testing nothing.
TEST(SoundFontSynth, RealBundledSoundFontLoadsSuccessfully) {
  SoundFontSynth synth;
  EXPECT_TRUE(synth.IsLoaded());
}

TEST(SoundFontSynth, RenderingASingleNoteProducesAudibleOutput) {
  SoundFontSynth synth;
  ASSERT_TRUE(synth.IsLoaded());

  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 69, 100});  // Note On, channel 0, A4, velocity 100
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 69, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());

  auto samples = synth.RenderMidi(*midi, 22050);
  ASSERT_FALSE(samples.empty());
  bool any_nonzero = false;
  for (int16_t s : samples) {
    if (s != 0) any_nonzero = true;
  }
  EXPECT_TRUE(any_nonzero) << "a real note through a real soundfont must be audible";
}

TEST(SoundFontSynth, DifferentProgramsOnTheSameNoteSoundAudiblyDifferent) {
  // Real, distinct GM instrument families should not render identically
  // -- if they did, the soundfont integration wouldn't actually be
  // selecting different real instrument presets per channel/program
  // (see MidiNote::program and SoundFontSynth::RenderMidi's own doc
  // comment).
  SoundFontSynth synth;
  ASSERT_TRUE(synth.IsLoaded());

  auto render_with_program = [&](int program) {
    std::vector<uint8_t> events;
    AppendVlq(events, 0);
    events.push_back(static_cast<uint8_t>(0xC0));  // Program Change, channel 0
    events.push_back(static_cast<uint8_t>(program));
    AppendVlq(events, 0);
    events.insert(events.end(), {0x90, 60, 100});
    AppendVlq(events, 960);
    events.insert(events.end(), {0x80, 60, 0});

    auto bytes = BuildMidi(480, events);
    auto midi = ParseMidi(bytes.data(), bytes.size());
    return synth.RenderMidi(*midi, 22050);
  };

  std::vector<int16_t> piano = render_with_program(0);    // Acoustic Grand Piano
  std::vector<int16_t> organ = render_with_program(19);   // Church Organ

  ASSERT_EQ(piano.size(), organ.size());
  double diff_sum_sq = 0.0;
  for (size_t i = 0; i < piano.size(); ++i) {
    double diff = static_cast<double>(piano[i]) - organ[i];
    diff_sum_sq += diff * diff;
  }
  double diff_rms = std::sqrt(diff_sum_sq / static_cast<double>(piano.size()));
  EXPECT_GT(diff_rms, 100.0)
      << "two real, distinct GM instrument families playing the same note must not render "
         "byte-for-byte identical audio";
}

TEST(SoundFontSynth, PercussionChannelProducesRealDrumKitAudio) {
  SoundFontSynth synth;
  ASSERT_TRUE(synth.IsLoaded());

  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x99, 36, 127});  // Note On, channel 9, note 36 ("bass drum")
  AppendVlq(events, 480);
  events.insert(events.end(), {0x89, 36, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes[0].channel, 9);

  auto samples = synth.RenderMidi(*midi, 22050);
  EXPECT_GT(Rms(samples), 0.0) << "a real drum hit through the real soundfont's own drum kit "
                                   "(bank 128) must be audible";
}

TEST(SoundFontSynth, EmptyMidiRendersASingleSilentSampleWithoutCrashing) {
  SoundFontSynth synth;
  ASSERT_TRUE(synth.IsLoaded());

  auto bytes = BuildMidi(480, {});
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  EXPECT_TRUE(midi->notes.empty());

  auto samples = synth.RenderMidi(*midi, 22050);
  EXPECT_EQ(samples.size(), 1u);
  EXPECT_EQ(samples[0], 0);
}
