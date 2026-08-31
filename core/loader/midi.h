#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/loader/wav.h"

namespace zeebulator {

// One note, already merged across all tracks/channels into a single
// absolute-tick timeline.
struct MidiNote {
  uint32_t start_tick;
  uint32_t end_tick;
  int note;      // MIDI note number, 0-127 (69 = A4 = 440Hz)
  int velocity;  // 0-127
  int channel;   // 0-15; channel 9 is the real GM percussion channel
  int program;   // 0-127, the real GM instrument active on this note's
                 // channel via the most recent Program Change event
                 // before this note started (0 -- Acoustic Grand Piano
                 // -- if the file never sent one, per the GM default).
};

struct MidiFile {
  int division = 0;  // ticks per quarter note (SMPTE-timecode division is not supported)
  std::vector<MidiNote> notes;  // merged across all tracks, start_tick-ordered
  // (tick, microseconds_per_quarter_note) tempo-change points, tick-ordered,
  // always has an entry at tick 0 (defaulting to 120 BPM if the file
  // never sets one explicitly, per the SMF spec).
  std::vector<std::pair<uint32_t, uint32_t>> tempo_changes;
};

// Converts an absolute tick position to real seconds, honoring every
// real tempo change up to that point. Shared by this file's own
// hand-rolled synth (`RenderMidiToPcm`) and
// `core/audio/soundfont_synth.cpp`'s real soundfont-based one -- both
// need the same real tick->time conversion to place real note events
// at the right real moment.
double TickToSeconds(uint32_t tick, int division,
                      const std::vector<std::pair<uint32_t, uint32_t>>& tempo_changes);

// Parses a Standard MIDI File, format 0 or 1 (format 2 -- independent
// unsynced sequences -- and SMPTE-timecode division are explicitly
// rejected rather than mis-parsed; real Double Dragon .mid files are
// format 0, confirmed -- see TASKS.md Phase 6). Returns nullopt for
// anything malformed or unsupported.
std::optional<MidiFile> ParseMidi(const uint8_t* data, size_t size);

// Renders a parsed MIDI file to PCM with a simple synthesizer: every
// melodic note becomes a tone at its correct pitch and real-time
// position (ticks converted to seconds via the tempo map), amplitude
// scaled by velocity. Real GM channel-10 percussion notes
// (`channel == 9`) are skipped entirely rather than rendered as pitched
// tones -- their note numbers mean specific drum sounds, not pitches,
// and running them through the pitched synthesizer produced real,
// confirmed-live spurious low-frequency content (heard as "extremely
// deep and very loud" background music, PHASE8_LOG.md's "Sound, round
// twelve") once real gameplay music was actually reachable for the
// first time.
//
// Each note's own real GM program number (`MidiNote::program`) selects
// one of a small number of waveform/envelope archetypes grouped by the
// real, standardized GM program-number families (piano, organ, guitar,
// bass, strings, brass, etc. -- see `TimbreForProgram` in the .cpp):
// plucked/percussive families (piano, guitar, bass, chromatic
// percussion, ethnic, percussive) get a triangle or sawtooth wave with
// a real exponential decay envelope instead of a flat sustain, since a
// real piano/guitar/bass note audibly loses energy over its own
// duration even while held; sustained families (organ, strings, brass,
// pads, etc.) keep a flat sustain with a short linear fade in/out.
// Still deliberately no real sampled-instrument or soundfont-based
// rendering (a much larger undertaking than this synthesizer's own
// documented scope) -- this is a better-shaped crude synthesizer, not
// a full General MIDI implementation.
WavAudio RenderMidiToPcm(const MidiFile& midi, int sample_rate);

}  // namespace zeebulator
