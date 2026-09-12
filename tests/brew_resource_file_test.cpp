// Testes do parser de arquivo de recurso do BREW (`.brf`).
//
// Dados SINTETICOS, montados aqui byte a byte -- a politica deste repositorio
// nao commita asset real de jogo (ver CONTRIBUTING.md). O layout que o parser
// le esta documentado em core/brew/brew_resource_file.h, e foi medido em dois
// arquivos reais de mod/274755; o que estes testes provam e que o parser le
// exatamente aquele layout, inclusive a ordem de bytes das strings.

#include "core/brew/brew_resource_file.h"

#include <gtest/gtest.h>

namespace {

void Put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
  b[o] = static_cast<uint8_t>(v);
  b[o + 1] = static_cast<uint8_t>(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  b[o] = static_cast<uint8_t>(v);
  b[o + 1] = static_cast<uint8_t>(v >> 8);
  b[o + 2] = static_cast<uint8_t>(v >> 16);
  b[o + 3] = static_cast<uint8_t>(v >> 24);
}

// Monta um .brf com dois recursos: uma string UTF-16LE com BOM (id 1002) e um
// blob binario qualquer (id 5005). Os deslocamentos sao absolutos, como nos
// arquivos reais.
std::vector<uint8_t> BuildBrf() {
  const std::vector<uint16_t> text = {'O', 'l', 'a'};
  const std::vector<uint8_t> blob = {0x42, 0x4D, 0x01, 0x02, 0x03};
  const size_t string_off = 0x100;
  const size_t blob_off = string_off + 2 + text.size() * 2;
  const size_t end = blob_off + blob.size();
  std::vector<uint8_t> b(end, 0);

  Put16(b, 0x00, 0x11);
  Put16(b, 0x02, 1);
  Put16(b, 0x04, 1);
  Put16(b, 0x06, 2);              // nRegistros
  Put32(b, 0x08, 0x20);           // inicio dos registros
  Put32(b, 0x0c, 2 * 8);          // tamanho da area de registros
  Put32(b, 0x10, 0x20 + 2 * 8);   // tabela de deslocamentos
  Put32(b, 0x14, 2);              // nDeslocamentos
  Put32(b, 0x18, string_off);     // inicio dos dados
  Put32(b, 0x1c, static_cast<uint32_t>(end));  // fim dos dados

  // registros: {tipo, id, indice, sinalizadores}
  // {tipo, id, campo desconhecido, INDICE} -- o indice e o ULTIMO u16.
  Put16(b, 0x20 + 0, 1);
  Put16(b, 0x20 + 2, 1002);
  Put16(b, 0x20 + 4, 0);
  Put16(b, 0x20 + 6, 0);
  Put16(b, 0x28 + 0, 6);
  Put16(b, 0x28 + 2, 5005);
  Put16(b, 0x28 + 4, 0);
  Put16(b, 0x28 + 6, 1);

  // tabela de deslocamentos
  Put32(b, 0x20 + 2 * 8 + 0, static_cast<uint32_t>(string_off));
  Put32(b, 0x20 + 2 * 8 + 4, static_cast<uint32_t>(blob_off));

  // string: BOM UTF-16LE + code units + terminador
  // BOM little-endian: os bytes sao FF FE (U+FEFF gravado em little-endian).
  b[string_off] = 0xFF;
  b[string_off + 1] = 0xFE;
  for (size_t i = 0; i < text.size(); ++i) Put16(b, string_off + 2 + i * 2, text[i]);
  Put16(b, string_off + 2 + text.size() * 2, 0);
  for (size_t i = 0; i < blob.size(); ++i) b[blob_off + i] = blob[i];
  return b;
}

}  // namespace

TEST(BrewResourceFile, ReadsTheDirectoryOfAWellFormedFile) {
  const std::vector<uint8_t> b = BuildBrf();
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  EXPECT_EQ(dir.record_count, 2);
  EXPECT_EQ(dir.offset_count, 2);
  EXPECT_EQ(dir.offset_table, 0x30u);
}

TEST(BrewResourceFile, RejectsAFileWhoseDirectoryDoesNotStartAfterTheRecords) {
  std::vector<uint8_t> b = BuildBrf();
  Put32(b, 0x10, 0x40);  // tabela no lugar errado
  zeebulator::BrewResourceDirectory dir;
  EXPECT_FALSE(zeebulator::ParseBrewResourceDirectory(b, &dir));
}

TEST(BrewResourceFile, DecodesAUtf16StringWithBom) {
  const std::vector<uint8_t> b = BuildBrf();
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  std::vector<uint16_t> chars;
  ASSERT_TRUE(zeebulator::ReadBrewResourceString(b, dir, 1002, &chars));
  ASSERT_EQ(chars.size(), 3u);
  EXPECT_EQ(chars[0], 'O');
  EXPECT_EQ(chars[1], 'l');
  EXPECT_EQ(chars[2], 'a');
}

TEST(BrewResourceFile, ReturnsTheRawBytesOfANonStringResource) {
  const std::vector<uint8_t> b = BuildBrf();
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  uint32_t start = 0;
  uint32_t size = 0;
  ASSERT_TRUE(zeebulator::ReadBrewResourceRecord(b, dir, 6, 5005, /*type_match_any=*/false, &start,
                                                 &size));
  EXPECT_EQ(size, 5u);
  EXPECT_EQ(b[start], 0x42);
  EXPECT_EQ(b[start + 1], 0x4D);
}

TEST(BrewResourceFile, FindsAResourceWhenOnlyTheIdIsKnown) {
  const std::vector<uint8_t> b = BuildBrf();
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  uint32_t start = 0;
  uint32_t size = 0;
  ASSERT_TRUE(zeebulator::ReadBrewResourceRecord(b, dir, /*type=*/1234, 5005,
                                                 /*type_match_any=*/true, &start, &size));
  EXPECT_EQ(size, 5u);
}

TEST(BrewResourceFile, RejectsAnIdThatIsNotInTheFile) {
  const std::vector<uint8_t> b = BuildBrf();
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  uint32_t start = 0;
  uint32_t size = 0;
  EXPECT_FALSE(zeebulator::ReadBrewResourceRecord(b, dir, 1, 4242, true, &start, &size));
  std::vector<uint16_t> chars;
  EXPECT_FALSE(zeebulator::ReadBrewResourceString(b, dir, 4242, &chars));
}

TEST(BrewResourceFile, ReadsBigEndianStringsWhenTheBomSaysSo) {
  std::vector<uint8_t> b = BuildBrf();
  // BOM big-endian: os bytes sao FE FF, e cada code unit seguinte vem com o
  // byte mais significativo PRIMEIRO (00 4F = 'O').
  b[0x100] = 0xFE;
  b[0x101] = 0xFF;
  const uint8_t be_text[] = {0x00, 0x4F, 0x00, 0x6C, 0x00, 0x61, 0x00, 0x00};
  for (size_t i = 0; i < sizeof(be_text); ++i) b[0x102 + i] = be_text[i];
  zeebulator::BrewResourceDirectory dir;
  ASSERT_TRUE(zeebulator::ParseBrewResourceDirectory(b, &dir));
  std::vector<uint16_t> chars;
  ASSERT_TRUE(zeebulator::ReadBrewResourceString(b, dir, 1002, &chars));
  ASSERT_EQ(chars.size(), 3u);
  EXPECT_EQ(chars[0], 'O');
  EXPECT_EQ(chars[2], 'a');
}

TEST(BrewResourceFile, RejectsNonCanonicalRecordOriginInsteadOfReadingHardcodedTwenty) {
  auto b = BuildBrf();
  // Parser antigo aceitava 0x28, mas ReadBrewResourceRecord continuava lendo
  // registros em 0x20. O formato real medido fixa a origem em 0x20.
  b[8] = 0x28;
  zeebulator::BrewResourceDirectory dir;
  EXPECT_FALSE(zeebulator::ParseBrewResourceDirectory(b, &dir));
}
