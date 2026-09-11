#include "core/brew/media_hle.h"

#include "core/control/debug_sink.h"
#include "core/loader/mp3.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

namespace zeebulator {

namespace {

// Env-gated audio/media trace (ZEEB_LOG_MEDIA=1). Off by default, so it
// can stay committed like the rest of this codebase's instrumentation
// family (ZEEB_LOG_SLOT / ZEEB_LOG_CREATEINSTANCE / ZEEB_LOG_FILE).
void MediaLog(const char* fmt, ...) {
  static const bool on = std::getenv("ZEEB_LOG_MEDIA") != nullptr;
  char buf[512];
  std::va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ::zeebulator::DebugLog(::zeebulator::DebugCat::kMedia, buf);
  if (!on) return;
  std::fprintf(stderr, "[media] %s\n", buf);
}

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
  // MP3: an ID3v2 tag ("ID3") or a raw MPEG audio sync word (0xFF followed by
  // 0xE_/0xF_). minimp3 tolerates leading junk, so a soft sniff is enough --
  // ParseMp3 still returns nullopt if no real frame decodes.
  if (data.size() >= 3 && ((data[0] == 'I' && data[1] == 'D' && data[2] == '3') ||
                           (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0))) {
    return ParseMp3(data.data(), data.size());
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
  if (HasExtension(name, ".mp3")) {
    return ParseMp3(data.data(), data.size());
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
  // Real BREW IMedia objects expose system fields the game reads
  // directly from guest memory — found decoding ddragonz.mod's notify
  // chain (see research/sources/zeebulator-notify-chain-decoded.md):
  //   +0x00 vtable
  //   +0x08 media source (dispatcher 0x11f4dc NULL-checks it; Play
  //         helper 0x11d04c falls back to it when +0x28 is zero)
  //   +0x1c u32 priority stamp (dispatcher resets to -1)
  //   +0x24 byte (dispatcher clears)
  //   +0x25 byte — the "ready" flag Play() success sets (dispatcher
  //         requires it nonzero for the vtable[11] priority-reset path)
  //   +0x28 media source cache (Play helper uses it if nonzero)
  // The old vtable-only 4-byte object made every one of those reads
  // land on adjacent garbage → BX NULL wander. Allocate a proper
  // 0x40-byte guest object: vtable at +0, +8 self-references so the
  // Play fallback's vtable[6] resolves to our Play trap, rest zeroed.
  // Reusar endereco de objeto ja solto antes de avancar o ponteiro. A regiao
  // de objetos e finita, e um jogo que cria e solta IMedia em laco a esgota:
  // sem reuso o ponteiro so anda para a frente ate bater no fim, e a partir
  // dai TODA criacao falha. Com AddRef/Release de verdade (slots 0 e 1, que
  // eram Stub e nunca liberavam nada) o endereco volta para esta lista.
  uint32_t obj_addr;
  if (!free_object_addresses_.empty()) {
    obj_addr = free_object_addresses_.back();
    free_object_addresses_.pop_back();
  } else {
    obj_addr = next_object_address_;
    next_object_address_ += 0x40;
  }
  memory_.Write32(obj_addr, vtable_address_);
  memory_.Write32(obj_addr + 8, obj_addr);  // media source = self
  for (uint32_t off = 0x0c; off < 0x40; off += 4) {
    memory_.Write32(obj_addr + off, 0);
  }
  media_by_object_[obj_addr] = Media{};
  return obj_addr;
}

uint32_t MediaHle::CreateMediaObject() { return AllocateMediaObject(); }

void MediaHle::AddRefImpl(IArmCore& core) {
  // Contagem de referencias honesta: o objeto nasce com 1 em
  // AllocateMediaObject, e cada AddRef soma. Devolve a contagem nova, que e
  // o contrato real de IBase_AddRef.
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }
  ++it->second.ref_count;
  MediaLog("obj=0x%08x AddRef -> %u", core.GetRegister(kR0), it->second.ref_count);
  core.SetRegister(kR0, it->second.ref_count);
}

void MediaHle::ReleaseImpl(IArmCore& core) {
  // Antes isto era um Stub que nao fazia nada, entao nenhum objeto de midia
  // era liberado em toda a execucao. Alem do vazamento, isso e o que faz um
  // jogo que reabre midia em laco esgotar a regiao de objetos e passar a
  // falhar em tudo.
  const uint32_t obj = core.GetRegister(kR0);
  auto it = media_by_object_.find(obj);
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }
  if (it->second.ref_count > 0) --it->second.ref_count;
  const uint32_t remaining = it->second.ref_count;
  MediaLog("obj=0x%08x Release -> %u", obj, remaining);
  if (remaining == 0) {
    // Uma voz ainda tocando precisa parar junto: o dono dela acabou de
    // desaparecer, e deixar o mixer somando um clipe orfao e exatamente o
    // tipo de som fantasma que nao se rastreia depois.
    if (it->second.has_voice) {
      mixer_.Stop(it->second.voice);
      it->second.has_voice = false;
    }
    media_by_object_.erase(it);
    memory_.Write32(obj, 0);
    free_object_addresses_.push_back(obj);
  }
  core.SetRegister(kR0, remaining);
}


void MediaHle::RegisterNotifyImpl(IArmCore& core) {
  // int RegisterNotify(IMedia *po, PFNMEDIANOTIFY pfnNotify, void *pUser)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  it->second.notify_fn = core.GetRegister(kR1);
  it->second.notify_user = core.GetRegister(kR2);
  MediaLog("obj=0x%08x RegisterNotify fn=0x%08x user=0x%08x", core.GetRegister(kR0),
           core.GetRegister(kR1), core.GetRegister(kR2));
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
        // Antes isto falhava calado. Um arquivo de audio que o jogo pede e o
        // VFS nao resolve e indistinguivel, no log, de um codec faltando --
        // e as duas causas exigem correcoes completamente diferentes.
        MediaLog("obj=0x%08x SetData FILE '%s' -> RECUSADO: nao esta no VFS",
                 core.GetRegister(kR0), name.c_str());
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
      MediaLog("obj=0x%08x SetData -> RECUSADO: MMD_ISOURCE (cls_data=%u) nao implementado",
               core.GetRegister(kR0), cls_data);
      core.SetRegister(kR0, 1);  // MMD_ISOURCE not supported yet
      return;
    }
    if (!decoded) {
      // Um codec que nao reconhecemos so era visivel como silencio. Registrar
      // os primeiros bytes torna o formato identificavel direto do log, sem
      // precisar extrair o asset e adivinhar: RIFF/MThd/ID3/0xFFEx ja sao
      // tratados, entao o que aparecer aqui e exatamente a lista do que falta
      // implementar (QCP, AMR, ADPCM e afins).
      // 32 bytes cobrem o cabecalho inteiro de um RIFF/WAVE ("RIFF" + tamanho
      // + "WAVEfmt " + tamanho do bloco + o format tag em +20), que e o que
      // distingue PCM e IMA-ADPCM (ja suportados) de MS-ADPCM, A-law, mu-law
      // ou MP3-dentro-de-WAV. Com 8 bytes so daria para ver "RIFF" e concluir
      // nada.
      uint8_t head[32] = {};
      size_t head_n = 0;
      if (cls_data == kMmdBuffer) {
        head_n = std::min<size_t>(32, data_size);
        for (size_t i = 0; i < head_n; ++i) head[i] = memory_.Read8(data_ptr + static_cast<uint32_t>(i));
      }
      char hex[3 * 32 + 1] = {};
      for (size_t i = 0; i < head_n; ++i) std::snprintf(hex + i * 3, 4, "%02x ", head[i]);
      char ascii[33] = {};
      for (size_t i = 0; i < head_n; ++i)
        ascii[i] = (head[i] >= 0x20 && head[i] < 0x7f) ? static_cast<char>(head[i]) : '.';
      MediaLog("obj=0x%08x SetData %u bytes -> RECUSADO: formato nao reconhecido [%s| %s]",
               core.GetRegister(kR0), data_size, hex, ascii);
      core.SetRegister(kR0, 1);  // corrupt, or a codec we don't support yet (e.g. IMA-ADPCM/MP3)
      return;
    }
    media.channels = decoded->channels;
    media.sample_rate = decoded->sample_rate;
    media.samples = std::make_shared<const std::vector<int16_t>>(std::move(decoded->samples));
    media.has_data = true;
    media.state = kStateReady;
    if (cls_data == kMmdFileName) {
      MediaLog("obj=0x%08x SetData FILE '%s' -> %u ch, %u Hz, %zu samples",
               core.GetRegister(kR0), ReadCString(memory_, data_ptr).c_str(),
               media.channels, media.sample_rate, media.samples->size());
    } else {
      MediaLog("obj=0x%08x SetData BUFFER %u bytes -> %u ch, %u Hz, %zu samples",
               core.GetRegister(kR0), data_size, media.channels, media.sample_rate,
               media.samples->size());
    }
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmPlayRepeat) {
    media.loop = (p1 == 0);  // 0 = forever; exact counts > 1 aren't tracked yet
    MediaLog("obj=0x%08x SetRepeat p1=%u -> loop=%d", core.GetRegister(kR0), p1, media.loop);
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
    MediaLog("obj=0x%08x SetVolume %d", core.GetRegister(kR0), media.volume);
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
  MediaLog("obj=0x%08x PLAY voice=%d %u ch %u Hz loop=%d vol=%d (%zu samples)",
           it->first, media.voice, media.channels, media.sample_rate, media.loop,
           media.volume, media.samples ? media.samples->size() : 0);
  // Real BREW sets the "ready" flag at obj+0x25 on successful Play();
  // the game's notify dispatcher (ddragonz.mod 0x11f4dc) requires it
  // nonzero to take the priority-reset path — without it the channel
  // priority stamp never comes down and the channel stays poisoned.
  memory_.Write8(it->first + 0x25, 1);
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

  // A real notify callback can call IMedia::Release(), which erases its
  // Media from media_by_object_. Never hold an unordered_map iterator across
  // guest re-entry: first snapshot finished notifications, then invoke them.
  struct FinishedNotify {
    uint32_t object_addr;
    int voice;
    uint32_t fn;
    uint32_t user;
  };
  std::vector<FinishedNotify> finished;
  for (auto& [object_addr, media] : media_by_object_) {
    if (!media.has_voice || media.notify_fn == 0) continue;
    if (mixer_.IsPlaying(media.voice)) continue;
    media.has_voice = false;
    media.state = kStateReady;
    finished.push_back({object_addr, media.voice, media.notify_fn, media.notify_user});
  }
  for (const FinishedNotify& done : finished) {
    MediaLog("obj=0x%08x voice=%d FINISHED -> notify fn=0x%08x user=0x%08x",
             done.object_addr, done.voice, done.fn, done.user);
    hle_.CallArmFunction(done.fn, done.user, notify_scratch_address_);
  }
}

void MediaHle::StopImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  const bool was_playing = media.has_voice;
  if (media.has_voice) {
    mixer_.Stop(media.voice);
    media.has_voice = false;
  }
  media.state = media.has_data ? kStateReady : kStateIdle;
  MediaLog("obj=0x%08x STOP", core.GetRegister(kR0));
  // IMEDIA_Stop no BREW real avisa o callback registrado que a reproducao
  // terminou; so parar a voz e devolver 0 nao fecha o ciclo. Um gerenciador
  // de audio que troca de faixa espera essa notificacao para saber que a
  // anterior acabou e que pode seguir -- sem ela a maquina de estados dele
  // fica parada e a proxima faixa nunca comeca.
  //
  // Medido no cnk2: o jogo toca a musica de menu (obj 0x80200000, loop),
  // chama Stop nela, carrega a faixa seguinte de 642010 bytes em outro
  // objeto, registra notify... e nunca chama Play. Que e exatamente o
  // "entro em partida e nao toca nada" relatado ao vivo.
  //
  // Mesma convencao ja usada em PlayImpl quando ele substitui uma voz que
  // ainda tocava: MM_CMD_PLAY + MM_STATUS_ABORT no scratch de notificacao.
  if (was_playing && media.notify_fn != 0) {
    constexpr uint32_t kMmCmdPlay = 4;
    constexpr uint32_t kMmStatusAbort = 3;
    memory_.Write32(notify_scratch_address_ + 8, kMmCmdPlay);
    memory_.Write32(notify_scratch_address_ + 16, kMmStatusAbort);
    hle_.CallArmFunction(media.notify_fn, media.notify_user, notify_scratch_address_);
  }
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
      [this](IArmCore& c) { AddRefImpl(c); },             // 0  AddRef
      [this](IArmCore& c) { ReleaseImpl(c); },            // 1  Release
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
