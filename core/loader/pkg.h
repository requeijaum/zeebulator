#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zeebulator {

struct PkgEntry {
  std::string name;
  uint32_t hash = 0;
  uint32_t offset = 0;
  uint32_t compressed_size = 0;
  uint32_t decompressed_size = 0;
};

// Reads a real Zeebo/BREW ".pkg" asset archive -- the classic-arcade-port
// container format (Super BurgerTime, TASKS.md Phase 8). No public spec
// exists; reverse-engineered directly from `supbtime.pkg`'s own real
// bytes (PHASE8_LOG.md has the full derivation) after the earlier "naive
// Quake PAK" hypothesis was confirmed wrong.
//
// Layout, confirmed against the one real sample examined so far:
//
//   offset 0:   magic "PACK" (4 bytes)
//   offset 4:   uint32 LE entry count
//   offset 8:   uint32 LE absolute file offset of the trailing filename
//               table (see below)
//   offset 12:  256 bytes, all zero in the one real file seen -- purpose
//               unconfirmed (reserved header room?); preserved as fixed
//               padding, not parsed
//   offset 268: `entry count` fixed 20-byte directory records, back to
//               back:
//                 uint32 LE unknown constant (0x00020000 in every real
//                         record seen so far -- possibly a fixed load-
//                         buffer size; not needed to extract correctly,
//                         so not exposed)
//                 uint32 LE hash (algorithm not identified; not needed,
//                         since real names come from the filename table
//                         below, in the same order)
//                 uint32 LE compressed size
//                 uint32 LE decompressed size
//                 uint32 LE absolute file offset of this entry's own
//                         data, a raw RFC 1950 zlib stream (*not* a
//                         gzip stream with its own header/filename --
//                         unlike GgzArchive's convention)
//   at the footer offset: one more raw zlib stream, decompressing to
//               `entry count` fixed 256-byte, null-padded ASCII
//               filenames, in the same order as the directory table --
//               confirmed by real, legible names ("gc05.bin", "gk03",
//               "mae00.bin", ...) matching the declared entry count
//               exactly.
class PkgArchive {
 public:
  // Takes ownership of `data`. Throws std::runtime_error on a malformed
  // archive. Decompresses the (small) trailing filename table
  // immediately, to populate each entry's real name; per-entry asset
  // data is left compressed until Extract() is called, matching
  // GgzArchive's convention.
  static PkgArchive Parse(std::vector<uint8_t> data);

  const std::vector<PkgEntry>& Entries() const { return entries_; }

  // Decompresses one entry. Throws std::runtime_error on a zlib error or
  // if the decompressed size doesn't match the entry's declared size.
  std::vector<uint8_t> Extract(const PkgEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<PkgEntry> entries_;
};

}  // namespace zeebulator
