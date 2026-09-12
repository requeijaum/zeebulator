#include "core/audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace zeebulator {

namespace {

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

Mixer::Mixer(int output_sample_rate) : output_sample_rate_(output_sample_rate) {}

Mixer::VoiceId Mixer::Play(std::shared_ptr<const std::vector<int16_t>> samples, int channels,
                            int sample_rate, bool loop, int volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  Voice voice;
  voice.id = next_id_++;
  voice.samples = std::move(samples);
  voice.channels = channels;
  voice.sample_rate = sample_rate;
  voice.loop = loop;
  voice.volume = std::clamp(volume, 0, 100);
  voices_.push_back(std::move(voice));
  return voices_.back().id;
}

void Mixer::SetVolume(VoiceId id, int volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.volume = std::clamp(volume, 0, 100);
  }
}

void Mixer::Stop(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                [id](const Voice& v) { return v.id == id; }),
                voices_.end());
}

void Mixer::Pause(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.paused = true;
  }
}

void Mixer::Resume(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.paused = false;
  }
}

bool Mixer::IsPlaying(VoiceId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Voice& v : voices_) {
    if (v.id == id) return !v.finished;
  }
  return false;
}

void Mixer::Mix(Backend& backend, size_t frame_count) {
  std::vector<int32_t> accum(frame_count * 2, 0);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& voice : voices_) {
      if (voice.paused || voice.finished || !voice.samples) continue;
      size_t total_frames = voice.samples->size() / static_cast<size_t>(voice.channels);
      if (total_frames == 0) continue;

      // Linear resampling: real decoded clips don't all share the
      // Mixer's fixed output rate (see this class's own doc comment),
      // so each output frame advances the source position by the
      // voice's own rate ratio rather than 1:1.
      const double step = static_cast<double>(voice.sample_rate) / output_sample_rate_;
      double pos = voice.position_frames;
      for (size_t i = 0; i < frame_count; ++i, pos += step) {
        double sample_pos = pos;
        if (voice.loop) {
          sample_pos = std::fmod(sample_pos, static_cast<double>(total_frames));
        } else if (sample_pos >= static_cast<double>(total_frames)) {
          break;  // this voice has nothing left to contribute this call
        }

        size_t frame0 = static_cast<size_t>(sample_pos);
        size_t frame1 = voice.loop ? (frame0 + 1) % total_frames
                                    : std::min(frame0 + 1, total_frames - 1);
        double frac = sample_pos - static_cast<double>(frame0);

        double gain = voice.volume / 100.0;
        double left, right;
        if (voice.channels == 2) {
          left = ((*voice.samples)[frame0 * 2] * (1.0 - frac) + (*voice.samples)[frame1 * 2] * frac) *
                 gain;
          right = ((*voice.samples)[frame0 * 2 + 1] * (1.0 - frac) +
                    (*voice.samples)[frame1 * 2 + 1] * frac) *
                  gain;
        } else {
          left = right =
              ((*voice.samples)[frame0] * (1.0 - frac) + (*voice.samples)[frame1] * frac) * gain;
        }
        accum[i * 2] += static_cast<int32_t>(left);
        accum[i * 2 + 1] += static_cast<int32_t>(right);
      }

      if (voice.loop) {
        voice.position_frames = std::fmod(pos, static_cast<double>(total_frames));
      } else {
        voice.position_frames = pos;
        if (voice.position_frames >= static_cast<double>(total_frames)) voice.finished = true;
      }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                  [](const Voice& v) { return v.finished; }),
                  voices_.end());
  }

  std::vector<int16_t> out(frame_count * 2);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<int16_t>(std::clamp<int32_t>(accum[i], -32768, 32767));
  }
  DumpAudioIfRequested(out);
  backend.PushAudioSamples(out.data(), frame_count, output_sample_rate_);
}

// Despejo do audio mixado em WAV (ZEEB_DUMP_AUDIO=caminho.wav).
//
// Sem isto nao da para dizer se o som "funciona": os logs de IMedia mostram
// SetData/SetVolume/RegisterNotify, mas nada disso prova que uma amostra
// chegou a saida. O zeebx tem --dump-audio e este projeto nao tinha
// equivalente, entao nao havia como comparar os dois lado a lado.
//
// O cabecalho e escrito com tamanhos zerados na criacao e reescrito a cada
// bloco, para que o arquivo continue valido mesmo se a execucao for
// interrompida (que e o caso normal aqui -- os jogos rodam sob timeout).
void Mixer::DumpAudioIfRequested(const std::vector<int16_t>& out) {
  static const char* path = std::getenv("ZEEB_DUMP_AUDIO");
  if (path == nullptr) return;
  if (dump_file_ == nullptr) {
    dump_file_ = std::fopen(path, "wb");
    if (dump_file_ == nullptr) return;
    unsigned char header[44] = {};
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;                 // tamanho do bloco fmt
    header[20] = 1;                  // PCM
    header[22] = 2;                  // canais (estereo)
    const uint32_t rate = static_cast<uint32_t>(output_sample_rate_);
    const uint32_t byte_rate = rate * 2 * 2;
    std::memcpy(header + 24, &rate, 4);
    std::memcpy(header + 28, &byte_rate, 4);
    header[32] = 4;                  // alinhamento de bloco
    header[34] = 16;                 // bits por amostra
    std::memcpy(header + 36, "data", 4);
    std::fwrite(header, 1, sizeof(header), dump_file_);
  }
  std::fwrite(out.data(), sizeof(int16_t), out.size(), dump_file_);
  dump_bytes_ += out.size() * sizeof(int16_t);

  const uint32_t data_size = static_cast<uint32_t>(dump_bytes_);
  const uint32_t riff_size = data_size + 36;
  std::fseek(dump_file_, 4, SEEK_SET);
  std::fwrite(&riff_size, 4, 1, dump_file_);
  std::fseek(dump_file_, 40, SEEK_SET);
  std::fwrite(&data_size, 4, 1, dump_file_);
  std::fseek(dump_file_, 0, SEEK_END);
  std::fflush(dump_file_);
}

bool Mixer::Serialize(std::ostream& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!WritePod(out, next_id_)) return false;
  if (!WritePod(out, static_cast<uint32_t>(voices_.size()))) return false;
  for (const Voice& v : voices_) {
    if (!WritePod(out, v.id)) return false;
    if (!WritePod(out, v.channels)) return false;
    if (!WritePod(out, v.sample_rate)) return false;
    if (!WritePod(out, v.loop)) return false;
    if (!WritePod(out, v.volume)) return false;
    if (!WritePod(out, v.paused)) return false;
    if (!WritePod(out, v.position_frames)) return false;
    if (!WritePod(out, v.finished)) return false;
    const std::vector<int16_t>& samples = v.samples ? *v.samples : std::vector<int16_t>{};
    if (!WritePod(out, static_cast<uint32_t>(samples.size()))) return false;
    if (!samples.empty()) {
      out.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
      if (!out.good()) return false;
    }
  }
  return true;
}

bool Mixer::Deserialize(std::istream& in) {
  VoiceId next_id = 0;
  if (!ReadPod(in, next_id)) return false;
  uint32_t voice_count = 0;
  if (!ReadPod(in, voice_count) || voice_count > 1024u) return false;

  constexpr uint32_t kMaxSamplesPerVoice = 64u * 1024u * 1024u;
  std::vector<Voice> voices;
  try { voices.reserve(voice_count); } catch (const std::exception&) { return false; }
  for (uint32_t i = 0; i < voice_count; ++i) {
    Voice v;
    if (!ReadPod(in, v.id)) return false;
    if (!ReadPod(in, v.channels)) return false;
    if (!ReadPod(in, v.sample_rate)) return false;
    if (!ReadPod(in, v.loop)) return false;
    if (!ReadPod(in, v.volume)) return false;
    if (!ReadPod(in, v.paused)) return false;
    if (!ReadPod(in, v.position_frames)) return false;
    if (!ReadPod(in, v.finished)) return false;
    uint32_t sample_count = 0;
    if (!ReadPod(in, sample_count) || sample_count > kMaxSamplesPerVoice) return false;
    std::shared_ptr<std::vector<int16_t>> samples;
    try {
      samples = std::make_shared<std::vector<int16_t>>(sample_count);
    } catch (const std::exception&) {
      return false;
    }
    if (sample_count != 0) {
      in.read(reinterpret_cast<char*>(samples->data()),
              static_cast<std::streamsize>(sample_count * sizeof(int16_t)));
      if (!in.good()) return false;
    }
    v.samples = std::move(samples);
    voices.push_back(std::move(v));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  next_id_ = next_id;
  voices_ = std::move(voices);
  return true;
}

}  // namespace zeebulator
