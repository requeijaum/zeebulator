#include "core/loader/ggz.h"

#include <zlib.h>

#include <stdexcept>
#include <utility>

namespace zeebulator {

namespace {

uint32_t ReadU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Parses just enough of a gzip member's header (RFC 1952 section 2.3) to
// pull out the original filename, if the FNAME flag is set. Returns an
// empty string if it isn't — callers fall back to an ordinal name, per
// ggzbrewtools' documented observation that not every entry has FNAME set.
std::string ReadGzipFilename(const uint8_t* data, size_t size, uint32_t offset) {
  if (static_cast<uint64_t>(offset) + 10 > size) {
    throw std::runtime_error("GGZ: gzip header out of bounds");
  }
  if (data[offset] != 0x1F || data[offset + 1] != 0x8B) {
    throw std::runtime_error("GGZ: entry does not point to a gzip stream");
  }
  uint8_t flg = data[offset + 3];
  size_t pos = offset + 10;

  if (flg & 0x04) {  // FEXTRA
    if (pos + 2 > size) throw std::runtime_error("GGZ: truncated gzip FEXTRA field");
    uint16_t xlen = static_cast<uint16_t>(data[pos]) |
                     (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2 + xlen;
  }

  if (!(flg & 0x08)) return {};  // FNAME not set

  size_t start = pos;
  while (pos < size && data[pos] != 0) ++pos;
  if (pos >= size) throw std::runtime_error("GGZ: unterminated gzip FNAME");
  return std::string(reinterpret_cast<const char*>(data + start), pos - start);
}

}  // namespace

GgzArchive GgzArchive::Parse(std::vector<uint8_t> data) {
  GgzArchive archive;
  archive.data_ = std::move(data);
  const uint8_t* d = archive.data_.data();
  size_t size = archive.data_.size();

  if (size < 8) throw std::runtime_error("GGZ: file too small for a table header");

  uint32_t table_length = ReadU32BE(d);
  if (table_length == 0 || table_length % 8 != 0 || table_length > size) {
    throw std::runtime_error("GGZ: invalid table length");
  }

  uint32_t num_entries = table_length / 8;
  archive.entries_.reserve(num_entries);
  for (uint32_t i = 0; i < num_entries; ++i) {
    uint32_t offset = ReadU32BE(d + i * 8);
    uint32_t decompressed_size = ReadU32BE(d + i * 8 + 4);
    if (offset >= size) throw std::runtime_error("GGZ: entry offset out of bounds");

    std::string name = ReadGzipFilename(d, size, offset);
    if (name.empty()) name = "unnamed_" + std::to_string(i);

    archive.entries_.push_back(GgzEntry{std::move(name), offset, decompressed_size});
  }
  return archive;
}

std::vector<uint8_t> GgzArchive::Extract(const GgzEntry& entry) const {
  if (entry.decompressed_size == 0) return {};

  std::vector<uint8_t> out(entry.decompressed_size);

  z_stream strm{};
  // windowBits = 15 + 16 tells zlib to decode gzip framing specifically
  // (rather than raw deflate or zlib-wrapped deflate).
  if (inflateInit2(&strm, 15 + 16) != Z_OK) {
    throw std::runtime_error("GGZ: inflateInit2 failed");
  }
  strm.next_in = const_cast<Bytef*>(data_.data() + entry.offset);
  strm.avail_in = static_cast<uInt>(data_.size() - entry.offset);
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());

  int ret = inflate(&strm, Z_FINISH);
  uInt produced = static_cast<uInt>(out.size()) - strm.avail_out;
  inflateEnd(&strm);

  if (ret != Z_STREAM_END || produced != entry.decompressed_size) {
    throw std::runtime_error("GGZ: decompression failed or size mismatch for '" +
                              entry.name + "'");
  }
  return out;
}

}  // namespace zeebulator
