#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zeebulator {

struct GgzEntry {
  std::string name;
  uint32_t offset;
  uint32_t decompressed_size;
};

// Reads a GGZ asset archive. Reverse-engineered format (no public spec
// exists — see TASKS.md Phase 2 for the derivation): the file opens with
// a table of N big-endian (offset:u32, decompressed_size:u32) 8-byte
// entries; the table's own length in bytes equals the first entry's
// offset value, so N = that value / 8. Each entry points to a standard
// RFC 1952 gzip stream elsewhere in the file, whose header FNAME field
// (when present) carries the asset's original filename.
class GgzArchive {
 public:
  // Takes ownership of `data`. Throws std::runtime_error on a malformed
  // archive (does not decompress anything yet — that's Extract()).
  static GgzArchive Parse(std::vector<uint8_t> data);

  const std::vector<GgzEntry>& Entries() const { return entries_; }

  // Decompresses one entry. Throws std::runtime_error on a zlib error or
  // if the decompressed size doesn't match the entry's declared size.
  std::vector<uint8_t> Extract(const GgzEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<GgzEntry> entries_;
};

}  // namespace zeebulator
