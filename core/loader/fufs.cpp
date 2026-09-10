#include "core/loader/fufs.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace zeebulator {

namespace {

constexpr size_t kHeaderSize = 12;
constexpr size_t kRecordSize = 12;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Infla o rodape de nomes: u32 LE com o tamanho cru seguido de um stream
// zlib. Os 4 arquivos do corpus que tem esse rodape terminam SEM os 4
// bytes finais de adler32 (o stream fica "incompleto" para o zlib), por
// isso Z_STREAM_END nao e exigido -- basta ter produzido exatamente os
// bytes declarados. Devolve vazio em qualquer erro.
std::vector<uint8_t> InflateNameBlob(const uint8_t* data, size_t size, size_t at) {
  if (at + 4 > size) return {};
  uint32_t declared = ReadU32LE(data + at);
  if (declared == 0 || declared > 64u * 1024u * 1024u) return {};
  size_t stream_size = size - (at + 4);
  if (stream_size == 0) return {};

  std::vector<uint8_t> out(declared);
  z_stream strm{};
  if (inflateInit(&strm) != Z_OK) return {};
  strm.next_in = const_cast<Bytef*>(data + at + 4);
  strm.avail_in = static_cast<uInt>(stream_size);
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  int ret = inflate(&strm, Z_FINISH);
  bool complete = (strm.total_out == declared) && (ret == Z_STREAM_END || ret == Z_OK ||
                                                   ret == Z_BUF_ERROR);
  inflateEnd(&strm);
  if (!complete) return {};
  return out;
}

// Quebra um bloco de nomes terminados em NUL. Exige exatamente `count`
// nomes e que o bloco termine em NUL (sem sobra). Devolve vazio se nao
// bater -- nome errado e pior que nome nenhum.
std::vector<std::string> SplitNames(const uint8_t* blob, size_t size, uint32_t count) {
  if (size == 0 || blob[size - 1] != 0) return {};
  std::vector<std::string> names;
  names.reserve(count);
  size_t start = 0;
  for (size_t i = 0; i < size; ++i) {
    if (blob[i] != 0) continue;
    if (names.size() == count) return {};  // nomes a mais
    names.emplace_back(reinterpret_cast<const char*>(blob + start), i - start);
    start = i + 1;
  }
  if (names.size() != count) return {};
  return names;
}

}  // namespace

uint32_t FufsArchive::HashName(std::string_view name) {
  uint32_t h = 0;
  for (unsigned char c : name) {
    uint32_t up = static_cast<uint32_t>(std::toupper(c));
    h = h * 67u + up - 113u;
  }
  return h;
}

std::optional<FufsArchive> FufsArchive::Parse(std::vector<uint8_t> data) {
  const size_t size = data.size();
  if (size < kHeaderSize) return std::nullopt;
  const uint8_t* d = data.data();
  if (std::memcmp(d, "FUFS", 4) != 0) return std::nullopt;

  // Campo 4: o bit 31 esta setado nos 6 arquivos medidos; nao e exigido
  // aqui (nao ha como saber se e obrigatorio), mas os 31 bits baixos
  // precisam cair dentro do arquivo e depois do fim dos dados.
  const uint32_t tail_field = ReadU32LE(d + 4);
  const uint64_t names_end = tail_field & 0x7fffffffu;
  const uint32_t count = ReadU32LE(d + 8);

  if (count == 0) return std::nullopt;
  if (static_cast<uint64_t>(count) > (size - kHeaderSize) / kRecordSize) return std::nullopt;
  const uint64_t table_end = kHeaderSize + static_cast<uint64_t>(count) * kRecordSize;
  if (names_end > size || names_end < table_end) return std::nullopt;

  FufsArchive archive;
  archive.entries_.reserve(count);

  uint64_t expected_offset = table_end;
  uint32_t previous_hash = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* rec = d + kHeaderSize + static_cast<size_t>(i) * kRecordSize;
    FufsEntry entry;
    entry.offset = ReadU32LE(rec);
    entry.name_hash = ReadU32LE(rec + 4);
    entry.size = ReadU32LE(rec + 8);

    // Hash estritamente crescente: e o que a busca binaria pressupoe e,
    // na pratica, o filtro que rejeita lixo com o magic certo.
    if (i != 0 && entry.name_hash <= previous_hash) return std::nullopt;
    previous_hash = entry.name_hash;

    // Payloads contiguos, comecando no fim da tabela (medido nos 6
    // arquivos): sem buraco, sem sobreposicao, tudo dentro do arquivo.
    if (entry.offset != expected_offset) return std::nullopt;
    const uint64_t entry_end = static_cast<uint64_t>(entry.offset) + entry.size;
    if (entry_end > names_end) return std::nullopt;
    expected_offset = entry_end;

    archive.entries_.push_back(std::move(entry));
  }

  const uint64_t data_end = expected_offset;

  // Lista de nomes (opcional): primeiro a copia em texto puro entre o fim
  // dos dados e `names_end`; se ela nao servir, o rodape zlib no proprio
  // `names_end`.
  std::vector<std::string> names;
  if (names_end > data_end) {
    names = SplitNames(d + data_end, static_cast<size_t>(names_end - data_end), count);
  }
  if (names.empty()) {
    std::vector<uint8_t> inflated = InflateNameBlob(d, size, static_cast<size_t>(names_end));
    if (!inflated.empty()) {
      names = SplitNames(inflated.data(), inflated.size(), count);
    }
  }
  if (!names.empty()) {
    bool all_match = true;
    for (uint32_t i = 0; i < count; ++i) {
      if (HashName(names[i]) != archive.entries_[i].name_hash) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      for (uint32_t i = 0; i < count; ++i) archive.entries_[i].name = std::move(names[i]);
      archive.has_names_ = true;
    }
  }

  archive.data_ = std::move(data);
  return archive;
}

const FufsEntry* FufsArchive::FindByHash(uint32_t hash) const {
  auto it = std::lower_bound(
      entries_.begin(), entries_.end(), hash,
      [](const FufsEntry& e, uint32_t h) { return e.name_hash < h; });
  if (it == entries_.end() || it->name_hash != hash) return nullptr;
  return &*it;
}

std::vector<uint8_t> FufsArchive::Extract(const FufsEntry& entry) const {
  if (static_cast<uint64_t>(entry.offset) + entry.size > data_.size()) return {};
  const uint8_t* start = data_.data() + entry.offset;
  return std::vector<uint8_t>(start, start + entry.size);
}

}  // namespace zeebulator
