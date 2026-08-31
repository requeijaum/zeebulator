#include "core/brew/media_hle.h"

#include <zlib.h>

#include <sstream>

#include <gtest/gtest.h>

#include "core/audio/mixer.h"
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::IArmCore;
using zeebulator::kR0;
using zeebulator::kR1;
using zeebulator::MediaHle;
using zeebulator::Mixer;
using zeebulator::VirtualFilesystem;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kVtable = 0x80000000;
constexpr uint32_t kObjectRegion = 0x80001000;
constexpr uint32_t kScratch = 0x00090000;

// Real MM_PARM_* constants -- see core/brew/media_hle.cpp.
constexpr uint32_t kParmMediaData = 1;
constexpr uint32_t kParmVolume = 4;
constexpr uint32_t kParmPlayRepeat = 11;
constexpr uint32_t kMmdFileName = 0;
constexpr uint32_t kMmdBuffer = 1;

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
void AppendTag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

std::vector<uint8_t> BuildMonoPcmWav(uint32_t sample_rate, const std::vector<int16_t>& samples) {
  std::vector<uint8_t> pcm;
  for (int16_t s : samples) AppendU16LE(pcm, static_cast<uint16_t>(s));

  std::vector<uint8_t> body;
  AppendTag(body, "fmt ");
  AppendU32LE(body, 16);
  AppendU16LE(body, 1);  // PCM
  AppendU16LE(body, 1);  // mono
  AppendU32LE(body, sample_rate);
  AppendU32LE(body, sample_rate * 2);
  AppendU16LE(body, 2);
  AppendU16LE(body, 16);
  AppendTag(body, "data");
  AppendU32LE(body, static_cast<uint32_t>(pcm.size()));
  body.insert(body.end(), pcm.begin(), pcm.end());

  std::vector<uint8_t> out;
  AppendTag(out, "RIFF");
  AppendU32LE(out, static_cast<uint32_t>(4 + body.size()));
  AppendTag(out, "WAVE");
  out.insert(out.end(), body.begin(), body.end());
  return out;
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

// A minimal single-note format-0 MIDI file -- see tests/midi_test.cpp
// for the thoroughly-tested parser/synth this exercises through MediaHle.
std::vector<uint8_t> BuildOneNoteMidi() {
  std::vector<uint8_t> track;
  AppendVlq(track, 0);
  track.insert(track.end(), {0x90, 69, 100});  // Note On, A4, vel 100
  AppendVlq(track, 480);
  track.insert(track.end(), {0x80, 69, 0});  // Note Off
  AppendVlq(track, 0);
  track.insert(track.end(), {0xFF, 0x2F, 0x00});  // End of Track

  std::vector<uint8_t> out;
  AppendTag(out, "MThd");
  AppendU32BE(out, 6);
  AppendU16BE(out, 0);    // format 0
  AppendU16BE(out, 1);    // 1 track
  AppendU16BE(out, 480);  // division
  AppendTag(out, "MTrk");
  AppendU32BE(out, static_cast<uint32_t>(track.size()));
  out.insert(out.end(), track.begin(), track.end());
  return out;
}

// Real gzip compression -- see core/brew/media_hle.cpp's own Gunzip
// (and ModRuntime::DecompressGzipInPlaceImpl, the other real gzip
// consumer this project already has) for why real sound.ggz entries
// need this: Double Dragon's own custom loader hands SetMediaParm raw,
// still-gzip-compressed bytes, not a decoded container.
std::vector<uint8_t> Gzip(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out(data.size() + 128);
  z_stream strm{};
  EXPECT_EQ(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY),
            Z_OK);
  strm.next_in = const_cast<Bytef*>(data.data());
  strm.avail_in = static_cast<uInt>(data.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  out.resize(out.size() - strm.avail_out);
  deflateEnd(&strm);
  return out;
}

void WriteCString(zeebulator::Memory& mem, uint32_t addr, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    mem.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
  }
  mem.Write8(addr + static_cast<uint32_t>(s.size()), 0);
}

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  VirtualFilesystem vfs;
  Mixer mixer{22050};
  MediaHle media_hle{cpu.GetMemory(), hle, vfs, mixer, kObjectRegion};

  Fixture() {
    vfs.AddFile("tone.wav", BuildMonoPcmWav(22050, {100, 200, 300, 400}));
    vfs.AddFile("tone.mid", BuildOneNoteMidi());
    media_hle.Build(kVtable);
  }

  uint32_t Slot(uint32_t slot) { return cpu.GetMemory().Read32(kVtable + slot * 4); }

  // Writes an AEEMediaData{clsData=MMD_FILE_NAME, pData=filename, dwSize=0}
  // struct at kScratch and returns its address.
  uint32_t WriteMediaData(const std::string& filename) {
    uint32_t name_addr = kScratch + 0x100;
    WriteCString(cpu.GetMemory(), name_addr, filename);
    uint32_t md_addr = kScratch;
    cpu.GetMemory().Write32(md_addr + 0, kMmdFileName);
    cpu.GetMemory().Write32(md_addr + 4, name_addr);
    cpu.GetMemory().Write32(md_addr + 8, 0);
    return md_addr;
  }

  // Writes an AEEMediaData{clsData=MMD_BUFFER, pData=<raw bytes>,
  // dwSize=data.size()} struct at kScratch and returns its address --
  // the real shape Double Dragon's own custom sound.ggz loader uses
  // (TASKS.md/PHASE8_LOG.md Phase 8, the sound investigation).
  uint32_t WriteMediaDataBuffer(const std::vector<uint8_t>& data) {
    uint32_t data_addr = kScratch + 0x100;
    for (size_t i = 0; i < data.size(); ++i) {
      cpu.GetMemory().Write8(data_addr + static_cast<uint32_t>(i), data[i]);
    }
    uint32_t md_addr = kScratch;
    cpu.GetMemory().Write32(md_addr + 0, kMmdBuffer);
    cpu.GetMemory().Write32(md_addr + 4, data_addr);
    cpu.GetMemory().Write32(md_addr + 8, static_cast<uint32_t>(data.size()));
    return md_addr;
  }
};

class RecordingBackend : public zeebulator::Backend {
 public:
  void PushVideoFrame(const void*, int, int, zeebulator::PixelFormat) override {}
  zeebulator::ZPadState PollInput() override { return {}; }
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count, int) override {
    last_frames.assign(interleaved_stereo, interleaved_stereo + frame_count * 2);
  }
  std::vector<int16_t> last_frames;
};

}  // namespace

// Slot indices, matching AEEIMedia.h's real order (see media_hle.cpp).
enum Slots {
  kRegisterNotify = 3,
  kSetMediaParm = 4,
  kGetMediaParm = 5,
  kPlay = 6,
  kRecord = 7,
  kStop = 8,
  kSeek = 9,
  kPause = 10,
  kResume = 11,
  kGetTotalTime = 12,
  kGetState = 13,
};

TEST(MediaHle, VtableHasAllFourteenRealSlots) {
  Fixture f;
  for (uint32_t slot = 0; slot < 14; ++slot) {
    EXPECT_NE(f.Slot(slot), 0u) << "slot " << slot << " missing";
  }
}

TEST(MediaHle, SetMediaDataOnMissingFileFails) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("nope.wav");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  EXPECT_NE(result, 0u);
}

TEST(MediaHle, SetMediaDataThenGetStateReportsReady) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  EXPECT_EQ(result, 0u);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 2u) << "MM_STATE_READY";
}

TEST(MediaHle, SetMediaDataDispatchesMidFilesToMidiSynthAndPlaysThem) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.mid");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  ASSERT_EQ(result, 0u) << "a .mid file should decode via the MIDI synth path, not fail as PCM";

  uint32_t play_result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(play_result, 0u);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY";

  uint32_t ms = f.hle.CallArmFunction(f.Slot(kGetTotalTime), obj);
  EXPECT_NEAR(ms, 500u, 50u) << "one quarter note at the default 120 BPM tempo is ~500ms";
}

TEST(MediaHle, SetMediaDataWithMmdBufferDecodesAnUncompressedRawWavBuffer) {
  // Real evidence (TASKS.md/PHASE8_LOG.md Phase 8, the sound
  // investigation): Double Dragon's own custom sound.ggz loader hands
  // SetMediaParm a raw in-memory buffer (clsData=1/MMD_BUFFER), not a
  // filename -- confirmed live, not guessed.
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaDataBuffer(BuildMonoPcmWav(22050, {100, 200, 300, 400}));
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  EXPECT_EQ(result, 0u);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 2u) << "MM_STATE_READY";
}

TEST(MediaHle, SetMediaDataWithMmdBufferDecodesAnUncompressedRawMidiBuffer) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaDataBuffer(BuildOneNoteMidi());
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  ASSERT_EQ(result, 0u) << "a raw MIDI buffer should be sniffed by its MThd magic, not rejected";

  uint32_t ms = f.hle.CallArmFunction(f.Slot(kGetTotalTime), obj);
  EXPECT_NEAR(ms, 500u, 50u);
}

TEST(MediaHle, SetMediaDataWithMmdBufferDecompressesARealGzipCompressedBuffer) {
  // The real, live-confirmed shape: Double Dragon's own sound.ggz
  // entries are gzip-compressed (a real buffer's first bytes are the
  // real gzip magic, with the original filename visible in the
  // header's FNAME field) -- this is the scenario that actually
  // matters for the real game, not just a synthetic raw buffer.
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaDataBuffer(Gzip(BuildOneNoteMidi()));
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  ASSERT_EQ(result, 0u) << "a gzip-compressed MIDI buffer should decompress then decode, not fail";

  uint32_t play_result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(play_result, 0u);
  uint32_t ms = f.hle.CallArmFunction(f.Slot(kGetTotalTime), obj);
  EXPECT_NEAR(ms, 500u, 50u);
}

TEST(MediaHle, SetMediaDataWithAnUnsupportedClsDataFails) {
  // MMD_ISOURCE (2) -- no real evidence this title needs it, so it's
  // correctly rejected rather than silently misinterpreted as a raw
  // buffer or a filename.
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md_addr = kScratch;
  f.cpu.GetMemory().Write32(md_addr + 0, 2);
  f.cpu.GetMemory().Write32(md_addr + 4, 0);
  f.cpu.GetMemory().Write32(md_addr + 8, 0);
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md_addr, 0);
  EXPECT_NE(result, 0u);
}

TEST(MediaHle, PlayWithoutMediaDataFails) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_NE(result, 0u);
}

TEST(MediaHle, PlayStartsAMixerVoiceThatMixesRealDecodedSamples) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);

  uint32_t play_result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(play_result, 0u);

  // GetState should report PLAY (3), with pbStateChanging == FALSE.
  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY";
  EXPECT_EQ(f.cpu.GetMemory().Read32(pb_addr), 0u);
}

TEST(MediaHle, SetVolumeIsReflectedByGetVolume) {
  // Real MM_PARM_VOLUME used to be accepted and silently discarded --
  // see MediaHle::SetMediaParmImpl's own doc comment.
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();

  uint32_t p1_addr = kScratch + 0x300;
  f.hle.CallArmFunction(f.Slot(kGetMediaParm), obj, kParmVolume, p1_addr, 0);
  EXPECT_EQ(f.cpu.GetMemory().Read32(p1_addr), 100u) << "real AEE_MAX_VOLUME default";

  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmVolume, 30, 0);
  f.hle.CallArmFunction(f.Slot(kGetMediaParm), obj, kParmVolume, p1_addr, 0);
  EXPECT_EQ(f.cpu.GetMemory().Read32(p1_addr), 30u);
}

TEST(MediaHle, SetVolumeActuallyReducesMixedOutputAmplitude) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmVolume, 50, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  RecordingBackend backend;
  f.mixer.Mix(backend, 1);

  EXPECT_EQ(backend.last_frames[0], 50) << "tone.wav's first real sample is 100 -- at 50% volume "
                                            "that must reach the Mixer's own output as 50";
}

TEST(MediaHle, StopEndsPlaybackAndReturnsToReadyState) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  f.hle.CallArmFunction(f.Slot(kStop), obj);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 2u) << "MM_STATE_READY";
}

TEST(MediaHle, PauseThenResumeReturnsToPlayState) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  f.hle.CallArmFunction(f.Slot(kPause), obj);
  uint32_t pb_addr = kScratch + 0x200;
  EXPECT_EQ(f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr), 5u) << "MM_STATE_PLAY_PAUSE";

  f.hle.CallArmFunction(f.Slot(kResume), obj);
  EXPECT_EQ(f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr), 3u) << "MM_STATE_PLAY";
}

TEST(MediaHle, GetTotalTimeComputesCorrectMilliseconds) {
  // 4 samples at 22050 Hz -> 4/22050 s ~= 0.1814ms rounds down to 0 with
  // integer math at this tiny size; use a fixture-independent larger
  // synthetic file instead so the math is exact and unambiguous.
  VirtualFilesystem local_vfs;
  std::vector<int16_t> samples(22050, 0);  // exactly 1 second at 22050Hz
  local_vfs.AddFile("one_sec.wav", BuildMonoPcmWav(22050, samples));
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  Mixer mixer(22050);
  MediaHle media_hle(cpu.GetMemory(), hle, local_vfs, mixer, kObjectRegion);
  media_hle.Build(kVtable);
  uint32_t local_obj = media_hle.CreateMediaObject();

  uint32_t name_addr = kScratch + 0x100;
  WriteCString(cpu.GetMemory(), name_addr, "one_sec.wav");
  cpu.GetMemory().Write32(kScratch + 0, kMmdFileName);
  cpu.GetMemory().Write32(kScratch + 4, name_addr);
  cpu.GetMemory().Write32(kScratch + 8, 0);
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetMediaParm * 4), local_obj,
                       kParmMediaData, kScratch, 0);

  uint32_t ms = hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kGetTotalTime * 4), local_obj);
  EXPECT_EQ(ms, 1000u);
}

TEST(MediaHle, PlayRepeatZeroLoopsIndefinitelyInTheMixer) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmPlayRepeat, 0, 0);  // loop forever
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  class NullBackend : public zeebulator::Backend {
   public:
    void PushVideoFrame(const void*, int, int, zeebulator::PixelFormat) override {}
    void PushAudioSamples(const int16_t*, size_t, int) override {}
    zeebulator::ZPadState PollInput() override { return {}; }
  } backend;

  // Mix far more frames than the 4-sample clip has -- a non-looping
  // voice would finish and stop contributing; a looping one keeps going.
  f.mixer.Mix(backend, 100);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY -- looping voices never finish on their own";
}

TEST(MediaHle, RejectsNonPcmWavInsteadOfMisdecoding) {
  Fixture f;
  VirtualFilesystem vfs;
  std::vector<uint8_t> body;
  AppendTag(body, "fmt ");
  AppendU32LE(body, 16);
  AppendU16LE(body, 0x0011);  // IMA ADPCM, not PCM
  AppendU16LE(body, 1);
  AppendU32LE(body, 22050);
  AppendU32LE(body, 22050);
  AppendU16LE(body, 1);
  AppendU16LE(body, 4);
  AppendTag(body, "data");
  AppendU32LE(body, 4);
  body.push_back(1);
  body.push_back(2);
  body.push_back(3);
  body.push_back(4);
  std::vector<uint8_t> wav;
  AppendTag(wav, "RIFF");
  AppendU32LE(wav, static_cast<uint32_t>(4 + body.size()));
  AppendTag(wav, "WAVE");
  wav.insert(wav.end(), body.begin(), body.end());
  vfs.AddFile("adpcm.wav", wav);

  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  Mixer mixer(22050);
  MediaHle media_hle(cpu.GetMemory(), hle, vfs, mixer, kObjectRegion);
  media_hle.Build(kVtable);
  uint32_t obj = media_hle.CreateMediaObject();

  uint32_t name_addr = kScratch + 0x100;
  WriteCString(cpu.GetMemory(), name_addr, "adpcm.wav");
  cpu.GetMemory().Write32(kScratch + 0, kMmdFileName);
  cpu.GetMemory().Write32(kScratch + 4, name_addr);
  cpu.GetMemory().Write32(kScratch + 8, 0);
  uint32_t result = hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetMediaParm * 4), obj,
                                         kParmMediaData, kScratch, 0);
  EXPECT_NE(result, 0u);
}

// Real Double Dragon code registers a notify callback for every sound
// object and relies on it firing (MM_CMD_PLAY/MM_STATUS_DONE) to reset
// its own per-channel priority bookkeeping once a sound finishes --
// confirmed via live disassembly of the real registered callback
// (`ddragonz.mod` 0x11d020/0x11f4dc, see MediaHle's class doc comment).
// Without Tick() actually firing it, a channel a high-priority sound
// once claimed stays permanently unusable by anything lower-priority --
// this is the real, live-reproduced "sound effects stop firing the
// longer you play" bug this whole investigation chased down.
TEST(MediaHle, TickFiresRegisteredNotifyWithDoneStatusOnceAVoiceFinishes) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);

  int notify_calls = 0;
  uint32_t notify_user_seen = 0;
  uint32_t notify_cmd_seen = 0;
  uint32_t notify_status_seen = 0;
  uint32_t notify_fn = f.hle.Register([&](IArmCore& core) {
    ++notify_calls;
    notify_user_seen = core.GetRegister(kR0);
    uint32_t notify_struct = core.GetRegister(kR1);
    notify_cmd_seen = f.cpu.GetMemory().Read32(notify_struct + 8);
    notify_status_seen = f.cpu.GetMemory().Read32(notify_struct + 16);
    core.SetRegister(kR0, 0);
  });

  constexpr uint32_t kUserData = 0x12345678;
  f.hle.CallArmFunction(f.Slot(kRegisterNotify), obj, notify_fn, kUserData);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  // Not finished yet -- Tick() shouldn't fire anything.
  f.media_hle.Tick();
  EXPECT_EQ(notify_calls, 0);

  // Drain the whole (very short, 4-sample) clip.
  RecordingBackend backend;
  f.mixer.Mix(backend, 100);

  f.media_hle.Tick();
  EXPECT_EQ(notify_calls, 1);
  EXPECT_EQ(notify_user_seen, kUserData);
  EXPECT_EQ(notify_cmd_seen, 4u) << "MM_CMD_PLAY";
  EXPECT_EQ(notify_status_seen, 2u) << "MM_STATUS_DONE";

  // Ticking again shouldn't re-fire for the same already-consumed voice.
  f.media_hle.Tick();
  EXPECT_EQ(notify_calls, 1);
}

// Real per-character sound channels are a small shared pool: a new,
// higher-priority sound reclaims an already-playing channel by calling
// Play() again on the same IMedia object mid-playback, not by
// Stop()ping it first (see MediaHle's own class doc comment) -- so the
// interrupted voice never finishes naturally and Tick() alone would
// never notice it once Play() overwrites media.voice. Without PlayImpl
// itself firing an ABORT notification for the old voice first, that
// channel's real priority-stamp reset never happens -- the same "sound
// effects stop firing" bug class Tick()'s own DONE-on-finish fix
// covers, just for the "interrupted" case instead of "finished".
TEST(MediaHle, PlayReclaimingAnAlreadyPlayingVoiceFiresAbortNotificationFirst) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);

  int notify_calls = 0;
  uint32_t notify_status_seen = 0;
  uint32_t notify_fn = f.hle.Register([&](IArmCore& core) {
    ++notify_calls;
    uint32_t notify_struct = core.GetRegister(kR1);
    notify_status_seen = f.cpu.GetMemory().Read32(notify_struct + 16);
    core.SetRegister(kR0, 0);
  });
  f.hle.CallArmFunction(f.Slot(kRegisterNotify), obj, notify_fn, 0);

  f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(notify_calls, 0) << "starting the first play shouldn't notify anything";

  // Reclaim the same channel for a new sound before the first one had
  // any chance to finish naturally.
  f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(notify_calls, 1);
  EXPECT_EQ(notify_status_seen, 3u) << "MM_STATUS_ABORT";

  // The new voice is genuinely playing -- Tick() shouldn't immediately
  // fire a second, spurious notification for it.
  f.media_hle.Tick();
  EXPECT_EQ(notify_calls, 1);
}

TEST(MediaHle, TickWithNoRegisteredNotifyDoesNotCrash) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  RecordingBackend backend;
  f.mixer.Mix(backend, 100);
  f.media_hle.Tick();

  uint32_t pb_addr = kScratch + 0x200;
  EXPECT_EQ(f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr), 2u) << "MM_STATE_READY";
}

// --- Real state persistence past this process's own lifetime ---
// (see Mixer::Serialize's own doc comment for the real bug this fixes
// together with it: a guest reusing an IMedia handle it created before
// a save state was taken found no record of it at all after loading
// one, since this class's own media_by_object_ was purely in-memory --
// real, live-reproduced total silence after a save-state load, not a
// transient glitch).

TEST(MediaHle, SerializeThenDeserializeRoundTripsAPlayableMediaObject) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  std::stringstream mixer_stream;
  std::stringstream media_stream;
  ASSERT_TRUE(f.mixer.Serialize(mixer_stream));
  ASSERT_TRUE(f.media_hle.Serialize(media_stream));

  // A completely fresh instance, matching a real cold `--load-state`.
  Fixture f2;
  ASSERT_TRUE(f2.mixer.Deserialize(mixer_stream));
  ASSERT_TRUE(f2.media_hle.Deserialize(media_stream));

  // Same guest object address, but f2 never itself called
  // CreateMediaObject/SetMediaParm/Play on it -- matching how a real
  // guest reuses a handle it already had from before the save state.
  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f2.hle.CallArmFunction(f2.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY, restored";

  RecordingBackend backend;
  f2.mixer.Mix(backend, 1);
  EXPECT_NE(backend.last_frames[0], 0) << "restored voice should keep producing real audio";
}

TEST(MediaHle, DeserializeOnATruncatedStreamFailsWithoutCrashing) {
  Fixture f;
  std::stringstream stream;
  stream.write("\x00\x00\x09\x00", 4);  // next_object_address_, then nothing else
  EXPECT_FALSE(f.media_hle.Deserialize(stream));
}
