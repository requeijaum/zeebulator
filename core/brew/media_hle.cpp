#include "core/brew/media_hle.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <utility>

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }
void StubFailed(IArmCore& core) { core.SetRegister(kR0, 1); }  // AEE_EFAILED-ish

// Real MM_PARM_* values from AEEIMedia.h (numeric constants, not
// copyrighted expression -- same rationale as the GLES enum values in
// gl_types.h).
constexpr int kParmVolume = 4;
constexpr int kParmPlayRepeat = 11;
constexpr int kParmMediaData = 1;
constexpr int kParmChannelShare = 16;

constexpr uint32_t kMmdFileName = 0;
// Real value confirmed live (TASKS.md/PHASE8_LOG.md Phase 8, the sound
// investigation): Double Dragon's own custom sound.ggz loader reads raw
// bytes directly into a malloc'd buffer (never through a named file the
// VFS could resolve) and hands SetMediaParm a real AEEMediaData with
// clsData=1, matching real BREW's MMD_BUFFER.
constexpr uint32_t kMmdBuffer = 1;

// Real evidence (same investigation): a live-captured MMD_BUFFER's first
// bytes are `1f 8b 08 08 ...` -- a genuine gzip stream (magic + CM=
// deflate + FLG=FNAME set), with the original filename (e.g.
// "bgm_1_...") visible right in the header -- Double Dragon's own
// sound.ggz entries are stored gzip-compressed, same real container
// convention this project's own loader (core/loader/ggz.cpp) and the
// runtime-helper table's offset-0xdc slot (ModRuntime::
// DecompressGzipInPlaceImpl) already handle elsewhere. Real gzip
// streams don't declare their own decompressed length up front, so
// this grows the output buffer as needed rather than assuming a fixed
// size, same approach as that other real gzip consumer.
std::optional<std::vector<uint8_t>> Gunzip(const std::vector<uint8_t>& compressed) {
  z_stream strm{};
  if (inflateInit2(&strm, 15 + 16) != Z_OK) return std::nullopt;
  std::vector<uint8_t> out(std::max<size_t>(compressed.size() * 4, 4096));
  strm.next_in = const_cast<Bytef*>(compressed.data());
  strm.avail_in = static_cast<uInt>(compressed.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  int ret;
  do {
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&strm);
      return std::nullopt;
    }
    if (strm.avail_out == 0 && ret != Z_STREAM_END) {
      size_t old_size = out.size();
      out.resize(old_size * 2);
      strm.next_out = out.data() + old_size;
      strm.avail_out = static_cast<uInt>(out.size() - old_size);
    }
  } while (ret != Z_STREAM_END);
  size_t produced = out.size() - strm.avail_out;
  inflateEnd(&strm);
  out.resize(produced);
  return out;
}

// Decodes an in-memory buffer that has no filename/extension to dispatch
// on (unlike DecodeAudioFile) -- sniffs the real container magic bytes
// instead: `RIFF` for WAV, `MThd` for Standard MIDI. Assumes the caller
// has already gunzipped it if needed.
// Renders parsed MIDI through the real soundfont synth when one was
// given and actually loaded, falling back to the hand-rolled
// approximation otherwise (see MediaHle's own constructor doc comment
// and RenderMidiToPcm's doc comment).
WavAudio RenderMidi(const MidiFile& midi, int mix_sample_rate, SoundFontSynth* soundfont_synth) {
  if (soundfont_synth != nullptr && soundfont_synth->IsLoaded()) {
    WavAudio out;
    out.sample_rate = mix_sample_rate;
    out.channels = 1;
    out.samples = soundfont_synth->RenderMidi(midi, mix_sample_rate);
    return out;
  }
  return RenderMidiToPcm(midi, mix_sample_rate);
}

std::optional<WavAudio> DecodeAudioBuffer(const std::vector<uint8_t>& data, int mix_sample_rate,
                                           SoundFontSynth* soundfont_synth) {
  if (data.size() >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
    return ParseWav(data.data(), data.size());
  }
  if (data.size() >= 4 && data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'd') {
    auto midi = ParseMidi(data.data(), data.size());
    if (!midi) return std::nullopt;
    return RenderMidi(*midi, mix_sample_rate, soundfont_synth);
  }
  return std::nullopt;
}

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

bool HasExtension(const std::string& name, const char* ext) {
  size_t ext_len = std::strlen(ext);
  if (name.size() < ext_len) return false;
  return std::equal(name.end() - static_cast<long>(ext_len), name.end(), ext,
                     [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

// Codec is chosen by file extension -- see MediaHle's class doc.
std::optional<WavAudio> DecodeAudioFile(const std::string& name, const std::vector<uint8_t>& data,
                                         int mix_sample_rate, SoundFontSynth* soundfont_synth) {
  if (HasExtension(name, ".wav")) {
    return ParseWav(data.data(), data.size());
  }
  if (HasExtension(name, ".mid") || HasExtension(name, ".midi")) {
    auto midi = ParseMidi(data.data(), data.size());
    if (!midi) return std::nullopt;
    return RenderMidi(*midi, mix_sample_rate, soundfont_synth);
  }
  return std::nullopt;
}

template <typename T>
bool WritePod(std::ostream& out, const T& v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return out.good();
}

template <typename T>
bool ReadPod(std::istream& in, T& v) {
  in.read(reinterpret_cast<char*>(&v), sizeof(v));
  return in.good();
}

}  // namespace

MediaHle::MediaHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs, Mixer& mixer,
                    uint32_t object_region_start, SoundFontSynth* soundfont_synth)
    : memory_(memory), hle_(hle), vfs_(vfs), mixer_(mixer), soundfont_synth_(soundfont_synth),
      next_object_address_(object_region_start),
      notify_scratch_address_(object_region_start + kNotifyScratchOffset) {}

uint32_t MediaHle::AllocateMediaObject() {
  uint32_t obj_addr = next_object_address_;
  next_object_address_ += 4;
  memory_.Write32(obj_addr, vtable_address_);
  media_by_object_[obj_addr] = Media{};
  return obj_addr;
}

uint32_t MediaHle::CreateMediaObject() { return AllocateMediaObject(); }

void MediaHle::RegisterNotifyImpl(IArmCore& core) {
  // int RegisterNotify(IMedia *po, PFNMEDIANOTIFY pfnNotify, void *pUser)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  it->second.notify_fn = core.GetRegister(kR1);
  it->second.notify_user = core.GetRegister(kR2);
  core.SetRegister(kR0, 0);
}

void MediaHle::SetMediaParmImpl(IArmCore& core) {
  // int SetMediaParm(IMedia *po, int nParamID, int32 p1, int32 p2)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  auto param_id = static_cast<int32_t>(core.GetRegister(kR1));
  uint32_t p1 = core.GetRegister(kR2);

  if (param_id == kParmMediaData) {
    // p1 -> AEEMediaData { AEECLSID clsData; void *pData; uint32 dwSize; }
    uint32_t cls_data = memory_.Read32(p1 + 0);
    uint32_t data_ptr = memory_.Read32(p1 + 4);
    uint32_t data_size = memory_.Read32(p1 + 8);

    std::optional<WavAudio> decoded;
    if (cls_data == kMmdFileName) {
      std::string name = ReadCString(memory_, data_ptr);
      const std::vector<uint8_t>* file_data = vfs_.Find(name);
      if (!file_data) {
        core.SetRegister(kR0, 1);
        return;
      }
      decoded = DecodeAudioFile(name, *file_data, mixer_.OutputSampleRate(), soundfont_synth_);
    } else if (cls_data == kMmdBuffer) {
      std::vector<uint8_t> raw(data_size);
      for (uint32_t i = 0; i < data_size; ++i) raw[i] = memory_.Read8(data_ptr + i);
      // Real sound.ggz entries are gzip-compressed (see kMmdBuffer's own
      // doc comment) -- gunzip first, matching the real magic bytes,
      // then dispatch on the decompressed content's own container magic.
      constexpr uint8_t kGzipMagic[2] = {0x1f, 0x8b};
      if (raw.size() >= 2 && raw[0] == kGzipMagic[0] && raw[1] == kGzipMagic[1]) {
        if (auto decompressed = Gunzip(raw)) {
          decoded = DecodeAudioBuffer(*decompressed, mixer_.OutputSampleRate(), soundfont_synth_);
        }
      } else {
        decoded = DecodeAudioBuffer(raw, mixer_.OutputSampleRate(), soundfont_synth_);
      }
    } else {
      core.SetRegister(kR0, 1);  // MMD_ISOURCE not supported yet
      return;
    }
    if (!decoded) {
      core.SetRegister(kR0, 1);  // corrupt, or a codec we don't support yet (e.g. IMA-ADPCM/MP3)
      return;
    }
    media.channels = decoded->channels;
    media.sample_rate = decoded->sample_rate;
    media.samples = std::make_shared<const std::vector<int16_t>>(std::move(decoded->samples));
    media.has_data = true;
    media.state = kStateReady;
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmPlayRepeat) {
    media.loop = (p1 == 0);  // 0 = forever; exact counts > 1 aren't tracked yet
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmVolume) {
    // Real MM_PARM_VOLUME, 0-100 (AEE_MAX_VOLUME) -- actually applied
    // now (this used to be accepted-but-ignored, a real, documented
    // gap: with real gameplay music actually audible for the first
    // time, PHASE8_LOG.md's "Sound, round twelve", a real, dense
    // multi-channel soundfont-rendered track drowning out real, much
    // quieter SFX voices in the shared Mixer turned out to be exactly
    // the kind of real per-channel volume balancing this parameter
    // exists for). Also updates an already-playing voice immediately,
    // not just future Play() calls -- real code can legitimately call
    // this after Play() has already started.
    media.volume = std::clamp(static_cast<int32_t>(p1), 0, 100);
    if (media.has_voice) mixer_.SetVolume(media.voice, media.volume);
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmChannelShare) {
    // Accepted, not yet applied to playback -- see class doc. Returning
    // success (rather than an error) avoids spuriously failing real app
    // logic that doesn't strictly depend on this actually taking effect.
    core.SetRegister(kR0, 0);
    return;
  }

  // Any other parm: no-op success, same reasoning as above.
  core.SetRegister(kR0, 0);
}

void MediaHle::GetMediaParmImpl(IArmCore& core) {
  // int GetMediaParm(IMedia *po, int nParamID, int32 *pP1, int32 *pP2)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  auto param_id = static_cast<int32_t>(core.GetRegister(kR1));
  uint32_t p_p1 = core.GetRegister(kR2);
  if (param_id == kParmVolume) {
    int volume = (it != media_by_object_.end()) ? it->second.volume : 100;
    if (p_p1 != 0) memory_.Write32(p_p1, static_cast<uint32_t>(volume));
    core.SetRegister(kR0, 0);
    return;
  }
  core.SetRegister(kR0, 1);  // not implemented for anything else yet
}

void MediaHle::PlayImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_data) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  if (media.has_voice) {
    // Real per-character sound channels are a small shared pool -- a new,
    // higher-priority sound reclaims an already-playing channel by
    // calling Play() again on the same IMedia object, not by Stop()ping
    // it first (see this class's own doc comment). The old voice never
    // gets to finish naturally, so Tick()'s own "voice finished"
    // detection would never notice it once media.voice below gets
    // overwritten -- silently dropping exactly the notification the real
    // per-character channel's own priority-stamp reset depends on, which
    // permanently locks that logical invocation's priority claim. This
    // is the same real bug class Tick()'s own DONE-on-finish fix already
    // covers, just for the "interrupted, not finished" case instead of
    // the "finished naturally" one -- confirmed live-reproduced the same
    // way (TASKS.md/PHASE8_LOG.md), recurring later into a real playthrough
    // once channel reclaims start happening (busier stages, more
    // simultaneous real attackers). Real MM_STATUS_ABORT (3) is the
    // documented status for exactly this case; the real registered
    // callback routes it to the same handler as MM_STATUS_DONE (2)
    // either way (see class doc comment), so this is a real, not just
    // convenient, status value.
    mixer_.Stop(media.voice);
    if (media.notify_fn != 0) {
      constexpr uint32_t kMmCmdPlay = 4;
      constexpr uint32_t kMmStatusAbort = 3;
      memory_.Write32(notify_scratch_address_ + 8, kMmCmdPlay);
      memory_.Write32(notify_scratch_address_ + 16, kMmStatusAbort);
      hle_.CallArmFunction(media.notify_fn, media.notify_user, notify_scratch_address_);
    }
  }
  media.voice =
      mixer_.Play(media.samples, media.channels, media.sample_rate, media.loop, media.volume);
  media.has_voice = true;
  media.state = kStatePlay;
  core.SetRegister(kR0, 0);
}

void MediaHle::Tick() {
  // Real AEEMediaCmdNotify field values this project's own live trace of
  // the real registered callback (`ddragonz.mod` 0x11d020) confirmed it
  // actually reads -- see the class doc comment.
  constexpr uint32_t kMmCmdPlay = 4;
  constexpr uint32_t kMmStatusDone = 2;
  memory_.Write32(notify_scratch_address_ + 8, kMmCmdPlay);
  memory_.Write32(notify_scratch_address_ + 16, kMmStatusDone);

  for (auto& [object_addr, media] : media_by_object_) {
    if (!media.has_voice || media.notify_fn == 0) continue;
    if (mixer_.IsPlaying(media.voice)) continue;
    media.has_voice = false;
    media.state = kStateReady;
    hle_.CallArmFunction(media.notify_fn, media.notify_user, notify_scratch_address_);
  }
}

void MediaHle::StopImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  if (media.has_voice) {
    mixer_.Stop(media.voice);
    media.has_voice = false;
  }
  media.state = media.has_data ? kStateReady : kStateIdle;
  core.SetRegister(kR0, 0);
}

void MediaHle::PauseImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_voice) {
    core.SetRegister(kR0, 1);
    return;
  }
  mixer_.Pause(it->second.voice);
  it->second.state = kStatePlayPause;
  core.SetRegister(kR0, 0);
}

void MediaHle::ResumeImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_voice) {
    core.SetRegister(kR0, 1);
    return;
  }
  mixer_.Resume(it->second.voice);
  it->second.state = kStatePlay;
  core.SetRegister(kR0, 0);
}

void MediaHle::GetTotalTimeImpl(IArmCore& core) {
  // int GetTotalTime(IMedia *po) -- ms, returned directly (no out-param).
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_data || it->second.channels == 0 ||
      it->second.sample_rate == 0) {
    core.SetRegister(kR0, 0);
    return;
  }
  const Media& media = it->second;
  uint64_t frames = media.samples->size() / static_cast<uint32_t>(media.channels);
  uint32_t ms = static_cast<uint32_t>(frames * 1000 / static_cast<uint32_t>(media.sample_rate));
  core.SetRegister(kR0, ms);
}

void MediaHle::GetStateImpl(IArmCore& core) {
  // int GetState(IMedia *po, boolean *pbStateChanging)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, kStateIdle);
    return;
  }
  Media& media = it->second;
  if (media.has_voice && !mixer_.IsPlaying(media.voice)) {
    // The voice finished naturally (non-looping playback completed)
    // since we last checked.
    media.has_voice = false;
    media.state = kStateReady;
  }
  uint32_t pb_state_changing = core.GetRegister(kR1);
  if (pb_state_changing != 0) memory_.Write32(pb_state_changing, 0);  // never async in this HLE
  core.SetRegister(kR0, static_cast<uint32_t>(media.state));
}

void MediaHle::Build(uint32_t vtable_address) {
  vtable_address_ = vtable_address;

  // Slot order verified against real AEEIMedia.h -- see class doc.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                              // 0  AddRef
      Stub,                                              // 1  Release
      Stub,                                              // 2  QueryInterface
      [this](IArmCore& c) { RegisterNotifyImpl(c); },     // 3  RegisterNotify
      [this](IArmCore& c) { SetMediaParmImpl(c); },       // 4  SetMediaParm
      [this](IArmCore& c) { GetMediaParmImpl(c); },       // 5  GetMediaParm
      [this](IArmCore& c) { PlayImpl(c); },               // 6  Play
      StubFailed,                                        // 7  Record (not implemented)
      [this](IArmCore& c) { StopImpl(c); },               // 8  Stop
      StubFailed,                                        // 9  Seek (not implemented)
      [this](IArmCore& c) { PauseImpl(c); },              // 10 Pause
      [this](IArmCore& c) { ResumeImpl(c); },             // 11 Resume
      [this](IArmCore& c) { GetTotalTimeImpl(c); },       // 12 GetTotalTime
      [this](IArmCore& c) { GetStateImpl(c); },           // 13 GetState
  };
  // Vtable-only: no single "object" here, since a real IMedia object is
  // created per CreateMediaObject() call (this mirrors FileHle's
  // shared-vtable-plus-many-instances pattern).
  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle_.Register(methods[i]);
    memory_.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
}

bool MediaHle::Serialize(std::ostream& out) const {
  if (!WritePod(out, next_object_address_)) return false;
  if (!WritePod(out, static_cast<uint32_t>(media_by_object_.size()))) return false;
  for (const auto& [object_addr, media] : media_by_object_) {
    if (!WritePod(out, object_addr)) return false;
    if (!WritePod(out, media.has_data)) return false;
    if (!WritePod(out, media.channels)) return false;
    if (!WritePod(out, media.sample_rate)) return false;
    const std::vector<int16_t>& samples = media.samples ? *media.samples : std::vector<int16_t>{};
    if (!WritePod(out, static_cast<uint32_t>(samples.size()))) return false;
    if (!samples.empty()) {
      out.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
      if (!out.good()) return false;
    }
    if (!WritePod(out, media.voice)) return false;
    if (!WritePod(out, media.has_voice)) return false;
    if (!WritePod(out, media.loop)) return false;
    if (!WritePod(out, media.volume)) return false;
    if (!WritePod(out, media.state)) return false;
    if (!WritePod(out, media.notify_fn)) return false;
    if (!WritePod(out, media.notify_user)) return false;
  }
  return true;
}

bool MediaHle::Deserialize(std::istream& in) {
  uint32_t next_object_address = 0;
  if (!ReadPod(in, next_object_address)) return false;
  uint32_t count = 0;
  if (!ReadPod(in, count)) return false;

  std::unordered_map<uint32_t, Media> loaded;
  loaded.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t object_addr = 0;
    if (!ReadPod(in, object_addr)) return false;
    Media media;
    if (!ReadPod(in, media.has_data)) return false;
    if (!ReadPod(in, media.channels)) return false;
    if (!ReadPod(in, media.sample_rate)) return false;
    uint32_t sample_count = 0;
    if (!ReadPod(in, sample_count)) return false;
    auto samples = std::make_shared<std::vector<int16_t>>(sample_count);
    if (sample_count != 0) {
      in.read(reinterpret_cast<char*>(samples->data()),
              static_cast<std::streamsize>(sample_count * sizeof(int16_t)));
      if (!in.good()) return false;
    }
    media.samples = std::move(samples);
    if (!ReadPod(in, media.voice)) return false;
    if (!ReadPod(in, media.has_voice)) return false;
    if (!ReadPod(in, media.loop)) return false;
    if (!ReadPod(in, media.volume)) return false;
    if (!ReadPod(in, media.state)) return false;
    if (!ReadPod(in, media.notify_fn)) return false;
    if (!ReadPod(in, media.notify_user)) return false;
    loaded.emplace(object_addr, std::move(media));
  }

  next_object_address_ = next_object_address;
  media_by_object_ = std::move(loaded);
  return true;
}

}  // namespace zeebulator
