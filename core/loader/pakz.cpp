#include "core/loader/pakz.h"

#include <lzma.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace zeebulator {

namespace {

constexpr uint32_t kHeaderSize = 12;
constexpr uint32_t kRecordSize = 64;
constexpr uint32_t kNameFieldSize = 40;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

PakzArchive PakzArchive::Parse(std::vector<uint8_t> data) {
  PakzArchive archive;
  archive.data_ = std::move(data);
  const uint8_t* d = archive.data_.data();
  size_t size = archive.data_.size();

  if (size < kHeaderSize) throw std::runtime_error("PAKZ: file too small for a header");
  if (d[0] != 'P' || d[1] != 'A' || d[2] != 'C' || d[3] != 'K') {
    throw std::runtime_error("PAKZ: bad magic");
  }

  uint32_t table_offset = ReadU32LE(d + 4);
  uint32_t table_size = ReadU32LE(d + 8);
  if (table_size % kRecordSize != 0) {
    throw std::runtime_error("PAKZ: table size isn't a multiple of the real record size");
  }
  uint64_t table_end = static_cast<uint64_t>(table_offset) + table_size;
  if (table_end > size) throw std::runtime_error("PAKZ: entry table runs past end of file");

  uint32_t entry_count = table_size / kRecordSize;
  archive.entries_.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    const uint8_t* rec = d + table_offset + static_cast<uint64_t>(i) * kRecordSize;
    PakzEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(rec),
                       strnlen(reinterpret_cast<const char*>(rec), kNameFieldSize));
    // rec[40..55] (4 uint32 fields) are unconfirmed, always zero in the
    // one real sample examined -- see this class's own doc comment.
    entry.offset = ReadU32LE(rec + 56);
    entry.compressed_size = ReadU32LE(rec + 60);
    if (static_cast<uint64_t>(entry.offset) + entry.compressed_size > size) {
      throw std::runtime_error("PAKZ: entry data runs past end of file");
    }
    archive.entries_.push_back(std::move(entry));
  }

  return archive;
}

std::vector<uint8_t> PakzArchive::Extract(const PakzEntry& entry) const {
  if (entry.compressed_size == 0) return {};

  lzma_stream strm = LZMA_STREAM_INIT;
  lzma_ret init_ret = lzma_alone_decoder(&strm, UINT64_MAX);
  if (init_ret != LZMA_OK) {
    throw std::runtime_error("PAKZ: lzma_alone_decoder init failed for " + entry.name);
  }

  strm.next_in = data_.data() + entry.offset;
  strm.avail_in = entry.compressed_size;

  // The real LZMA_ALONE header embeds the true decompressed size, but
  // reading it ourselves ahead of the real liblzma call would just be
  // duplicating logic liblzma already does correctly -- grow instead,
  // starting from a generous real-world guess (every real entry seen so
  // far decompresses well under this).
  std::vector<uint8_t> out(entry.compressed_size * 8 + 4096);
  size_t produced = 0;
  lzma_ret ret = LZMA_OK;
  while (true) {
    strm.next_out = out.data() + produced;
    strm.avail_out = out.size() - produced;
    ret = lzma_code(&strm, LZMA_FINISH);
    produced = out.size() - strm.avail_out;
    if (ret == LZMA_STREAM_END) break;
    if (ret != LZMA_OK) {
      lzma_end(&strm);
      throw std::runtime_error("PAKZ: lzma_code failed for " + entry.name);
    }
    if (strm.avail_out == 0) out.resize(out.size() * 2);
  }
  lzma_end(&strm);
  out.resize(produced);
  return out;
}

}  // namespace zeebulator
