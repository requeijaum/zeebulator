// Tests use synthetic, self-generated GGZ archives (built here with zlib's
// own deflate) rather than any real game's asset file -- the actual
// format was reverse-engineered against a real Double Dragon dump, but
// that file is copyrighted and never committed to this repo (see
// CONTRIBUTING.md's clean-room policy and TASKS.md Phase 2).

#include "core/loader/ggz.h"

#include <zlib.h>

#include <gtest/gtest.h>

#include <stdexcept>

using zeebulator::GgzArchive;

namespace {

std::vector<uint8_t> MakeGzipMember(const std::string& filename,
                                     const std::vector<uint8_t>& payload,
                                     bool set_filename = true) {
  z_stream strm{};
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                    Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("deflateInit2 failed");
  }

  std::vector<char> name_buf(filename.begin(), filename.end());
  name_buf.push_back('\0');
  gz_header header{};
  if (set_filename) {
    header.name = reinterpret_cast<Bytef*>(name_buf.data());
    header.name_max = static_cast<uInt>(name_buf.size());
    deflateSetHeader(&strm, &header);
  }

  std::vector<uint8_t> out(payload.size() + 256);
  strm.next_in = const_cast<Bytef*>(payload.data());
  strm.avail_in = static_cast<uInt>(payload.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  deflate(&strm, Z_FINISH);
  out.resize(out.size() - strm.avail_out);
  deflateEnd(&strm);
  return out;
}

struct SyntheticFile {
  std::string name;
  std::vector<uint8_t> payload;
  bool set_filename = true;
};

std::vector<uint8_t> BuildGgz(const std::vector<SyntheticFile>& files) {
  std::vector<std::vector<uint8_t>> streams;
  for (const auto& f : files) {
    streams.push_back(MakeGzipMember(f.name, f.payload, f.set_filename));
  }

  uint32_t table_len = static_cast<uint32_t>(files.size() * 8);
  std::vector<uint32_t> offsets;
  uint32_t cursor = table_len;
  for (const auto& s : streams) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(s.size());
  }

  std::vector<uint8_t> blob(cursor, 0);
  for (size_t i = 0; i < files.size(); ++i) {
    uint32_t off = offsets[i];
    uint32_t sz = static_cast<uint32_t>(files[i].payload.size());
    size_t p = i * 8;
    blob[p] = static_cast<uint8_t>(off >> 24);
    blob[p + 1] = static_cast<uint8_t>(off >> 16);
    blob[p + 2] = static_cast<uint8_t>(off >> 8);
    blob[p + 3] = static_cast<uint8_t>(off);
    blob[p + 4] = static_cast<uint8_t>(sz >> 24);
    blob[p + 5] = static_cast<uint8_t>(sz >> 16);
    blob[p + 6] = static_cast<uint8_t>(sz >> 8);
    blob[p + 7] = static_cast<uint8_t>(sz);
  }
  for (size_t i = 0; i < streams.size(); ++i) {
    std::copy(streams[i].begin(), streams[i].end(), blob.begin() + offsets[i]);
  }
  return blob;
}

}  // namespace

TEST(Ggz, ParsesTwoNamedEntriesAndExtractsCorrectData) {
  std::vector<uint8_t> payload1 = {1, 2, 3, 4, 5};
  std::vector<uint8_t> payload2(2000, 0xAB);  // large enough to actually compress

  auto blob = BuildGgz({{"hello.bin", payload1}, {"world.bin", payload2}});
  auto archive = GgzArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 2u);
  EXPECT_EQ(archive.Entries()[0].name, "hello.bin");
  EXPECT_EQ(archive.Entries()[1].name, "world.bin");

  EXPECT_EQ(archive.Extract(archive.Entries()[0]), payload1);
  EXPECT_EQ(archive.Extract(archive.Entries()[1]), payload2);
}

TEST(Ggz, EntryWithoutFnameGetsOrdinalFallbackName) {
  std::vector<uint8_t> payload = {9, 9, 9};
  auto blob = BuildGgz({{"ignored.bin", payload, /*set_filename=*/false}});
  auto archive = GgzArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 1u);
  EXPECT_EQ(archive.Entries()[0].name, "unnamed_0");
  EXPECT_EQ(archive.Extract(archive.Entries()[0]), payload);
}

TEST(Ggz, EmptyPayloadRoundTrips) {
  auto blob = BuildGgz({{"empty.bin", {}}});
  auto archive = GgzArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 1u);
  EXPECT_TRUE(archive.Extract(archive.Entries()[0]).empty());
}

TEST(Ggz, TooSmallFileIsRejected) {
  std::vector<uint8_t> tiny = {1, 2, 3};
  EXPECT_THROW(GgzArchive::Parse(tiny), std::runtime_error);
}

TEST(Ggz, InvalidTableLengthIsRejected) {
  // First word (table length) claims a value that isn't a multiple of 8.
  std::vector<uint8_t> bad = {0, 0, 0, 5, 0, 0, 0, 0};
  EXPECT_THROW(GgzArchive::Parse(bad), std::runtime_error);
}

TEST(Ggz, CorruptedGzipMagicIsRejected) {
  auto blob = BuildGgz({{"x.bin", {1, 2, 3}}});
  blob[8] = 0x00;  // stomp the gzip magic at the first entry's offset
  EXPECT_THROW(GgzArchive::Parse(blob), std::runtime_error);
}
