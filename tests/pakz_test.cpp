// Tests use synthetic, self-generated PAKZ archives (built here with
// liblzma's own real `lzma_alone_encoder`, the exact same real container
// format the parser expects) rather than any real game's asset file --
// the actual format was reverse-engineered against a real Zeebo Sports
// Tênis dump (`resources.pakz`), but that file is copyrighted and never
// committed to this repo (see CONTRIBUTING.md's clean-room policy).

#include "core/loader/pakz.h"

#include <lzma.h>

#include <cstring>
#include <stdexcept>

#include <gtest/gtest.h>

using zeebulator::PakzArchive;
using zeebulator::PakzEntry;

namespace {

std::vector<uint8_t> EncodeLzmaAlone(const std::vector<uint8_t>& payload) {
  lzma_options_lzma options;
  if (lzma_lzma_preset(&options, LZMA_PRESET_DEFAULT)) {
    throw std::runtime_error("lzma_lzma_preset failed");
  }
  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_alone_encoder(&strm, &options) != LZMA_OK) {
    throw std::runtime_error("lzma_alone_encoder init failed");
  }
  strm.next_in = payload.data();
  strm.avail_in = payload.size();
  std::vector<uint8_t> out(payload.size() + 4096);
  strm.next_out = out.data();
  strm.avail_out = out.size();
  lzma_ret ret = lzma_code(&strm, LZMA_FINISH);
  if (ret != LZMA_STREAM_END) {
    lzma_end(&strm);
    throw std::runtime_error("lzma_code encode failed");
  }
  out.resize(out.size() - strm.avail_out);
  lzma_end(&strm);
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

// Builds a well-formed synthetic PAKZ: real 12-byte header, real
// back-to-back per-entry LZMA_ALONE streams, and a real trailing 64-
// byte-per-entry table -- matching every structural element confirmed
// against the real file (core/loader/pakz.h has the full layout
// writeup).
std::vector<uint8_t> BuildPakz(const std::vector<SyntheticFile>& files) {
  std::vector<std::vector<uint8_t>> streams;
  for (const auto& f : files) streams.push_back(EncodeLzmaAlone(f.payload));

  constexpr uint32_t kHeaderSize = 12;
  constexpr uint32_t kRecordSize = 64;
  constexpr uint32_t kNameFieldSize = 40;

  uint32_t cursor = kHeaderSize;
  std::vector<uint32_t> offsets;
  for (const auto& s : streams) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(s.size());
  }
  uint32_t table_offset = cursor;
  uint32_t table_size = static_cast<uint32_t>(files.size()) * kRecordSize;

  std::vector<uint8_t> out;
  out.push_back('P');
  out.push_back('A');
  out.push_back('C');
  out.push_back('K');
  AppendU32LE(out, table_offset);
  AppendU32LE(out, table_size);
  for (const auto& s : streams) out.insert(out.end(), s.begin(), s.end());

  for (size_t i = 0; i < files.size(); ++i) {
    std::vector<uint8_t> record(kRecordSize, 0);
    std::memcpy(record.data(), files[i].name.data(),
                std::min(files[i].name.size(), static_cast<size_t>(kNameFieldSize)));
    // bytes 40-55: 4 unconfirmed uint32 fields, left zero (see
    // core/loader/pakz.h's own doc comment).
    record[56] = static_cast<uint8_t>(offsets[i]);
    record[57] = static_cast<uint8_t>(offsets[i] >> 8);
    record[58] = static_cast<uint8_t>(offsets[i] >> 16);
    record[59] = static_cast<uint8_t>(offsets[i] >> 24);
    uint32_t csize = static_cast<uint32_t>(streams[i].size());
    record[60] = static_cast<uint8_t>(csize);
    record[61] = static_cast<uint8_t>(csize >> 8);
    record[62] = static_cast<uint8_t>(csize >> 16);
    record[63] = static_cast<uint8_t>(csize >> 24);
    out.insert(out.end(), record.begin(), record.end());
  }
  return out;
}

}  // namespace

TEST(Pakz, ParsesTwoNamedEntriesAndExtractsCorrectData) {
  std::vector<uint8_t> payload1 = {1, 2, 3, 4, 5};
  std::vector<uint8_t> payload2(2000, 0xAB);  // large enough to actually compress

  auto blob = BuildPakz({{"athlete_types.m3g", payload1}, {"audio/0_.wav", payload2}});
  auto archive = PakzArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 2u);
  EXPECT_EQ(archive.Entries()[0].name, "athlete_types.m3g");
  EXPECT_EQ(archive.Entries()[1].name, "audio/0_.wav");

  EXPECT_EQ(archive.Extract(archive.Entries()[0]), payload1);
  EXPECT_EQ(archive.Extract(archive.Entries()[1]), payload2);
}

TEST(Pakz, EmptyPayloadRoundTrips) {
  auto blob = BuildPakz({{"empty.bin", {}}});
  auto archive = PakzArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 1u);
  EXPECT_TRUE(archive.Extract(archive.Entries()[0]).empty());
}

TEST(Pakz, LargePayloadRoundTripsPastTheInitialOutputBufferGuess) {
  // Extract()'s own growing-buffer loop starts at compressed_size*8+4096
  // -- a real, highly-compressible payload can exceed that many times
  // over (this one aims for ~50x), exercising the resize/retry path.
  std::vector<uint8_t> payload(2'000'000, 0x00);
  auto blob = BuildPakz({{"big.bin", payload}});
  auto archive = PakzArchive::Parse(blob);
  EXPECT_EQ(archive.Extract(archive.Entries()[0]), payload);
}

TEST(Pakz, TooSmallFileIsRejected) {
  std::vector<uint8_t> tiny = {'P', 'A', 'C', 'K', 0, 0};
  EXPECT_THROW(PakzArchive::Parse(tiny), std::runtime_error);
}

TEST(Pakz, BadMagicIsRejected) {
  auto blob = BuildPakz({{"x.bin", {1, 2, 3}}});
  blob[0] = 'X';
  EXPECT_THROW(PakzArchive::Parse(blob), std::runtime_error);
}

TEST(Pakz, TableSizeNotAMultipleOfRecordSizeIsRejected) {
  auto blob = BuildPakz({{"x.bin", {1, 2, 3}}});
  blob[8] = 1;  // table_size = 1, not a multiple of 64
  blob[9] = 0;
  blob[10] = 0;
  blob[11] = 0;
  EXPECT_THROW(PakzArchive::Parse(blob), std::runtime_error);
}

TEST(Pakz, TableRunningPastEndOfFileIsRejected) {
  auto blob = BuildPakz({{"x.bin", {1, 2, 3}}});
  // 0x40000000 -- a huge but real multiple of 64, so this exercises the
  // bounds check specifically, not the multiple-of-64 check above.
  blob[8] = 0x00;
  blob[9] = 0x00;
  blob[10] = 0x00;
  blob[11] = 0x40;
  EXPECT_THROW(PakzArchive::Parse(blob), std::runtime_error);
}

TEST(Pakz, CorruptedEntryStreamIsRejected) {
  auto blob = BuildPakz({{"x.bin", std::vector<uint8_t>(500, 0x42)}});
  // Entry 0's own real LZMA_ALONE stream starts right at byte 12 (the
  // real header size) -- stomp its first byte (the properties byte).
  blob[12] = 0x00;
  auto archive = PakzArchive::Parse(blob);
  EXPECT_THROW(archive.Extract(archive.Entries()[0]), std::runtime_error);
}
