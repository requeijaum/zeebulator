#include "core/loader/pkg.h"

#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace zeebulator {

namespace {

constexpr uint32_t kHeaderSize = 12;
constexpr uint32_t kReservedSize = 256;
constexpr uint32_t kDirectoryStart = kHeaderSize + kReservedSize;
constexpr uint32_t kRecordSize = 20;
constexpr uint32_t kNameRecordSize = 256;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Raw RFC 1950 zlib inflate (unlike GgzArchive's gzip-framed inflate) --
// `expected_size` is known up front for every real use in this file (a
// directory-declared decompressed size, or entry_count * kNameRecordSize
// for the filename table), so this can allocate exactly once rather than
// growing.
std::vector<uint8_t> InflateZlib(const uint8_t* data, size_t size, size_t expected_size,
                                  const char* what) {
  std::vector<uint8_t> out(expected_size);
  z_stream strm{};
  if (inflateInit2(&strm, 15) != Z_OK) {
    throw std::runtime_error(std::string("PKG: inflateInit2 failed for ") + what);
  }
  strm.next_in = const_cast<Bytef*>(data);
  strm.avail_in = static_cast<uInt>(size);
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());

  int ret = inflate(&strm, Z_FINISH);
  uInt produced = static_cast<uInt>(out.size()) - strm.avail_out;
  inflateEnd(&strm);

  if (ret != Z_STREAM_END || produced != expected_size) {
    throw std::runtime_error(std::string("PKG: decompression failed or size mismatch for ") +
                              what);
  }
  return out;
}

}  // namespace

PkgArchive PkgArchive::Parse(std::vector<uint8_t> data) {
  PkgArchive archive;
  archive.data_ = std::move(data);
  const uint8_t* d = archive.data_.data();
  size_t size = archive.data_.size();

  if (size < kDirectoryStart) throw std::runtime_error("PKG: file too small for a header");
  if (d[0] != 'P' || d[1] != 'A' || d[2] != 'C' || d[3] != 'K') {
    throw std::runtime_error("PKG: bad magic");
  }

  uint32_t entry_count = ReadU32LE(d + 4);
  uint32_t footer_offset = ReadU32LE(d + 8);
  if (footer_offset >= size) throw std::runtime_error("PKG: footer offset out of bounds");

  uint64_t directory_end = static_cast<uint64_t>(kDirectoryStart) +
                            static_cast<uint64_t>(entry_count) * kRecordSize;
  if (directory_end > size) throw std::runtime_error("PKG: directory runs past end of file");

  archive.entries_.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    const uint8_t* rec = d + kDirectoryStart + static_cast<uint64_t>(i) * kRecordSize;
    PkgEntry entry;
    // rec[0..3] is a per-record constant (0x00020000 in every real record
    // seen so far) -- not needed for extraction, so not stored.
    entry.hash = ReadU32LE(rec + 4);
    entry.compressed_size = ReadU32LE(rec + 8);
    entry.decompressed_size = ReadU32LE(rec + 12);
    entry.offset = ReadU32LE(rec + 16);
    if (static_cast<uint64_t>(entry.offset) + entry.compressed_size > size) {
      throw std::runtime_error("PKG: entry data runs past end of file");
    }
    archive.entries_.push_back(std::move(entry));
  }

  // The filename table is itself one more zlib stream, decompressing to
  // `entry_count` fixed 256-byte, null-padded ASCII names, in directory
  // order.
  std::vector<uint8_t> names = InflateZlib(d + footer_offset, size - footer_offset,
                                            static_cast<size_t>(entry_count) * kNameRecordSize,
                                            "filename table");
  for (uint32_t i = 0; i < entry_count; ++i) {
    const char* name_bytes = reinterpret_cast<const char*>(names.data()) + i * kNameRecordSize;
    archive.entries_[i].name.assign(name_bytes, strnlen(name_bytes, kNameRecordSize));
  }

  return archive;
}

std::vector<uint8_t> PkgArchive::Extract(const PkgEntry& entry) const {
  if (entry.decompressed_size == 0) return {};
  return InflateZlib(data_.data() + entry.offset, entry.compressed_size,
                      entry.decompressed_size, entry.name.c_str());
}

}  // namespace zeebulator
