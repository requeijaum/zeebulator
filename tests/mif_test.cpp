#include "core/loader/mif.h"

#include <gtest/gtest.h>

using zeebulator::ExtractMifStringPrefixes;
using zeebulator::ExtractMifStrings;
using zeebulator::MifString;

namespace {

void AppendUtf16String(std::vector<uint8_t>& buf, const std::string& text,
                        bool null_terminate = true) {
  buf.push_back(0xFF);
  buf.push_back(0xFE);
  for (char c : text) {
    buf.push_back(static_cast<uint8_t>(c));
    buf.push_back(0x00);
  }
  if (null_terminate) {
    buf.push_back(0x00);
    buf.push_back(0x00);
  }
}

}  // namespace

TEST(Mif, ExtractsSingleString) {
  std::vector<uint8_t> buf = {0xDE, 0xAD, 0xBE, 0xEF};  // leading junk
  uint32_t expected_offset = static_cast<uint32_t>(buf.size());
  AppendUtf16String(buf, "DOUBLE DRAGON Zeebo");

  auto strings = ExtractMifStrings(buf.data(), buf.size());
  ASSERT_EQ(strings.size(), 1u);
  EXPECT_EQ(strings[0].offset, expected_offset);
  EXPECT_EQ(strings[0].text, "DOUBLE DRAGON Zeebo");
}

TEST(Mif, BackToBackStringsWithNoNullSeparatorAreSplitCorrectly) {
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "Dragon Vs Chicken", /*null_terminate=*/false);
  AppendUtf16String(buf, "display1=a", /*null_terminate=*/true);

  auto strings = ExtractMifStrings(buf.data(), buf.size());
  ASSERT_EQ(strings.size(), 2u);
  EXPECT_EQ(strings[0].text, "Dragon Vs Chicken");
  EXPECT_EQ(strings[1].text, "display1=a");
}

TEST(Mif, MultipleStringsReportCorrectOffsets) {
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "Brizo Interactive Corp.");
  uint32_t second_offset = static_cast<uint32_t>(buf.size());
  AppendUtf16String(buf, "0.9.0");

  auto strings = ExtractMifStrings(buf.data(), buf.size());
  ASSERT_EQ(strings.size(), 2u);
  EXPECT_EQ(strings[0].offset, 0u);
  EXPECT_EQ(strings[1].offset, second_offset);
}

TEST(Mif, CoincidentalBomInBinaryDataWithNonPrintableContentIsFiltered) {
  std::vector<uint8_t> buf = {0xFF, 0xFE, 0x02, 0x10, 0x00, 0x00};  // fake BOM, garbage, null
  auto strings = ExtractMifStrings(buf.data(), buf.size());
  EXPECT_TRUE(strings.empty());
}

TEST(Mif, NoStringsInPureBinaryData) {
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  auto strings = ExtractMifStrings(buf.data(), buf.size());
  EXPECT_TRUE(strings.empty());
}

TEST(Mif, EmptyBufferProducesNoStrings) {
  std::vector<uint8_t> buf;
  EXPECT_TRUE(ExtractMifStrings(buf.data(), buf.size()).empty());
}

// Diferenca entre a versao estrita e a tolerante, com o caso REAL que a motivou.
//
// Medido no 277455.mif desta NAND: a string do titulo ("zenonia") e seguida pelo
// codigo 0x1000 e so depois por zeros. A versao estrita le o 0x1000, marca a
// string inteira como suja e a DESCARTA -- perdendo exatamente o nome do jogo.
// O mesmo acontece com "GOF" (277380), "3.0.0 B" (12875) e "VMGAME" (278200).
TEST(Mif, StrictVersionDropsAStringFollowedByANonPrintableCode) {
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "zenonia", /*null_terminate=*/false);
  // 0x1000 em UTF-16LE, depois zeros: e o que o arquivo real traz.
  buf.push_back(0x00);
  buf.push_back(0x10);
  for (int i = 0; i < 8; ++i) buf.push_back(0x00);
  EXPECT_TRUE(ExtractMifStrings(buf.data(), buf.size()).empty());
}

TEST(Mif, LenientVersionKeepsTheReadablePrefix) {
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "zenonia", /*null_terminate=*/false);
  buf.push_back(0x00);
  buf.push_back(0x10);
  for (int i = 0; i < 8; ++i) buf.push_back(0x00);
  const std::vector<MifString> got = ExtractMifStringPrefixes(buf.data(), buf.size());
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].text, "zenonia");
}

TEST(Mif, LenientVersionStillRejectsShortCoincidences) {
  // O risco que a versao estrita existe para evitar: um BOM que aparece por
  // coincidencia dentro de dados binarios. Sequencia curta demais nao vira nome.
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "ab", /*null_terminate=*/false);
  buf.push_back(0x00);
  buf.push_back(0x10);
  EXPECT_TRUE(ExtractMifStringPrefixes(buf.data(), buf.size()).empty());
  // Com comprimento suficiente, a mesma sequencia e aceita.
  std::vector<uint8_t> buf2;
  AppendUtf16String(buf2, "abc", /*null_terminate=*/false);
  buf2.push_back(0x00);
  buf2.push_back(0x10);
  EXPECT_EQ(ExtractMifStringPrefixes(buf2.data(), buf2.size()).size(), 1u);
}

TEST(Mif, LenientVersionHandlesSeveralStringsInSequence) {
  std::vector<uint8_t> buf;
  AppendUtf16String(buf, "(C)Gamevil");
  AppendUtf16String(buf, "zenonia", /*null_terminate=*/false);
  buf.push_back(0x00);
  buf.push_back(0x10);
  for (int i = 0; i < 8; ++i) buf.push_back(0x00);
  const std::vector<MifString> got = ExtractMifStringPrefixes(buf.data(), buf.size());
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].text, "(C)Gamevil");
  EXPECT_EQ(got[1].text, "zenonia");
}
