// Tests use synthetic, self-generated PKG archives (built here with
// zlib's own deflate, raw RFC 1950 streams) rather than any real game's
// asset file -- the actual format was reverse-engineered against a real
// Super BurgerTime dump (`supbtime.pkg`), but that file is copyrighted
// and never committed to this repo (see CONTRIBUTING.md's clean-room
// policy and PHASE8_LOG.md for the format's derivation).

#include "core/loader/pkg.h"

#include <zlib.h>

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>

using zeebulator::PkgArchive;
using zeebulator::PkgEntry;

namespace {

std::vector<uint8_t> DeflateZlib(const std::vector<uint8_t>& payload) {
  z_stream strm{};
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("deflateInit2 failed");
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

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

struct SyntheticFile {
  std::string name;
  std::vector<uint8_t> payload;
};

// Builds a well-formed synthetic PKG: real 12-byte header, real 256-byte
// reserved block, real 20-byte-per-entry directory, real per-entry raw
// zlib streams, and a real trailing zlib-compressed 256-byte-per-name
// filename table -- matching every structural element confirmed against
// the real file (core/loader/pkg.h has the full layout writeup).
std::vector<uint8_t> BuildPkg(const std::vector<SyntheticFile>& files) {
  std::vector<std::vector<uint8_t>> streams;
  for (const auto& f : files) streams.push_back(DeflateZlib(f.payload));

  constexpr uint32_t kDirectoryStart = 12 + 256;
  constexpr uint32_t kRecordSize = 20;
  uint32_t cursor = kDirectoryStart + static_cast<uint32_t>(files.size()) * kRecordSize;
  std::vector<uint32_t> offsets;
  for (const auto& s : streams) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(s.size());
  }
  uint32_t footer_offset = cursor;

  std::vector<uint8_t> out;
  out.push_back('P');
  out.push_back('A');
  out.push_back('C');
  out.push_back('K');
  AppendU32LE(out, static_cast<uint32_t>(files.size()));
  AppendU32LE(out, footer_offset);
  out.resize(kDirectoryStart, 0);  // the real 256-byte reserved block

  for (size_t i = 0; i < files.size(); ++i) {
    AppendU32LE(out, 0x00020000);  // the real, unexplained per-record constant
    AppendU32LE(out, 0xDEADBEEF);  // a hash value (algorithm unidentified, unused by the parser)
    AppendU32LE(out, static_cast<uint32_t>(streams[i].size()));
    AppendU32LE(out, static_cast<uint32_t>(files[i].payload.size()));
    AppendU32LE(out, offsets[i]);
  }
  for (const auto& s : streams) out.insert(out.end(), s.begin(), s.end());

  std::vector<uint8_t> names(files.size() * 256, 0);
  for (size_t i = 0; i < files.size(); ++i) {
    std::memcpy(names.data() + i * 256, files[i].name.data(), files[i].name.size());
  }
  auto compressed_names = DeflateZlib(names);
  out.insert(out.end(), compressed_names.begin(), compressed_names.end());
  return out;
}

}  // namespace

TEST(Pkg, ParsesTwoNamedEntriesAndExtractsCorrectData) {
  std::vector<uint8_t> payload1 = {1, 2, 3, 4, 5};
  std::vector<uint8_t> payload2(2000, 0xAB);  // large enough to actually compress

  auto blob = BuildPkg({{"gc05.bin", payload1}, {"mae00.bin", payload2}});
  auto archive = PkgArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 2u);
  EXPECT_EQ(archive.Entries()[0].name, "gc05.bin");
  EXPECT_EQ(archive.Entries()[1].name, "mae00.bin");

  EXPECT_EQ(archive.Extract(archive.Entries()[0]), payload1);
  EXPECT_EQ(archive.Extract(archive.Entries()[1]), payload2);
}

TEST(Pkg, EmptyPayloadRoundTrips) {
  auto blob = BuildPkg({{"empty.bin", {}}});
  auto archive = PkgArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 1u);
  EXPECT_TRUE(archive.Extract(archive.Entries()[0]).empty());
}

TEST(Pkg, TooSmallFileIsRejected) {
  std::vector<uint8_t> tiny = {'P', 'A', 'C', 'K', 0, 0, 0, 0};
  EXPECT_THROW(PkgArchive::Parse(tiny), std::runtime_error);
}

TEST(Pkg, BadMagicIsRejected) {
  auto blob = BuildPkg({{"x.bin", {1, 2, 3}}});
  blob[0] = 'X';
  EXPECT_THROW(PkgArchive::Parse(blob), std::runtime_error);
}

TEST(Pkg, OutOfBoundsFooterOffsetIsRejected) {
  auto blob = BuildPkg({{"x.bin", {1, 2, 3}}});
  blob[8] = 0xFF;
  blob[9] = 0xFF;
  blob[10] = 0xFF;
  blob[11] = 0xFF;
  EXPECT_THROW(PkgArchive::Parse(blob), std::runtime_error);
}

TEST(Pkg, DirectoryRunningPastEndOfFileIsRejected) {
  auto blob = BuildPkg({{"x.bin", {1, 2, 3}}});
  blob[4] = 100;  // claim 100 entries; the real directory only has room for 1
  EXPECT_THROW(PkgArchive::Parse(blob), std::runtime_error);
}

TEST(Pkg, CorruptedEntryStreamIsRejected) {
  auto blob = BuildPkg({{"x.bin", std::vector<uint8_t>(500, 0x42)}});
  // A single-entry archive's directory is 12 (header) + 256 (reserved) +
  // 20 (one record) = 288 bytes, so entry 0's own real zlib stream
  // starts right there -- stomp its first byte (the zlib CMF byte).
  blob[288] = 0x00;
  auto archive = PkgArchive::Parse(blob);
  EXPECT_THROW(archive.Extract(archive.Entries()[0]), std::runtime_error);
}
