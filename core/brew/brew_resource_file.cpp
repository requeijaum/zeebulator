#include "core/brew/brew_resource_file.h"

namespace zeebulator {
namespace {

inline uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) {
  return static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
}
inline uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) {
  return static_cast<uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
}

}  // namespace

bool ParseBrewResourceDirectory(const std::vector<uint8_t>& file, BrewResourceDirectory* out) {
  if (out == nullptr || file.size() < 0x20) return false;
  const uint32_t records_start = Rd32(file, 0x08);
  out->record_count = Rd16(file, 0x06);
  const uint32_t records_bytes = Rd32(file, 0x0c);
  out->offset_table = Rd32(file, 0x10);
  out->offset_count = Rd32(file, 0x14);
  const uint32_t data_start = Rd32(file, 0x18);
  out->data_end = Rd32(file, 0x1c);
  if (out->record_count == 0 || out->offset_count == 0) return false;
  // O layout medido fixa os registros em +0x20. Aceitar outro valor e depois
  // ler de 0x20 validava uma origem e consumia outra.
  if (records_start != 0x20) return false;
  // Dois campos que se conferem entre si, nos DOIS arquivos reais: o tamanho
  // declarado da area de registros e igual a nRegistros*8, e a tabela de
  // deslocamentos comeca imediatamente depois dela.
  if (records_bytes != static_cast<uint32_t>(out->record_count) * 8u) return false;
  const uint64_t records_end = static_cast<uint64_t>(records_start) + records_bytes;
  if (records_end > file.size() || out->offset_table != records_end) return false;
  const uint64_t table_end = static_cast<uint64_t>(out->offset_table) +
                             static_cast<uint64_t>(out->offset_count) * 4u;
  if (table_end > data_start || data_start > file.size()) {
    return false;
  }
  // Em arquivos reais (ex: tectoyli.brf, tamanho 1439595), o campo data_end declarado
  // pode ser maior (0x195146) do que o tamanho em disco. Clamp seguro para o fim real do arquivo:
  if (out->data_end > file.size() || out->data_end < data_start) {
    out->data_end = static_cast<uint32_t>(file.size());
  }
  return true;
}

bool ReadBrewResourceRecord(const std::vector<uint8_t>& file, const BrewResourceDirectory& dir,
                            uint16_t type, uint16_t id, bool type_match_any, uint32_t* out_start,
                            uint32_t* out_size) {
  if (out_start == nullptr || out_size == nullptr) return false;
  const uint32_t data_start = Rd32(file, 0x18);
  for (uint16_t i = 0; i < dir.record_count; ++i) {
    const size_t rec = 0x20 + static_cast<size_t>(i) * 8;
    if (rec + 8 > file.size()) return false;
    if (Rd16(file, rec + 2) != id) continue;
    if (!type_match_any && Rd16(file, rec) != type) continue;
    // O indice e o ULTIMO u16 do registro (ver o cabecalho do .h).
    const uint16_t index = Rd16(file, rec + 6);
    if (index >= dir.offset_count) return false;
    const uint32_t start = Rd32(file, dir.offset_table + static_cast<size_t>(index) * 4);
    const uint32_t end = (static_cast<uint32_t>(index + 1) < dir.offset_count)
                             ? Rd32(file, dir.offset_table + static_cast<size_t>(index + 1) * 4)
                             : dir.data_end;
    if (start < data_start || start >= end || end > file.size()) return false;
    *out_start = start;
    *out_size = end - start;
    return true;
  }
  return false;
}

bool ReadBrewResourceString(const std::vector<uint8_t>& file, const BrewResourceDirectory& dir,
                            uint16_t id, std::vector<uint16_t>* out) {
  uint32_t start = 0;
  uint32_t size = 0;
  if (!ReadBrewResourceRecord(file, dir, /*type=*/1, id, /*type_match_any=*/false, &start, &size)) {
    return false;
  }
  // O BOM decide a ordem dos bytes, e a decisao e sobre os BYTES, nao sobre o
  // valor lido em little-endian: FF FE = U+FEFF gravado em little-endian (sem
  // troca), FE FF = U+FEFF gravado em big-endian (com troca). Comparar o valor
  // de Rd16 aqui inverte os dois casos -- foi exatamente o que um teste deste
  // parser pegou, e o efeito seria decodificar TODA string do arquivo real com
  // os bytes trocados.
  bool little_endian = true;
  size_t cursor = start;
  if (size >= 2) {
    const uint8_t b0 = file[start];
    const uint8_t b1 = file[start + 1];
    if (b0 == 0xFF && b1 == 0xFE) {
      cursor += 2;
    } else if (b0 == 0xFE && b1 == 0xFF) {
      little_endian = false;
      cursor += 2;
    }
  }
  const size_t end = start + size;
  out->clear();
  while (cursor + 1 < end) {
    const uint16_t raw = Rd16(file, cursor);
    if (raw == 0) break;
    out->push_back(little_endian ? raw : static_cast<uint16_t>((raw >> 8) | (raw << 8)));
    cursor += 2;
  }
  return true;
}

}  // namespace zeebulator
