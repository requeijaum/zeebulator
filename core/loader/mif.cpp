#include "core/loader/mif.h"

namespace zeebulator {

std::vector<MifString> ExtractMifStrings(const uint8_t* data, size_t size) {
  std::vector<MifString> results;
  size_t i = 0;
  while (i + 1 < size) {
    if (data[i] != 0xFF || data[i + 1] != 0xFE) {
      ++i;
      continue;
    }

    size_t start = i;
    size_t j = i + 2;
    std::string text;
    bool clean = true;
    while (j + 1 < size) {
      uint16_t code = static_cast<uint16_t>(data[j]) |
                       (static_cast<uint16_t>(data[j + 1]) << 8);
      if (code == 0) {
        j += 2;
        break;
      }
      if (code == 0xFEFF) break;  // next BOM, no null terminator here
      if (code < 32 || code > 126) {
        clean = false;
      } else {
        text.push_back(static_cast<char>(code));
      }
      j += 2;
    }

    if (clean && !text.empty()) {
      results.push_back(MifString{static_cast<uint32_t>(start), text});
    }
    i = j;
  }
  return results;
}


std::vector<MifString> ExtractMifStringPrefixes(const uint8_t* data, size_t size,
                                               size_t min_len) {
  std::vector<MifString> results;
  size_t i = 0;
  while (i + 1 < size) {
    if (data[i] != 0xFF || data[i + 1] != 0xFE) {
      ++i;
      continue;
    }
    const size_t start = i;
    size_t j = i + 2;
    std::string text;
    while (j + 1 < size) {
      const uint16_t code = static_cast<uint16_t>(data[j]) |
                            (static_cast<uint16_t>(data[j + 1]) << 8);
      if (code == 0 || code == 0xFEFF) break;
      // Termina aqui em vez de invalidar a string inteira: e o unico ponto em
      // que esta versao difere da estrita.
      if (code < 32 || code > 126) break;
      text.push_back(static_cast<char>(code));
      j += 2;
    }
    if (!text.empty() && text.size() >= min_len) {
      results.push_back(MifString{static_cast<uint32_t>(start), text});
    }
    i = (j > start) ? j : start + 2;
  }
  return results;
}

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Faixas de ClassID que aparecem no corpus: a faixa BREW padrao (0x01xxxxxx) e
// a faixa alta usada por alguns titulos licenciados (zenonia, por exemplo, usa
// 0xBF2E...). Fora disso nao e um ClassID, e sim outro campo do registro.
bool PlausibleClassId(uint32_t v) {
  return (v >= 0x01000000u && v <= 0x01FFFFFFu) || v >= 0xB0000000u;
}

}  // namespace

namespace {

// Cabecalho comum a ExtractMifSections/Applet/Extension.
struct MifHeader {
  uint32_t section_table_offset;
  uint32_t section_count;
};

bool ReadMifHeader(const uint8_t* data, size_t size, MifHeader* out) {
  static const uint8_t kMagic[6] = {0x11, 0x00, 0x01, 0x00, 0x01, 0x00};
  if (data == nullptr || size < 0x20) return false;
  for (size_t i = 0; i < sizeof(kMagic); ++i) {
    if (data[i] != kMagic[i]) return false;
  }
  const uint32_t table_offset = ReadU32LE(data + 0x10);
  const uint32_t count = ReadU32LE(data + 0x14);
  // Mesmo limite ja usado por ExtractMifClassIds: o maior .mif do corpus
  // (280386) declara 23 secoes; um numero grande aqui e cabecalho corrompido.
  if (count > 64) return false;
  const size_t table_bytes = static_cast<size_t>(count + 1) * 4;
  if (table_offset > size || table_bytes > size - table_offset) return false;
  out->section_table_offset = table_offset;
  out->section_count = count;
  return true;
}

}  // namespace

std::vector<MifSection> ExtractMifSections(const uint8_t* data, size_t size) {
  std::vector<MifSection> out;
  MifHeader h{};
  if (!ReadMifHeader(data, size, &h)) return out;
  for (uint32_t i = 0; i < h.section_count; ++i) {
    const uint32_t start = ReadU32LE(data + h.section_table_offset + i * 4);
    const uint32_t end = ReadU32LE(data + h.section_table_offset + (i + 1) * 4);
    // Offsets fora do arquivo ou em ordem decrescente sao lixo, nao secao.
    if (end < start || end > size) continue;
    out.push_back(MifSection{start, end - start});
  }
  return out;
}

std::vector<uint32_t> ExtractMifAppletClassIds(const uint8_t* data, size_t size) {
  std::vector<uint32_t> out;
  for (const MifSection& s : ExtractMifSections(data, size)) {
    // 20 bytes, com a 2a e a 4a palavra zeradas. Medido no corpus: as tres
    // secoes de 20 bytes que NAO sao applet (Prey Evil, Reckless Racing e
    // Zeebo App) comecam em 0x00xxfeff -- sao texto UTF-16 -- e tem essas
    // palavras diferentes de zero.
    if (s.size != 20 || s.offset + 20 > size) continue;
    if (ReadU32LE(data + s.offset + 4) != 0) continue;
    if (ReadU32LE(data + s.offset + 12) != 0) continue;
    const uint32_t cls = ReadU32LE(data + s.offset);
    if (cls == 0) continue;
    out.push_back(cls);
  }
  return out;
}

std::vector<uint32_t> ExtractMifExtensionClassIds(const uint8_t* data, size_t size) {
  std::vector<uint32_t> out;
  // A regra medida: so um MIF SEM applet fornece classe (ver mif.h).
  if (!ExtractMifAppletClassIds(data, size).empty()) return out;
  for (const MifSection& s : ExtractMifSections(data, size)) {
    if (s.size != 8 || s.offset + 8 > size) continue;
    const uint32_t cls = ReadU32LE(data + s.offset);
    const uint32_t flags = ReadU32LE(data + s.offset + 4);
    if (flags != 0 || !PlausibleClassId(cls)) continue;
    bool seen = false;
    for (uint32_t already : out) {
      if (already == cls) { seen = true; break; }
    }
    if (!seen) out.push_back(cls);
  }
  return out;
}

std::vector<uint32_t> ExtractMifClassIds(const uint8_t* data, size_t size) {
  std::vector<uint32_t> out;
  static const uint8_t kMagic[6] = {0x11, 0x00, 0x01, 0x00, 0x01, 0x00};
  if (data == nullptr || size < 0x20) return out;
  for (size_t i = 0; i < sizeof(kMagic); ++i) {
    if (data[i] != kMagic[i]) return out;
  }
  const uint32_t table_offset = ReadU32LE(data + 0x10);
  const uint32_t count = ReadU32LE(data + 0x14);
  // Um MIF real do corpus declara poucas dezenas de secoes; um numero grande
  // aqui significa cabecalho corrompido, nao um arquivo legitimo.
  if (count > 64) return out;
  const size_t table_bytes = static_cast<size_t>(count + 1) * 4;
  if (table_offset > size || table_bytes > size - table_offset) return out;

  for (uint32_t i = 0; i <= count; ++i) {
    const uint32_t section = ReadU32LE(data + table_offset + i * 4);
    if (section + 4 > size) continue;
    const uint32_t value = ReadU32LE(data + section);
    if (!PlausibleClassId(value)) continue;
    // Secoes podem se sobrepor (os registros ficam a 8 bytes de distancia e a
    // tabela lista varias entradas dentro do mesmo bloco), entao o mesmo
    // ClassID aparece mais de uma vez -- manter a ordem, sem repetir.
    bool seen = false;
    for (uint32_t already : out) {
      if (already == value) { seen = true; break; }
    }
    if (!seen) out.push_back(value);
  }
  return out;
}

}  // namespace zeebulator
