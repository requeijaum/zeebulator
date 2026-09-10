#include "core/loader/aez.h"

#include <cstring>

#include <zlib.h>

namespace zeebulator {
namespace {

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Marca de entrada armazenada crua no campo de tamanho comprimido.
constexpr uint32_t kStoredMarker = 0xFFFFFFFFu;

// Um nome de asset real do corpus tem entre 1 e 255 bytes (o campo e u8), mas
// so aceitamos ASCII imprimivel: e o que separa um AEZ de verdade de bytes
// aleatorios que por acaso comecem com um byte pequeno.
bool LooksLikeName(const uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7e) return false;
  }
  return true;
}

}  // namespace

std::optional<AezArchive> AezArchive::Parse(std::vector<uint8_t> data) {
  AezArchive out;
  size_t pos = 0;
  while (pos < data.size()) {
    const uint32_t name_len = data[pos];
    if (name_len == 0) return std::nullopt;
    if (pos + 1 + name_len + 8 > data.size()) return std::nullopt;
    if (!LooksLikeName(data.data() + pos + 1, name_len)) return std::nullopt;

    AezEntry e;
    e.name.assign(reinterpret_cast<const char*>(data.data() + pos + 1), name_len);
    e.decompressed_size = ReadU32LE(data.data() + pos + 1 + name_len);
    const uint32_t compressed_size = ReadU32LE(data.data() + pos + 1 + name_len + 4);
    e.payload_offset = static_cast<uint32_t>(pos + 1 + name_len + 8);
    e.stored = (compressed_size == kStoredMarker);
    e.payload_size = e.stored ? e.decompressed_size : compressed_size;

    // Um payload que ultrapassa o fim do arquivo significa que a geometria
    // esta errada -- recusa em vez de ler fora dos limites.
    if (static_cast<size_t>(e.payload_offset) + e.payload_size > data.size()) {
      return std::nullopt;
    }
    pos = static_cast<size_t>(e.payload_offset) + e.payload_size;
    out.entries_.push_back(std::move(e));
  }
  // Um AEZ valido termina exatamente no fim do arquivo e tem ao menos uma
  // entrada. Exigir os dois evita aceitar lixo que casualmente decodifique
  // um registro plausivel e pare no meio.
  if (out.entries_.empty() || pos != data.size()) return std::nullopt;
  out.data_ = std::move(data);
  return out;
}

std::optional<std::vector<uint8_t>> AezArchive::Extract(const AezEntry& entry) const {
  if (static_cast<size_t>(entry.payload_offset) + entry.payload_size > data_.size()) {
    return std::nullopt;
  }
  const uint8_t* src = data_.data() + entry.payload_offset;
  if (entry.stored) {
    return std::vector<uint8_t>(src, src + entry.payload_size);
  }

  std::vector<uint8_t> out(entry.decompressed_size);
  z_stream strm{};
  strm.next_in = const_cast<Bytef*>(src);
  strm.avail_in = entry.payload_size;
  strm.next_out = out.empty() ? nullptr : out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  // 15 + 16: janela maxima, com deteccao de cabecalho gzip (RFC 1952), igual
  // ao que GgzArchive::Extract ja faz para os .ggz.
  if (inflateInit2(&strm, 15 + 16) != Z_OK) return std::nullopt;
  const int ret = inflate(&strm, Z_FINISH);
  const uLong produced = strm.total_out;
  inflateEnd(&strm);
  if (ret != Z_STREAM_END || produced != entry.decompressed_size) return std::nullopt;
  return out;
}

}  // namespace zeebulator
