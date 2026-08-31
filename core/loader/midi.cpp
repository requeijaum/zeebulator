#include "core/loader/midi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace zeebulator {

namespace {

uint32_t ReadU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t ReadU16BE(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// Reads a MIDI variable-length quantity (7 bits per byte, MSB = "more
// bytes follow"), advancing `pos`. Returns 0 (and leaves pos unmoved
// past the end) if the buffer runs out before a terminating byte.
uint32_t ReadVlq(const uint8_t* data, size_t size, size_t& pos) {
  uint32_t value = 0;
  while (pos < size) {
    uint8_t byte = data[pos++];
    value = (value << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) break;
  }
  return value;
}

constexpr uint32_t kDefaultMicrosecondsPerQuarter = 500000;  // 120 BPM, the SMF-spec default
constexpr double kPi = 3.14159265358979323846;

}  // namespace

std::optional<MidiFile> ParseMidi(const uint8_t* data, size_t size) {
  if (size < 14 || std::memcmp(data, "MThd", 4) != 0) return std::nullopt;
  uint32_t header_len = ReadU32BE(data + 4);
  if (header_len < 6 || 8 + header_len > size) return std::nullopt;

  uint16_t format = ReadU16BE(data + 8);
  uint16_t num_tracks = ReadU16BE(data + 10);
  uint16_t division = ReadU16BE(data + 12);
  if (format > 1) return std::nullopt;             // format 2 not supported
  if ((division & 0x8000) != 0) return std::nullopt;  // SMPTE-timecode division not supported

  MidiFile midi;
  midi.division = division;

  // (channel, note) -> (start_tick, velocity, program active at Note On
  // time -- a later Program Change on the same channel before this
  // note's own Note Off must not retroactively change its timbre, so
  // this is captured once, not looked up again later), used to pair
  // Note On with the Note Off that ends it.
  std::map<std::pair<int, int>, std::tuple<uint32_t, int, int>> active_notes;
  // Real GM default: every channel starts on program 0 (Acoustic Grand
  // Piano) until its own Program Change event says otherwise. Scoped to
  // the whole file, not per-track, since a real channel's active
  // program is a file-wide (per-channel) concept even in format-1 files
  // with one channel spread across multiple tracks.
  int channel_program[16] = {0};

  size_t pos = 8 + header_len;
  for (uint16_t t = 0; t < num_tracks && pos + 8 <= size; ++t) {
    if (std::memcmp(data + pos, "MTrk", 4) != 0) return std::nullopt;
    uint32_t track_len = ReadU32BE(data + pos + 4);
    size_t track_start = pos + 8;
    size_t track_end = track_start + track_len;
    if (track_end > size) return std::nullopt;

    size_t p = track_start;
    uint32_t tick = 0;
    uint8_t running_status = 0;

    while (p < track_end) {
      tick += ReadVlq(data, track_end, p);
      if (p >= track_end) break;

      uint8_t status = data[p];
      if (status < 0x80) {
        status = running_status;  // running status: this byte is data, not a new status
      } else {
        ++p;
      }
      running_status = status;

      if (status == 0xFF) {  // meta event
        if (p >= track_end) break;
        uint8_t meta_type = data[p++];
        uint32_t len = ReadVlq(data, track_end, p);
        if (p + len > track_end) break;
        if (meta_type == 0x51 && len == 3) {  // Set Tempo
          uint32_t us_per_quarter = (static_cast<uint32_t>(data[p]) << 16) |
                                     (static_cast<uint32_t>(data[p + 1]) << 8) | data[p + 2];
          midi.tempo_changes.emplace_back(tick, us_per_quarter);
        }
        p += len;
      } else if (status == 0xF0 || status == 0xF7) {  // sysex
        uint32_t len = ReadVlq(data, track_end, p);
        if (p + len > track_end) break;
        p += len;
      } else {
        uint8_t event_type = status & 0xF0;
        int channel = status & 0x0F;
        if (event_type == 0x80 || event_type == 0x90) {  // note off / note on
          if (p + 2 > track_end) break;
          int note = data[p];
          int velocity = data[p + 1];
          p += 2;
          bool is_off = (event_type == 0x80) || (event_type == 0x90 && velocity == 0);
          auto key = std::make_pair(channel, note);
          if (is_off) {
            auto it = active_notes.find(key);
            if (it != active_notes.end()) {
              auto& [start_tick, start_velocity, program] = it->second;
              midi.notes.push_back(
                  MidiNote{start_tick, tick, note, start_velocity, channel, program});
              active_notes.erase(it);
            }
          } else {
            active_notes[key] = {tick, velocity, channel_program[channel]};
          }
        } else if (event_type == 0xC0) {  // Program Change
          if (p + 1 > track_end) break;
          channel_program[channel] = data[p];
          p += 1;
        } else if (event_type == 0xD0) {  // Channel Pressure -- 1 data byte, unused
          if (p + 1 > track_end) break;
          p += 1;
        } else {  // 0xA0/0xB0/0xE0: 2 data bytes
          if (p + 2 > track_end) break;
          p += 2;
        }
      }
    }
    // Any notes never explicitly turned off end at the track's last tick.
    for (auto& [key, start] : active_notes) {
      auto& [start_tick, start_velocity, program] = start;
      midi.notes.push_back(
          MidiNote{start_tick, tick, key.second, start_velocity, key.first, program});
    }
    active_notes.clear();

    pos = track_end;
  }

  if (midi.tempo_changes.empty() || midi.tempo_changes[0].first != 0) {
    midi.tempo_changes.insert(midi.tempo_changes.begin(), {0, kDefaultMicrosecondsPerQuarter});
  }
  std::stable_sort(midi.tempo_changes.begin(), midi.tempo_changes.end());
  std::stable_sort(midi.notes.begin(), midi.notes.end(),
                    [](const MidiNote& a, const MidiNote& b) { return a.start_tick < b.start_tick; });

  return midi;
}

// Converts an absolute tick position to seconds, honoring every tempo
// change up to that point (a real Double Dragon .mid changes tempo mid-
// file in at least the "_l"/"_0" loop-point variants observed -- see
// TASKS.md Phase 6). Declared in midi.h -- shared by this file's own
// hand-rolled synth and core/audio/soundfont_synth.cpp's real
// soundfont-based one, since both need the same real tick->time
// conversion.
double TickToSeconds(uint32_t tick, int division,
                     const std::vector<std::pair<uint32_t, uint32_t>>& tempo_changes) {
  double seconds = 0.0;
  for (size_t i = 0; i < tempo_changes.size(); ++i) {
    uint32_t segment_start = tempo_changes[i].first;
    uint32_t segment_end = (i + 1 < tempo_changes.size()) ? tempo_changes[i + 1].first : tick;
    if (segment_start >= tick) break;
    uint32_t ticks_in_segment = std::min(segment_end, tick) - segment_start;
    double seconds_per_tick = (tempo_changes[i].second / 1'000'000.0) / division;
    seconds += ticks_in_segment * seconds_per_tick;
    if (segment_end >= tick) break;
  }
  return seconds;
}

namespace {

float NoteFrequency(int note) { return 440.0f * std::pow(2.0f, (note - 69) / 12.0f); }

enum class Waveform { kSine, kTriangle, kSquare, kSawtooth };
enum class EnvelopeShape {
  kSustain,  // flat until release, short linear fade in/out -- organ/strings/pad-like
  kPluck,    // fast attack then a real exponential decay over the note's own
             // duration, independent of note-off -- piano/guitar/bass-like
};

struct Timbre {
  Waveform waveform;
  EnvelopeShape envelope;
};

// Maps a real GM program number (0-127) to a waveform/envelope
// archetype by the real, standardized GM program-number family
// groupings (every family is a contiguous 8-program block, in this
// fixed order, per the GM1 Sound Set spec) -- not per-instrument
// fidelity, just enough differentiation that a bassline doesn't sound
// identical to a lead or a pad. The Guitar family is split three ways
// by real playing characteristics, not just "guitar vs. not": real
// acoustic/clean guitar (24-27) decays quickly (`kPluck`), real muted
// guitar (28) is deliberately even shorter/percussive, but real
// overdriven/distortion guitar (29-31) actually *sustains* notes --
// real electric amp overdrive/distortion compresses and sustains, it
// doesn't decay like a plucked acoustic string. Confirmed live
// (PHASE8_LOG.md's "Sound, round twelve") those are the exact real
// programs Double Dragon's own melody channels use, and giving them a
// fast pluck decay let long, sustained real guitar notes fall to
// near-silence for most of their own real duration -- heard as the
// guitar "missing" even though it was technically still playing.
Timbre TimbreForProgram(int program) {
  if (program < 8) return {Waveform::kTriangle, EnvelopeShape::kPluck};     // Piano
  if (program < 16) return {Waveform::kTriangle, EnvelopeShape::kPluck};    // Chromatic Percussion
  if (program < 24) return {Waveform::kSquare, EnvelopeShape::kSustain};    // Organ
  if (program < 27) return {Waveform::kSawtooth, EnvelopeShape::kPluck};    // Guitar (acoustic/jazz)
  if (program < 28) return {Waveform::kSawtooth, EnvelopeShape::kSustain};  // Guitar (clean, electric)
  if (program < 29) return {Waveform::kSquare, EnvelopeShape::kPluck};      // Guitar (muted)
  if (program < 32) return {Waveform::kSquare, EnvelopeShape::kSustain};    // Guitar (overdriven/distortion/harmonics)
  if (program < 40) return {Waveform::kSine, EnvelopeShape::kPluck};        // Bass
  if (program < 48) return {Waveform::kSawtooth, EnvelopeShape::kSustain};  // Strings
  if (program < 56) return {Waveform::kSawtooth, EnvelopeShape::kSustain};  // Ensemble
  if (program < 64) return {Waveform::kSquare, EnvelopeShape::kSustain};    // Brass
  if (program < 72) return {Waveform::kSawtooth, EnvelopeShape::kSustain};  // Reed
  if (program < 80) return {Waveform::kSine, EnvelopeShape::kSustain};      // Pipe
  if (program < 88) return {Waveform::kSawtooth, EnvelopeShape::kSustain};  // Synth Lead
  if (program < 96) return {Waveform::kSine, EnvelopeShape::kSustain};      // Synth Pad
  if (program < 104) return {Waveform::kSine, EnvelopeShape::kSustain};     // Synth Effects
  if (program < 112) return {Waveform::kTriangle, EnvelopeShape::kPluck};   // Ethnic
  if (program < 120) return {Waveform::kTriangle, EnvelopeShape::kPluck};   // Percussive
  return {Waveform::kSine, EnvelopeShape::kSustain};                       // Sound Effects
}

// Highest harmonic number that still stays safely under Nyquist for a
// given fundamental -- real square/sawtooth waves have harmonics that
// extend indefinitely, and generating them naively (a single
// discontinuous waveform per sample) lets every harmonic above Nyquist
// alias back down into the audible range as real, confirmed-live
// crackle/popping. Additive (harmonic-summed) synthesis avoids this by
// construction: no harmonic above the cap is ever generated.
int MaxHarmonic(double freq, int sample_rate) {
  double nyquist = sample_rate * 0.45;  // a little under true Nyquist for headroom
  return std::clamp(static_cast<int>(nyquist / freq), 1, 15);
}

// `phase` is `frequency * t`, any non-negative real number of cycles
// (not pre-wrapped to a single cycle -- each waveform reduces it mod 1
// itself).
float GenerateWaveform(Waveform waveform, double phase, double freq, int sample_rate) {
  double cycle = phase - std::floor(phase);  // fractional position within the current cycle, [0,1)
  switch (waveform) {
    case Waveform::kSine:
      return static_cast<float>(std::sin(2.0 * kPi * phase));
    case Waveform::kTriangle:
      // Triangle's own real harmonics fall off as 1/k^2 (much faster
      // than square/sawtooth's 1/k), so the naive discontinuous-slope
      // formula doesn't produce audible aliasing the way the other two
      // do -- no additive synthesis needed here.
      return static_cast<float>(cycle < 0.5 ? (4.0 * cycle - 1.0) : (3.0 - 4.0 * cycle));
    case Waveform::kSquare: {
      int max_harmonic = MaxHarmonic(freq, sample_rate);
      double sum = 0.0;
      for (int k = 1; k <= max_harmonic; k += 2) {  // real square wave: odd harmonics only
        sum += std::sin(2.0 * kPi * k * phase) / k;
      }
      return static_cast<float>(sum * (4.0 / kPi));
    }
    case Waveform::kSawtooth: {
      int max_harmonic = MaxHarmonic(freq, sample_rate);
      double sum = 0.0;
      for (int k = 1; k <= max_harmonic; ++k) {  // real sawtooth: every harmonic, alternating sign
        double sign = (k % 2 == 0) ? -1.0 : 1.0;
        sum += sign * std::sin(2.0 * kPi * k * phase) / k;
      }
      return static_cast<float>(sum * (2.0 / kPi));
    }
  }
  return 0.0f;
}

enum class PercussionKind { kKick, kSnare, kHiHat, kOther };

// Classifies by the real, fixed GM percussion key map (channel 10's
// note numbers are a standardized instrument assignment, not
// pitches -- every General MIDI file uses these same note numbers for
// the same drum sounds). Only the note numbers common enough to be
// worth a distinct real sound are classified; anything else falls back
// to a generic hit.
PercussionKind ClassifyPercussion(int note) {
  if (note == 35 || note == 36) return PercussionKind::kKick;           // Acoustic/Bass Drum
  if (note == 38 || note == 40) return PercussionKind::kSnare;          // Acoustic/Electric Snare
  if (note == 42 || note == 44 || note == 46 ||                        // closed/pedal/open Hi-Hat
      note == 49 || note == 51 || note == 55 || note == 57 || note == 59) {  // crash/ride/splash cymbals
    return PercussionKind::kHiHat;
  }
  return PercussionKind::kOther;
}

// A short, fast-decaying hit shaped by real GM drum-key classification
// -- a plausible stand-in for a real sampled drum hit (out of scope --
// see this class's own doc comment) that's audibly present and at
// least coarsely drum-*shaped* rather than either the original bug (a
// wrong pitched tone), complete silence (both read as a "missing
// instrument" once real gameplay music was reachable to listen to,
// PHASE8_LOG.md's "Sound, round twelve"), or one identical noise burst
// for every real drum sound (confirmed live: didn't read as "drums" at
// all). Real kicks are a real, characteristic descending pitch sweep,
// not noise; real hi-hats/cymbals are brief and bright (approximated
// with a real one-pole high-pass on the noise, removing low-frequency
// content); snares and anything unclassified stay a broadband noise
// burst. `rng_state` is threaded through by the caller so consecutive
// real hits don't all sound identical.
void RenderPercussionHit(std::vector<float>& mix, size_t start_sample, int note, int velocity,
                          int sample_rate, uint32_t& rng_state) {
  float amplitude = std::clamp(velocity / 127.0f, 0.0f, 1.0f) * 0.15f;
  PercussionKind kind = ClassifyPercussion(note);

  if (kind == PercussionKind::kKick) {
    constexpr double kKickSeconds = 0.15, kStartFreq = 150.0, kEndFreq = 50.0;
    double decay_rate = -std::log(0.05) / kKickSeconds;
    size_t hit_samples = static_cast<size_t>(kKickSeconds * sample_rate);
    double phase = 0.0;
    for (size_t i = 0; i < hit_samples && start_sample + i < mix.size(); ++i) {
      double t = static_cast<double>(i) / sample_rate;
      double freq = kStartFreq + (kEndFreq - kStartFreq) * (t / kKickSeconds);
      phase += freq / sample_rate;
      double env = std::exp(-decay_rate * t);
      mix[start_sample + i] +=
          amplitude * 1.3f * static_cast<float>(env * std::sin(2.0 * kPi * phase));
    }
    return;
  }

  double hit_seconds = (kind == PercussionKind::kHiHat) ? 0.05 : 0.12;
  double decay_rate = -std::log(0.05) / hit_seconds;
  size_t hit_samples = static_cast<size_t>(hit_seconds * sample_rate);
  float prev_noise = 0.0f;
  for (size_t i = 0; i < hit_samples && start_sample + i < mix.size(); ++i) {
    double t = static_cast<double>(i) / sample_rate;
    double env = std::exp(-decay_rate * t);
    // xorshift32 -- fast, deterministic (reproducible in tests), good
    // enough spectral quality for a short percussive noise burst.
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    float noise = (static_cast<float>(rng_state) / 4294967295.0f) * 2.0f - 1.0f;
    float sample = noise;
    if (kind == PercussionKind::kHiHat) {
      sample = noise - prev_noise;  // one-pole high-pass: brighter, "tsss"-like, not a dull thud
      prev_noise = noise;
    }
    mix[start_sample + i] += amplitude * static_cast<float>(env) * sample;
  }
}

}  // namespace

WavAudio RenderMidiToPcm(const MidiFile& midi, int sample_rate) {
  WavAudio out;
  out.sample_rate = sample_rate;
  out.channels = 1;

  if (midi.notes.empty() || midi.division <= 0) {
    out.samples.assign(1, 0);  // avoid a zero-length clip
    return out;
  }

  uint32_t last_tick = 0;
  for (const MidiNote& n : midi.notes) last_tick = std::max(last_tick, n.end_tick);
  double total_seconds = TickToSeconds(last_tick, midi.division, midi.tempo_changes);
  size_t total_samples = static_cast<size_t>(total_seconds * sample_rate) + 1;
  std::vector<float> mix(total_samples, 0.0f);

  constexpr int kPercussionChannel = 9;  // real GM channel 10 (0-indexed) -- see this class's own doc comment
  constexpr double kFadeSeconds = 0.01;  // linear fade in/out to avoid clicks between notes
  uint32_t percussion_rng = 0x9E3779B9u;  // any fixed nonzero xorshift32 seed
  for (const MidiNote& n : midi.notes) {
    double start_s = TickToSeconds(n.start_tick, midi.division, midi.tempo_changes);
    if (n.channel == kPercussionChannel) {
      auto start_sample = static_cast<size_t>(start_s * sample_rate);
      RenderPercussionHit(mix, start_sample, n.note, n.velocity, sample_rate, percussion_rng);
      continue;
    }
    double end_s = TickToSeconds(n.end_tick, midi.division, midi.tempo_changes);
    if (end_s <= start_s) continue;
    float freq = NoteFrequency(n.note);
    float amplitude = std::clamp(n.velocity / 127.0f, 0.0f, 1.0f) * 0.2f;  // headroom for polyphony
    double duration = end_s - start_s;
    double fade = std::min(kFadeSeconds, duration / 2.0);
    Timbre timbre = TimbreForProgram(n.program);
    // Exponential decay rate such that a plucked note has decayed to
    // ~15% of its initial amplitude by its own real end -- an audible,
    // piano/guitar/bass-like loss of energy over the note's own
    // duration, not just at release.
    double decay_rate = -std::log(0.15) / std::max(duration, 0.05);

    auto start_sample = static_cast<size_t>(start_s * sample_rate);
    auto end_sample = static_cast<size_t>(end_s * sample_rate);
    for (size_t i = start_sample; i < end_sample && i < mix.size(); ++i) {
      double t = (i - start_sample) / static_cast<double>(sample_rate);
      double env;
      if (t < fade) {
        env = t / fade;  // shared attack, every envelope shape
      } else if (timbre.envelope == EnvelopeShape::kPluck) {
        env = std::exp(-decay_rate * t);
      } else if (duration - t < fade) {
        env = (duration - t) / fade;
      } else {
        env = 1.0;
      }
      mix[i] += amplitude * static_cast<float>(env) *
                GenerateWaveform(timbre.waveform, freq * t, freq, sample_rate);
    }
  }

  // Post-mix normalization rather than a hard per-sample clamp: with
  // dense real polyphony (confirmed live, PHASE8_LOG.md's "Sound,
  // round twelve" -- up to 9 simultaneous real channels, ~1800 notes
  // over one real ~47s track), the fixed per-note headroom above still
  // let the summed mix regularly exceed full scale, and a hard clamp
  // there is audible clipping distortion (real, measured live: ~5-7%
  // of samples pinned at the int16 limit) -- heard as "extremely deep
  // and very loud" once real gameplay music was actually reachable for
  // the first time. Scaling the whole clip down by its own real peak
  // removes that distortion outright while leaving every note's
  // relative loudness to every other note exactly as rendered (a
  // uniform gain change, not a per-note or per-frequency guess).
  float peak = 0.0f;
  for (float v : mix) peak = std::max(peak, std::fabs(v));
  float gain = (peak > 1.0f) ? (1.0f / peak) : 1.0f;

  out.samples.resize(mix.size());
  for (size_t i = 0; i < mix.size(); ++i) {
    float v = std::clamp(mix[i] * gain, -1.0f, 1.0f) * 32767.0f;
    out.samples[i] = static_cast<int16_t>(v);
  }
  return out;
}

}  // namespace zeebulator
