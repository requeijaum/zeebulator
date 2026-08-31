#include "core/loader/mif.h"

#include <gtest/gtest.h>

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
