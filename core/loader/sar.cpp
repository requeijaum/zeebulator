#include "core/loader/sar.h"

#include <cstring>

namespace zeebulator {
namespace {

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// AB 'SWVARC' BB 0D 0A 1A 0A -- 12 bytes, no estilo da assinatura do PNG.
constexpr uint8_t kMagic[12] = {0xAB, 'S', 'W', 'V', 'A', 'R',
                                'C',  0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
// Fim do cabecalho fixo = inicio do primeiro registro do indice.
constexpr size_t kHeaderSize = 0x25;
// Bytes de um registro fora o nome: 4 campos u32 antes + NUL + u32 depois.
constexpr size_t kRecordOverhead = 21;
// O indice termina com um unico byte 0x00 antes da area de payloads.
constexpr size_t kIndexTerminator = 1;

// Nomes reais do corpus sao ASCII imprimivel. E o que separa um SAR de bytes
// aleatorios que por acaso tenham um tamanho de registro plausivel.
bool LooksLikeName(const uint8_t* p, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7e) return false;
  }
  return true;
}

}  // namespace

std::optional<SarArchive> SarArchive::Parse(std::vector<uint8_t> data) {
  if (data.size() < kHeaderSize + kIndexTerminator) return std::nullopt;
  if (std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) return std::nullopt;
  // Byte de versao/flags: 0x00 nos 53 arquivos medidos. Qualquer outro valor e
  // uma variante que este parser nao levantou -- recusa em vez de adivinhar.
  if (data[0x0c] != 0x00) return std::nullopt;

  const uint32_t data_offset = ReadU32LE(data.data() + 0x0d);
  const uint32_t total_size = ReadU32LE(data.data() + 0x11);
  const uint32_t count = ReadU32LE(data.data() + 0x21);

  // O tamanho declarado tem que ser exatamente o do arquivo: pega truncamento
  // e sobra de lixo no fim de uma vez so.
  if (total_size != data.size()) return std::nullopt;
  if (data_offset < kHeaderSize + kIndexTerminator || data_offset > data.size()) {
    return std::nullopt;
  }
  if (count == 0) return std::nullopt;
  // Limite derivado do proprio arquivo, nao um numero inventado: o menor
  // registro possivel tem kRecordOverhead + 1 bytes (nome de 1 caractere).
  const size_t index_bytes = data_offset - kHeaderSize - kIndexTerminator;
  if (count > index_bytes / (kRecordOverhead + 1)) return std::nullopt;

  SarArchive out;
  out.timestamp_ = ReadU32LE(data.data() + 0x15);
  out.header_hash_ = ReadU32LE(data.data() + 0x1d);
  out.entries_.reserve(count);

  const size_t index_end = data_offset - kIndexTerminator;
  size_t pos = kHeaderSize;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 16 > index_end) return std::nullopt;
    const uint32_t rec_size = ReadU32LE(data.data() + pos);
    if (rec_size < kRecordOverhead + 1) return std::nullopt;
    if (rec_size > index_end - pos) return std::nullopt;

    const uint8_t* name_start = data.data() + pos + 16;
    const size_t name_max = rec_size - kRecordOverhead;  // sem NUL nem hash final
    if (!LooksLikeName(name_start, name_max)) return std::nullopt;
    if (name_start[name_max] != 0x00) return std::nullopt;  // NUL no lugar exato

    SarEntry e;
    e.name.assign(reinterpret_cast<const char*>(name_start), name_max);
    e.timestamp = ReadU32LE(data.data() + pos + 4);
    e.payload_size = ReadU32LE(data.data() + pos + 8);
    e.name_hash = ReadU32LE(data.data() + pos + 12);
    e.content_hash = ReadU32LE(data.data() + pos + 16 + name_max + 1);
    e.payload_offset = 0;  // preenchido abaixo, quando a area de payload e conferida
    out.entries_.push_back(std::move(e));
    pos += rec_size;
  }
  // Os registros tem que cobrir o indice inteiro e parar no terminador.
  if (pos != index_end) return std::nullopt;
  if (data[index_end] != 0x00) return std::nullopt;

  // Payloads contiguos, na ordem do indice, cobrindo exatamente o resto do
  // arquivo: sem buraco, sem sobreposicao e sem sobra.
  uint64_t off = data_offset;
  for (SarEntry& e : out.entries_) {
    if (off + e.payload_size > data.size()) return std::nullopt;
    e.payload_offset = static_cast<uint32_t>(off);
    off += e.payload_size;
  }
  if (off != data.size()) return std::nullopt;

  out.data_ = std::move(data);
  return out;
}

std::optional<std::vector<uint8_t>> SarArchive::Extract(const SarEntry& entry) const {
  const uint64_t end = static_cast<uint64_t>(entry.payload_offset) + entry.payload_size;
  if (end > data_.size()) return std::nullopt;
  const uint8_t* src = data_.data() + entry.payload_offset;
  return std::vector<uint8_t>(src, src + entry.payload_size);
}

const SarEntry* SarArchive::Find(std::string_view name) const {
  for (const SarEntry& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

}  // namespace zeebulator
