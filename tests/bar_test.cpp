// Tests use small, synthetic, hand-built ".bar" byte arrays -- the real
// format was reverse-engineered against Peggle's real `resources.bar`
// (TASKS.md Phase 2/8), but real game assets are never committed to
// this repo (see CONTRIBUTING.md's clean-room policy).

#include "core/loader/bar.h"

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>

using zeebulator::BarArchive;
using zeebulator::BarEntry;
using zeebulator::BarResourceId;

namespace {

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

void AppendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

// Builds a well-formed synthetic BAR archive: real 32-byte header, the
// real resource-ID directory (straight 8-byte records per
// `resource_ids`, no sub-header -- see bar.h's own doc comment on why
// there isn't one), the real offset table (entry_count offsets + one
// file-size sentinel), then the resources themselves back to back --
// matching every structural element confirmed against real files
// (core/loader/bar.h has the full layout writeup).
std::vector<uint8_t> BuildBar(const std::vector<std::vector<uint8_t>>& resources,
                               const std::vector<BarResourceId>& resource_ids = {}) {
  constexpr uint32_t kHeaderSize = 32;
  uint32_t table1_start = kHeaderSize;
  uint32_t table1_size = static_cast<uint32_t>(resource_ids.size()) * 8;
  uint32_t table_start = table1_start + table1_size;
  uint32_t entry_count = static_cast<uint32_t>(resources.size());
  uint32_t data_start = table_start + (entry_count + 1) * 4;

  uint32_t cursor = data_start;
  std::vector<uint32_t> offsets;
  for (const auto& r : resources) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(r.size());
  }
  offsets.push_back(cursor);  // sentinel = final file size
  uint32_t data_size = cursor - data_start;

  std::vector<uint8_t> out;
  AppendU32LE(out, 0x00010011);  // offset 0, unconfirmed
  AppendU32LE(out, 0x003e0001);  // offset 4, unconfirmed
  AppendU32LE(out, table1_start);
  AppendU32LE(out, table1_size);
  AppendU32LE(out, table_start);
  AppendU32LE(out, entry_count);
  AppendU32LE(out, data_start);
  AppendU32LE(out, data_size);
  for (const BarResourceId& r : resource_ids) {
    AppendU16LE(out, r.type);
    AppendU16LE(out, r.requested_id);
    AppendU16LE(out, r.unknown);
    AppendU16LE(out, r.entry_index);
  }

  for (uint32_t off : offsets) AppendU32LE(out, off);
  for (const auto& r : resources) out.insert(out.end(), r.begin(), r.end());
  return out;
}

}  // namespace

TEST(Bar, ParsesTwoEntriesAndExtractsCorrectData) {
  std::vector<uint8_t> res0 = {'R', 'I', 'F', 'F', 1, 2, 3, 4};
  std::vector<uint8_t> res1(100, 0xAB);
  auto blob = BuildBar({res0, res1});
  auto archive = BarArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 2u);
  EXPECT_EQ(archive.Extract(archive.Entries()[0]), res0);
  EXPECT_EQ(archive.Extract(archive.Entries()[1]), res1);
}

TEST(Bar, EntrySizeIsDerivedFromConsecutiveOffsets) {
  auto blob = BuildBar({std::vector<uint8_t>(10, 1), std::vector<uint8_t>(20, 2),
                         std::vector<uint8_t>(5, 3)});
  auto archive = BarArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 3u);
  EXPECT_EQ(archive.Entries()[0].size, 10u);
  EXPECT_EQ(archive.Entries()[1].size, 20u);
  EXPECT_EQ(archive.Entries()[2].size, 5u);
}

TEST(Bar, TooSmallFileIsRejected) {
  std::vector<uint8_t> tiny(16, 0);
  EXPECT_THROW(BarArchive::Parse(tiny), std::runtime_error);
}

TEST(Bar, WrongTable1StartOffsetIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[8] = 99;  // header offset 8 no longer says "32"
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, TableRunningPastEndOfFileIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[20] = 100;  // claim 100 entries; the real table only has room for 1
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, MismatchedDataSizeHeaderFieldIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[28] = 0xFF;  // corrupt the declared data-region size
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, DecreasingOffsetTableIsRejected) {
  auto blob = BuildBar({{1, 2, 3, 4}, {5, 6, 7, 8}});
  // Overwrite the second offset-table entry with something smaller
  // than the first, so it actually goes backwards (table starts right
  // after the 32-byte header; no resource IDs requested here, so
  // table1 is empty). Equal (not smaller) is legitimate -- see
  // ZeroLengthEntryFromEqualConsecutiveOffsetsIsAccepted.
  size_t table_start = 32;
  uint32_t first_offset;
  std::memcpy(&first_offset, blob.data() + table_start, 4);
  uint32_t smaller = first_offset - 1;
  std::memcpy(blob.data() + table_start + 4, &smaller, 4);
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, ZeroLengthEntryFromEqualConsecutiveOffsetsIsAccepted) {
  // Real, shipped title evidence, not a synthetic edge case: Alien
  // Breaker Deluxe's own real `data.bar` has a genuine zero-length
  // resource entry (two consecutive real offset-table values equal,
  // confirmed live against the actual file) -- rejecting this as
  // "corrupt" was this project's own bug, not a real file's fault.
  auto blob = BuildBar({{1, 2, 3, 4}, {}, {5, 6, 7, 8}});
  auto archive = BarArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 3u);
  EXPECT_EQ(archive.Entries()[1].size, 0u);
  EXPECT_TRUE(archive.Extract(archive.Entries()[1]).empty());
}

TEST(Bar, FindResolvesARealTypeIdPairToTheCorrectEntry) {
  std::vector<uint8_t> res0 = {'a'};
  std::vector<uint8_t> res1 = {'b', 'b'};
  std::vector<uint8_t> res2 = {'c', 'c', 'c'};
  auto blob = BuildBar({res0, res1, res2}, {BarResourceId{1, 4000, 3, 2}});
  auto archive = BarArchive::Parse(blob);

  const BarEntry* found = archive.Find(1, 4000);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(archive.Extract(*found), res2);
}

TEST(Bar, FindResolvesTheVeryFirstDirectoryRecordWithNoSkippedBytes) {
  // Real, shipped title evidence, not a synthetic edge case: the first
  // 8 bytes of the resource-ID sub-table were originally believed to
  // be part of an unconfirmed, skipped 16-byte sub-header -- wrong
  // (see bar.h's own doc comment): both real samples this project has
  // (Alien Breaker Deluxe, Peggle) decode those bytes as real,
  // sensible records too. This exercises exactly that: a real record
  // at index 0 (the bytes that used to be silently discarded) must
  // resolve correctly, not just later ones.
  std::vector<uint8_t> res0 = {'a'};
  std::vector<uint8_t> res1 = {'b', 'b'};
  auto blob = BuildBar({res0, res1}, {BarResourceId{1, 3000, 0, 0}, BarResourceId{1, 9000, 0, 1}});
  auto archive = BarArchive::Parse(blob);

  const BarEntry* found = archive.Find(1, 3000);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(archive.Extract(*found), res0);
}

TEST(Bar, FindReturnsNullForAnUnlistedTypeIdPair) {
  auto blob = BuildBar({{1, 2, 3}}, {BarResourceId{1, 4000, 0, 0}});
  auto archive = BarArchive::Parse(blob);

  EXPECT_EQ(archive.Find(1, 9999), nullptr);
  EXPECT_EQ(archive.Find(6, 4000), nullptr) << "type must match too, not just the id";
}

TEST(Bar, FindFallsThroughToASequentialEntryPastTheDeclaredRecord) {
  // Confirmed real algorithm, not a guess (see bar.h's own doc
  // comment): a directory record marks only the first id of a
  // contiguous run; ids after it resolve to sequential entries by
  // simple offset. Real evidence: Heavy Weapon's own real code
  // requests ids 54 and 158 past its one real directory record, and
  // both land exactly on real PNG files at those computed entries.
  std::vector<uint8_t> res0 = {'a'};
  std::vector<uint8_t> res1 = {'b', 'b'};
  std::vector<uint8_t> res2 = {'c', 'c', 'c'};
  auto blob = BuildBar({res0, res1, res2}, {BarResourceId{20480, 9001, 0, 0}});
  auto archive = BarArchive::Parse(blob);

  const BarEntry* found = archive.Find(20480, 9002);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(archive.Extract(*found), res1);
}

TEST(Bar, FindFallThroughIsBoundedByTheNextDeclaredRecordOfTheSameType) {
  // Record A (id 100 -> entry 0) reserves entries 0-4 as its own
  // fallback run (5 entries' worth of id gap before record B starts at
  // id 200 -> entry 5). id 104 (offset 4) is the last one still inside
  // that run; id 105 (offset 5) would land exactly on record B's own
  // entry and must not silently resolve to it.
  std::vector<std::vector<uint8_t>> resources(6);
  for (int i = 0; i < 6; ++i) resources[i] = {static_cast<uint8_t>('a' + i)};
  auto blob = BuildBar(resources, {BarResourceId{1, 100, 0, 0}, BarResourceId{1, 200, 0, 5}});
  auto archive = BarArchive::Parse(blob);

  const BarEntry* last_in_run = archive.Find(1, 104);
  ASSERT_NE(last_in_run, nullptr);
  EXPECT_EQ(archive.Extract(*last_in_run), resources[4]);

  EXPECT_EQ(archive.Find(1, 105), nullptr) << "must not overrun into record B's own entry";
}

TEST(Bar, FindFallThroughPicksTheNearestPrecedingRecordOfTheSameType) {
  std::vector<uint8_t> res0 = {'a'};
  std::vector<uint8_t> res1 = {'b'};
  std::vector<uint8_t> res2 = {'c'};
  std::vector<uint8_t> res3 = {'d'};
  auto blob = BuildBar(
      {res0, res1, res2, res3},
      {BarResourceId{1, 100, 0, 0}, BarResourceId{1, 200, 0, 2}});
  auto archive = BarArchive::Parse(blob);

  // id 201 is past record 200 (the nearer one), not record 100.
  const BarEntry* found = archive.Find(1, 201);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(archive.Extract(*found), res3);
}

TEST(Bar, FindReturnsNullWhenIdPrecedesEveryDeclaredRecordOfThatType) {
  auto blob = BuildBar({{1, 2, 3}}, {BarResourceId{1, 4000, 0, 0}});
  auto archive = BarArchive::Parse(blob);

  EXPECT_EQ(archive.Find(1, 3999), nullptr);
}

TEST(Bar, OutOfBoundsEntryIndexInDirectoryIsRejected) {
  auto blob = BuildBar({{1, 2, 3}}, {BarResourceId{1, 4000, 0, 5}});  // only entry 0 exists
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}
