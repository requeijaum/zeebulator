#include "core/loader/mif.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Monta um MIF minimo com a geometria medida: magico, contagem, tabela de
// offsets em 0x10/0x14, e registros de 8 bytes {ClassID, flags}.
std::vector<uint8_t> BuildMif(const std::vector<uint32_t>& class_ids) {
  std::vector<uint8_t> d(0x20, 0);
  const uint8_t magic[6] = {0x11, 0x00, 0x01, 0x00, 0x01, 0x00};
  for (int i = 0; i < 6; ++i) d[i] = magic[i];
  const uint32_t count = static_cast<uint32_t>(class_ids.size()) - 1;
  const uint32_t table_offset = 0x20;
  auto put = [&d](size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (i * 8));
  };
  put(0x10, table_offset);
  put(0x14, count);
  const uint32_t records = table_offset + (count + 1) * 4;
  d.resize(records + class_ids.size() * 8, 0);
  for (size_t i = 0; i < class_ids.size(); ++i) {
    const uint32_t rec = records + static_cast<uint32_t>(i) * 8;
    d.resize(std::max<size_t>(d.size(), rec + 8), 0);
    put(table_offset + i * 4, rec);
    put(rec, class_ids[i]);
  }
  return d;
}

}  // namespace

TEST(MifClassIdsTest, ReadsDeclarationOrder) {
  auto d = BuildMif({0x01070798u, 0x0102febdu});
  auto ids = zeebulator::ExtractMifClassIds(d.data(), d.size());
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], 0x01070798u);
  EXPECT_EQ(ids[1], 0x0102febdu);
}

// A faixa alta e real no corpus: zenonia usa um ClassID >= 0xB0000000.
TEST(MifClassIdsTest, AcceptsHighRangeClassId) {
  auto d = BuildMif({0xBF2E0121u});
  auto ids = zeebulator::ExtractMifClassIds(d.data(), d.size());
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], 0xBF2E0121u);
}

// CONTROLE NEGATIVO: sem o magico nao e um MIF.
TEST(MifClassIdsTest, RejectsWrongMagic) {
  auto d = BuildMif({0x01070798u});
  d[0] = 0x12;
  EXPECT_TRUE(zeebulator::ExtractMifClassIds(d.data(), d.size()).empty());
}

// CONTROLE NEGATIVO: bytes aleatorios nao podem produzir ClassIDs.
TEST(MifClassIdsTest, RejectsRandomBytes) {
  std::vector<uint8_t> junk;
  uint32_t state = 987654321u;
  for (int i = 0; i < 2048; ++i) {
    state = state * 1103515245u + 12345u;
    junk.push_back(static_cast<uint8_t>(state >> 16));
  }
  EXPECT_TRUE(zeebulator::ExtractMifClassIds(junk.data(), junk.size()).empty());
}

// CONTROLE NEGATIVO: tabela apontando para fora do arquivo nao pode ler alem
// do fim nem devolver lixo.
TEST(MifClassIdsTest, RejectsOutOfRangeTable) {
  auto d = BuildMif({0x01070798u});
  d[0x10] = 0xFF;
  d[0x11] = 0xFF;
  EXPECT_TRUE(zeebulator::ExtractMifClassIds(d.data(), d.size()).empty());
}

// CONTROLE NEGATIVO: contagem absurda no cabecalho e recusada.
TEST(MifClassIdsTest, RejectsAbsurdCount) {
  auto d = BuildMif({0x01070798u});
  d[0x14] = 0xFF;
  d[0x15] = 0xFF;
  EXPECT_TRUE(zeebulator::ExtractMifClassIds(d.data(), d.size()).empty());
}

// Um mesmo ClassID alcancado por duas entradas da tabela aparece uma vez so.
TEST(MifClassIdsTest, DeduplicatesRepeatedSections) {
  auto d = BuildMif({0x01070798u, 0x01070798u});
  auto ids = zeebulator::ExtractMifClassIds(d.data(), d.size());
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], 0x01070798u);
}
