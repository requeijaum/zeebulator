#include "core/audio/mixer.h"

#include <memory>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::Backend;
using zeebulator::Mixer;
using zeebulator::PixelFormat;
using zeebulator::ZPadState;

namespace {

class RecordingBackend : public Backend {
 public:
  void PushVideoFrame(const void*, int, int, PixelFormat) override {}
  ZPadState PollInput() override { return {}; }
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                         int sample_rate) override {
    ++push_count;
    last_sample_rate = sample_rate;
    last_frames.assign(interleaved_stereo, interleaved_stereo + frame_count * 2);
  }

  int push_count = 0;
  int last_sample_rate = 0;
  std::vector<int16_t> last_frames;
};

auto MakeMono(std::vector<int16_t> samples) {
  return std::make_shared<const std::vector<int16_t>>(std::move(samples));
}

}  // namespace

TEST(Mixer, SingleMonoVoiceDuplicatedToBothChannels) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({100, 200, 300}), /*channels=*/1, /*sample_rate=*/22050, /*loop=*/false);

  mixer.Mix(backend, 3);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_sample_rate, 22050);
  std::vector<int16_t> expected = {100, 100, 200, 200, 300, 300};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, StereoVoicePassesThroughUnmodified) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({10, 20, 30, 40}), /*channels=*/2, 22050, false);

  mixer.Mix(backend, 2);

  std::vector<int16_t> expected = {10, 20, 30, 40};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, TwoSimultaneousVoicesSum) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({100, 100}), 1, 22050, false);
  mixer.Play(MakeMono({50, -50}), 1, 22050, false);

  mixer.Mix(backend, 2);

  std::vector<int16_t> expected = {150, 150, 50, 50};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, OverlappingLoudVoicesClampInsteadOfWrapping) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({30000}), 1, 22050, false);
  mixer.Play(MakeMono({30000}), 1, 22050, false);  // sum = 60000, overflows int16

  mixer.Mix(backend, 1);

  EXPECT_EQ(backend.last_frames[0], 32767) << "must clamp, not wrap to negative";
  EXPECT_EQ(backend.last_frames[1], 32767);
}

TEST(Mixer, LoopingVoiceWrapsAndKeepsPlaying) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({1, 2}), 1, 22050, /*loop=*/true);

  // 5 frames from a 2-frame looping clip: 1,2,1,2,1
  mixer.Mix(backend, 5);
  std::vector<int16_t> expected = {1, 1, 2, 2, 1, 1, 2, 2, 1, 1};
  EXPECT_EQ(backend.last_frames, expected);
  EXPECT_TRUE(mixer.IsPlaying(id)) << "looping voices never finish on their own";
}

TEST(Mixer, NonLoopingVoiceFinishesAndStopsContributing) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({7, 8}), 1, 22050, /*loop=*/false);

  mixer.Mix(backend, 2);  // exactly consumes the clip
  EXPECT_FALSE(mixer.IsPlaying(id));

  mixer.Mix(backend, 2);  // nothing left to contribute
  std::vector<int16_t> expected_silence = {0, 0, 0, 0};
  EXPECT_EQ(backend.last_frames, expected_silence);
}

TEST(Mixer, PauseStopsAdvancingThenResumeContinuesFromSamePosition) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({10, 20, 30, 40}), 1, 22050, false);

  mixer.Mix(backend, 1);  // consumes sample 0 (value 10)
  mixer.Pause(id);
  mixer.Mix(backend, 1);  // paused: silence, position must not advance
  EXPECT_EQ(backend.last_frames[0], 0);

  mixer.Resume(id);
  mixer.Mix(backend, 1);  // should now emit sample index 1 (value 20), not 2
  EXPECT_EQ(backend.last_frames[0], 20);
}

TEST(Mixer, StopRemovesVoiceEntirely) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({100, 100}), 1, 22050, false);

  mixer.Stop(id);
  EXPECT_FALSE(mixer.IsPlaying(id));

  mixer.Mix(backend, 1);
  EXPECT_EQ(backend.last_frames[0], 0) << "stopped voice contributes nothing";
}

TEST(Mixer, VoiceAtHalfOutputRatePlaysBackAtHalfSpeed) {
  // A voice recorded at half the Mixer's output rate must take twice
  // as many output frames to fully play -- otherwise it's being read
  // frame-for-frame against the wrong rate (the real "extremely deep"
  // pitch bug this project shipped, PHASE8_LOG.md's "Sound, round
  // twelve"), not resampled.
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id =
      mixer.Play(MakeMono({0, 100, 200, 300}), /*channels=*/1, /*sample_rate=*/11025,
                 /*loop=*/false);

  mixer.Mix(backend, 7);  // not quite enough output frames to finish yet
  EXPECT_TRUE(mixer.IsPlaying(id));

  mixer.Mix(backend, 1);  // the 8th output frame finishes a 4-source-frame clip at half rate
  EXPECT_FALSE(mixer.IsPlaying(id));
}

TEST(Mixer, ResamplingLinearlyInterpolatesBetweenSourceFrames) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({0, 100}), /*channels=*/1, /*sample_rate=*/11025, /*loop=*/false);

  mixer.Mix(backend, 2);  // step=0.5: output frame 1 lands exactly halfway between 0 and 100
  EXPECT_EQ(backend.last_frames[0], 0);
  EXPECT_EQ(backend.last_frames[2], 50);
}

TEST(Mixer, PlayAtHalfVolumeHalvesOutputAmplitude) {
  // Real MM_PARM_VOLUME (see MediaHle::SetMediaParmImpl) used to be
  // accepted and silently discarded -- a real, dense soundfont-
  // rendered music voice drowning out real, much quieter SFX voices in
  // this shared Mixer (PHASE8_LOG.md's "Sound, round thirteen") is
  // exactly the real symptom actually applying it fixes.
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({1000}), 1, 22050, false, /*volume=*/50);

  mixer.Mix(backend, 1);

  EXPECT_EQ(backend.last_frames[0], 500);
  EXPECT_EQ(backend.last_frames[1], 500);
}

TEST(Mixer, PlayAtZeroVolumeIsSilent) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({30000}), 1, 22050, false, /*volume=*/0);

  mixer.Mix(backend, 1);

  EXPECT_EQ(backend.last_frames[0], 0);
  EXPECT_EQ(backend.last_frames[1], 0);
}

TEST(Mixer, SetVolumeChangesAnAlreadyPlayingVoice) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({1000, 1000}), 1, 22050, false);

  mixer.Mix(backend, 1);
  EXPECT_EQ(backend.last_frames[0], 1000) << "starts at the real default (full) volume";

  mixer.SetVolume(id, 20);
  mixer.Mix(backend, 1);
  EXPECT_EQ(backend.last_frames[0], 200) << "a live volume change must affect the very next Mix()";
}

TEST(Mixer, MixWithNoActiveVoicesPushesSilence) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Mix(backend, 4);
  ASSERT_EQ(backend.push_count, 1);
  std::vector<int16_t> expected(8, 0);
  EXPECT_EQ(backend.last_frames, expected);
}

// --- Real state persistence past this process's own lifetime ---
// (see Mixer::Serialize's own doc comment for the real bug this fixes:
// a save state used to only capture guest CPU/memory, so every voice
// already playing before the save was taken was just gone after
// loading one -- real, live-reproduced total silence, not a transient
// glitch).

TEST(Mixer, SerializeThenDeserializeRoundTripsAnActiveVoice) {
  Mixer mixer(22050);
  Mixer::VoiceId id =
      mixer.Play(MakeMono({100, 200, 300, 400}), /*channels=*/1, /*sample_rate=*/22050,
                 /*loop=*/true, /*volume=*/50);
  RecordingBackend backend;
  mixer.Mix(backend, 2);  // advance position_frames partway through

  std::stringstream stream;
  ASSERT_TRUE(mixer.Serialize(stream));

  Mixer mixer2(22050);
  ASSERT_TRUE(mixer2.Deserialize(stream));
  EXPECT_TRUE(mixer2.IsPlaying(id)) << "same voice id should survive the round trip";

  RecordingBackend backend2;
  mixer2.Mix(backend2, 2);
  EXPECT_NE(backend2.last_frames[0], 0) << "restored voice should keep producing real audio";
}

TEST(Mixer, DeserializeRestoresNextIdSoNewVoicesDoNotCollide) {
  Mixer mixer(22050);
  Mixer::VoiceId id1 = mixer.Play(MakeMono({1, 2}), 1, 22050, false);

  std::stringstream stream;
  ASSERT_TRUE(mixer.Serialize(stream));

  Mixer mixer2(22050);
  ASSERT_TRUE(mixer2.Deserialize(stream));
  Mixer::VoiceId id2 = mixer2.Play(MakeMono({3, 4}), 1, 22050, false);
  EXPECT_NE(id1, id2);
}

TEST(Mixer, DeserializeOnATruncatedStreamFailsWithoutCrashing) {
  Mixer mixer(22050);
  std::stringstream stream;
  stream.write("\x01\x00\x00\x00", 4);  // claims next_id_=1, then nothing else
  EXPECT_FALSE(mixer.Deserialize(stream));
}
