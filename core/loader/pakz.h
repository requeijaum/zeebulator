#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zeebulator {

struct PakzEntry {
  std::string name;
  uint32_t offset = 0;
  uint32_t compressed_size = 0;
};

// Reads a real Zeebo `resources.pakz` asset archive -- shared by (at
// least) Zeebo Sports Tênis/Peteca/Volei, Zeeboids, and one kids'-
// activity title's own downloads, all independently confirmed to use
// this exact byte layout. No public spec exists; reverse-engineered
// directly from Zeebo Sports Tênis's own real `resources.pakz`.
//
// Layout, confirmed against the one real sample examined so far:
//
//   offset 0:  magic "PACK" (4 bytes)
//   offset 4:  uint32 LE absolute file offset of the trailing entry
//              table (see below)
//   offset 8:  uint32 LE byte size of that table (always a multiple of
//              64 -- one real sample had exactly 298 entries, 19072
//              bytes)
//   offset 12: real per-entry compressed data begins here, back to
//              back, each entry's own byte range given directly by its
//              table record below (no gaps, no padding between entries
//              -- confirmed by summing every real entry's own declared
//              size and getting exactly `table offset - 12`)
//   at the table offset: `table size / 64` fixed 64-byte records, back
//              to back:
//                bytes 0-39:  real filename, null-padded ASCII
//                bytes 40-55: 4 unrelated uint32 fields, unconfirmed
//                             purpose, always zero in the one real
//                             sample examined -- not exposed
//                bytes 56-59: uint32 LE absolute file offset of this
//                             entry's own compressed data
//                bytes 60-63: uint32 LE compressed byte size
//
// Each entry's own compressed bytes are a real, standard classic
// ".lzma"/"LZMA_ALONE"-format stream (5-byte properties/dictionary-size
// header + 8-byte uncompressed size + payload) -- confirmed directly:
// the first real entry examined (`athlete_types.m3g`) decompresses
// cleanly via liblzma's own `lzma_alone_decoder` to real, legible
// content starting with a real embedded string, "...Crazyball Engine.
// Copyright...". Not a proprietary or per-title scheme -- the same
// standard algorithm the classic command-line `lzma`/`unlzma` tools
// produce, just packed many-to-a-file instead of one-file-per-stream.
class PakzArchive {
 public:
  // Takes ownership of `data`. Throws std::runtime_error on a malformed
  // archive (bad magic, a table offset/size that doesn't fit the real
  // file, or a table size that isn't a multiple of the real 64-byte
  // record size).
  static PakzArchive Parse(std::vector<uint8_t> data);

  const std::vector<PakzEntry>& Entries() const { return entries_; }

  // Decompresses one entry via liblzma's real `lzma_alone_decoder`.
  // Throws std::runtime_error on a real liblzma error (a genuinely
  // malformed/truncated stream, not this project's own guesswork --
  // liblzma is a real, independent, standard implementation).
  std::vector<uint8_t> Extract(const PakzEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<PakzEntry> entries_;
};

}  // namespace zeebulator
