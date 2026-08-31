#include "core/loader/midi.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

using zeebulator::MidiFile;
using zeebulator::ParseMidi;
using zeebulator::RenderMidiToPcm;

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
  // End of track meta event.
  AppendVlq(track, 0);
  track.push_back(0xFF);
  track.push_back(0x2F);
  track.push_back(0x00);

  std::vector<uint8_t> out;
  out.insert(out.end(), {'M', 'T', 'h', 'd'});
  AppendU32BE(out, 6);
  AppendU16BE(out, 0);  // format 0
  AppendU16BE(out, 1);  // 1 track
  AppendU16BE(out, division);
  out.insert(out.end(), {'M', 'T', 'r', 'k'});
  AppendU32BE(out, static_cast<uint32_t>(track.size()));
  out.insert(out.end(), track.begin(), track.end());
  return out;
}

}  // namespace

TEST(Midi, RejectsFileMissingHeader) {
  std::vector<uint8_t> not_midi = {'j', 'u', 'n', 'k', 0, 0, 0, 0};
  EXPECT_FALSE(ParseMidi(not_midi.data(), not_midi.size()).has_value());
}

TEST(Midi, RejectsSmpteDivision) {
  auto midi_bytes = BuildMidi(0, {});
  // Overwrite division with an SMPTE-style value (top bit set).
  midi_bytes[12] = 0xE3;
  midi_bytes[13] = 0x00;
  EXPECT_FALSE(ParseMidi(midi_bytes.data(), midi_bytes.size()).has_value());
}

TEST(Midi, ParsesSingleNoteOnOffPair) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});  // Note On, channel 0, note 60, vel 100
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 60, 0});  // Note Off, same note

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 1u);
  EXPECT_EQ(midi->notes[0].start_tick, 0u);
  EXPECT_EQ(midi->notes[0].end_tick, 480u);
  EXPECT_EQ(midi->notes[0].note, 60);
  EXPECT_EQ(midi->notes[0].velocity, 100);
}

TEST(Midi, NoteOnWithZeroVelocityActsAsNoteOff) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 64, 80});
  AppendVlq(events, 240);
  events.insert(events.end(), {0x90, 64, 0});  // Note On vel=0 == Note Off

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 1u);
  EXPECT_EQ(midi->notes[0].end_tick, 240u);
}

TEST(Midi, RunningStatusOmitsRepeatedStatusByte) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});  // Note On, explicit status
  AppendVlq(events, 10);
  events.insert(events.end(), {62, 90});  // running status: another Note On, no 0x90 byte
  AppendVlq(events, 100);
  events.insert(events.end(), {0x80, 60, 0});  // Note Off, explicit status
  AppendVlq(events, 0);
  events.insert(events.end(), {62, 0});  // running status: Note Off for note 62, no 0x80 byte

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 2u) << "both notes must be recovered via running status";
}

TEST(Midi, SimultaneousNotesFormAChord) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 64, 100});
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 67, 100});
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 60, 0});
  AppendVlq(events, 0);
  events.insert(events.end(), {0x80, 64, 0});
  AppendVlq(events, 0);
  events.insert(events.end(), {0x80, 67, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  EXPECT_EQ(midi->notes.size(), 3u);
}

TEST(Midi, UnclosedNoteEndsAtTrackEnd) {
  // Built by hand rather than via BuildMidi()/events (which always
  // attaches a zero-delta End-of-Track): a delta-time must always be
  // immediately followed by an event in real MIDI, so to get a nonzero
  // gap between the Note On and End-of-Track, the gap has to be End-of-
  // Track's own delta-time.
  std::vector<uint8_t> track;
  AppendVlq(track, 0);
  track.insert(track.end(), {0x90, 60, 100});  // Note On, never closed
  AppendVlq(track, 100);                       // 100 ticks pass...
  track.push_back(0xFF);                       // ...then End-of-Track
  track.push_back(0x2F);
  track.push_back(0x00);

  std::vector<uint8_t> bytes;
  bytes.insert(bytes.end(), {'M', 'T', 'h', 'd'});
  AppendU32BE(bytes, 6);
  AppendU16BE(bytes, 0);
  AppendU16BE(bytes, 1);
  AppendU16BE(bytes, 480);
  bytes.insert(bytes.end(), {'M', 'T', 'r', 'k'});
  AppendU32BE(bytes, static_cast<uint32_t>(track.size()));
  bytes.insert(bytes.end(), track.begin(), track.end());

  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 1u);
  EXPECT_EQ(midi->notes[0].end_tick, 100u) << "closed at the track's final tick position";
}

TEST(Midi, TempoChangeAffectsRenderedDuration) {
  // One note lasting exactly 1 quarter note (division ticks) at the
  // default tempo (120 BPM = 0.5s/quarter) should render to ~0.5s.
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 60, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->tempo_changes.size(), 1u);
  EXPECT_EQ(midi->tempo_changes[0].second, 500000u) << "default 120 BPM";

  auto pcm = RenderMidiToPcm(*midi, 22050);
  double expected_seconds = 0.5;
  double actual_seconds = static_cast<double>(pcm.samples.size()) / pcm.sample_rate;
  EXPECT_NEAR(actual_seconds, expected_seconds, 0.05);
}

TEST(Midi, ExplicitTempoChangeIsParsedAndUsed) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.push_back(0xFF);
  events.push_back(0x51);
  events.push_back(0x03);
  // 1,000,000 us/quarter = 60 BPM (half the default speed).
  events.push_back(0x0F);
  events.push_back(0x42);
  events.push_back(0x40);
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 60, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->tempo_changes.size(), 1u);
  EXPECT_EQ(midi->tempo_changes[0].second, 1000000u);

  auto pcm = RenderMidiToPcm(*midi, 22050);
  double actual_seconds = static_cast<double>(pcm.samples.size()) / pcm.sample_rate;
  EXPECT_NEAR(actual_seconds, 1.0, 0.05) << "60 BPM -> 1 quarter note = 1 second";
}

TEST(Midi, RenderProducesNonSilentAudioAtCorrectPitch) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 69, 127});  // A4 = 440Hz, max velocity
  AppendVlq(events, 480);
  events.insert(events.end(), {0x80, 69, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());

  auto pcm = RenderMidiToPcm(*midi, 22050);
  ASSERT_FALSE(pcm.samples.empty());
  bool any_nonzero = false;
  for (int16_t s : pcm.samples) {
    if (s != 0) any_nonzero = true;
  }
  EXPECT_TRUE(any_nonzero) << "a max-velocity note must produce audible (non-silent) samples";

  // Rough sanity check: count zero-crossings over the render and derive
  // an approximate frequency, expecting it to be near 440Hz.
  int crossings = 0;
  for (size_t i = 1; i < pcm.samples.size(); ++i) {
    if ((pcm.samples[i - 1] < 0) != (pcm.samples[i] < 0)) ++crossings;
  }
  double seconds = static_cast<double>(pcm.samples.size()) / pcm.sample_rate;
  double approx_freq = (crossings / 2.0) / seconds;
  EXPECT_NEAR(approx_freq, 440.0, 30.0);
}

TEST(Midi, ProgramChangeIsCapturedAtNoteOnTimeNotRetroactively) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0xC0, 48});  // Program Change, channel 0, program 48 (Strings)
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 60, 100});  // Note On -- must capture program 48
  AppendVlq(events, 10);
  events.insert(events.end(), {0xC0, 0});  // Program Change to 0 -- must NOT affect the note above
  AppendVlq(events, 470);
  events.insert(events.end(), {0x80, 60, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 1u);
  EXPECT_EQ(midi->notes[0].program, 48)
      << "a later Program Change on the same channel must not retroactively change an "
         "already-started note's captured program";
}

double Rms(const std::vector<int16_t>& samples, size_t begin, size_t end) {
  double sum_sq = 0.0;
  for (size_t i = begin; i < end; ++i) sum_sq += static_cast<double>(samples[i]) * samples[i];
  return std::sqrt(sum_sq / static_cast<double>(end - begin));
}

TEST(Midi, PluckEnvelopeInstrumentsDecayOverTheNotesOwnDuration) {
  // Program 0 (Acoustic Grand Piano) is a real GM "plucked" family --
  // real piano notes audibly lose energy over their own duration even
  // while held, unlike a sustained organ/pad tone.
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 69, 127});
  AppendVlq(events, 1920);  // a long-held note: 4 quarter notes = 2s at default tempo
  events.insert(events.end(), {0x80, 69, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes[0].program, 0);

  auto pcm = RenderMidiToPcm(*midi, 22050);
  size_t n = pcm.samples.size();
  double rms_first_half = Rms(pcm.samples, 0, n / 2);
  double rms_second_half = Rms(pcm.samples, n / 2, n);
  EXPECT_LT(rms_second_half, rms_first_half * 0.5)
      << "a plucked-family note must audibly decay well before its own end";
}

TEST(Midi, SustainEnvelopeInstrumentsDoNotDecayOverTheNotesOwnDuration) {
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0xC0, 48});  // Strings -- a real GM "sustained" family
  AppendVlq(events, 0);
  events.insert(events.end(), {0x90, 69, 127});
  AppendVlq(events, 1920);
  events.insert(events.end(), {0x80, 69, 0});

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes[0].program, 48);

  auto pcm = RenderMidiToPcm(*midi, 22050);
  size_t n = pcm.samples.size();
  double rms_first_half = Rms(pcm.samples, 0, n / 2);
  double rms_second_half = Rms(pcm.samples, n / 2, n);
  EXPECT_GT(rms_second_half, rms_first_half * 0.8)
      << "a sustained-family note must hold its loudness across its own duration";
}

TEST(Midi, PercussionChannelNotesRenderAsANoiseBurstNotAPitchedTone) {
  // Real GM channel 10 (index 9) note numbers mean specific drum
  // sounds, not pitches -- running them through the pitched
  // synthesizer produced real, confirmed-live spurious low-frequency
  // "boom" throughout real background music (PHASE8_LOG.md's "Sound,
  // round twelve"). A note on channel 9 renders as a short, fast-
  // decaying noise burst -- audibly present (unlike total silence,
  // which read as a "missing instrument" once real gameplay music was
  // reachable to listen to) but not a pitched tone.
  std::vector<uint8_t> events;
  AppendVlq(events, 0);
  events.insert(events.end(), {0x99, 36, 127});  // Note On, channel 9, note 36 ("bass drum")
  AppendVlq(events, 480);
  events.insert(events.end(), {0x89, 36, 0});  // Note Off, channel 9

  auto bytes = BuildMidi(480, events);
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  ASSERT_EQ(midi->notes.size(), 1u);
  EXPECT_EQ(midi->notes[0].channel, 9);

  auto pcm = RenderMidiToPcm(*midi, 22050);
  bool any_nonzero = false;
  for (int16_t s : pcm.samples) {
    if (s != 0) any_nonzero = true;
  }
  EXPECT_TRUE(any_nonzero) << "a percussion hit must be audible, not silent";

  // A real percussive hit decays away in well under the source note's
  // own (musically meaningless, for real drum note numbers) duration.
  size_t tail_samples = std::min(pcm.samples.size(), static_cast<size_t>(0.05 * pcm.sample_rate));
  double tail_rms = Rms(pcm.samples, pcm.samples.size() - tail_samples, pcm.samples.size());
  EXPECT_LT(tail_rms, 500.0) << "a real percussion hit must have decayed away well before the "
                                 "end of a half-second source note";
}

TEST(Midi, EmptyFileRendersWithoutCrashing) {
  auto bytes = BuildMidi(480, {});
  auto midi = ParseMidi(bytes.data(), bytes.size());
  ASSERT_TRUE(midi.has_value());
  EXPECT_TRUE(midi->notes.empty());
  auto pcm = RenderMidiToPcm(*midi, 22050);
  EXPECT_FALSE(pcm.samples.empty());  // never a zero-length buffer
}
