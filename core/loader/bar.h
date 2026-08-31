#pragma once

#include <cstdint>
#include <vector>

namespace zeebulator {

struct BarEntry {
  uint32_t offset = 0;
  uint32_t size = 0;
};

// One real resource-ID directory record -- see BarArchive's own doc
// comment below for how this was confirmed.
struct BarResourceId {
  uint16_t type = 0;
  uint16_t requested_id = 0;
  uint16_t unknown = 0;
  uint16_t entry_index = 0;
};

// Reads a real Zeebo/PopCap ".bar" resource archive (`resources.bar`,
// Phase 2's originally-deferred "BAR file parser" task -- deferred
// because Double Dragon's own dump doesn't have one; Peggle's does).
// No public spec exists; reverse-engineered directly from Peggle's own
// real `resources.bar` (PHASE8_LOG.md has the full derivation). Real
// code opens this file through the generic `ISHELL_LoadResDataEx`
// BREW API rather than any custom `.mod`-embedded parser, so -- unlike
// GGZ or `.pkg` -- there is no real ARM code to trace showing how it's
// walked; every field below was derived and cross-checked purely from
// the raw bytes (self-consistent header arithmetic, and, most
// convincingly, real embedded `RIFF`/`ID3`/PNG file signatures landing
// exactly on every offset this parser computes independently).
//
// Layout, confirmed against the one real sample examined so far:
//
//   offset 0: uint32 LE, meaning unconfirmed
//   offset 4: uint32 LE, meaning unconfirmed
//   offset 8: uint32 LE -- a first sub-table's start offset (real
//             value 32, i.e. immediately after this 32-byte header;
//             not otherwise used by this parser, since that sub-
//             table's own contents are a separate, deferred question --
//             see below)
//   offset 12: uint32 LE -- that sub-table's byte length
//   offset 16: uint32 LE -- (offset 8 + offset 12): the *real* offset
//              table's start (see below) -- confirmed by direct
//              observation, not just arithmetic
//   offset 20: uint32 LE -- the real offset table's entry count
//   offset 24: uint32 LE -- where real resource data begins (equals
//              the real offset table's own first entry; kept only as
//              a redundant cross-check during parsing, not otherwise
//              needed)
//   offset 28: uint32 LE -- total size of the resource-data region
//              (file size minus offset 24; also a redundant check)
//
// At offset 32: a first sub-table (`offset 12` bytes long, real length
// 496 in the Peggle sample), now solved -- **this is the real
// resource-ID directory `ISHELL_LoadResDataEx(pIShell, pszResFile,
// wResID, resType, buffer, pnLen)` needs**: `(offset 12) / 8` real,
// straight 8-byte records from the very start of this sub-table (no
// sub-header at all -- see below), each `{uint16 type; uint16
// requested_id; uint16 unknown; uint16 entry_index}`. One record
// confirmed exactly against a real, live `peggle.mod` call site
// (traced via a full instruction trace, TASKS.md Phase 8): real code
// calls `ISHELL_LoadResDataEx(shell, "resources.bar", id=4000, type=1,
// ...)`, and this directory's own one `type=1` record reads `{1, 4000,
// 3, 304}` -- entry **304** decodes (independently, before this
// directory was even understood) to a real, legible localized UI
// string (`"English^Español^Portugu..."`), an exact, non-coincidental
// match. Most of the rest (`type=6`, `requested_id` 1000-6000,
// `entry_index` 58-249) are presumed image-resource lookups given the
// image-heavy makeup of this archive's own entries, though not
// individually cross-checked the same way. The third field's meaning
// remains unconfirmed (preserved, not interpreted).
//
// This sub-table's own first 16 bytes were originally believed to be
// an unconfirmed, skipped sub-header -- wrong, found bringing up a
// second real title (Alien Breaker Deluxe, TASKS.md): those bytes
// decode as two more real, sensible 8-byte records in *both* real
// samples this project has, not two coincidental false positives.
// Alien Breaker Deluxe's own real code requests a resource by an ID
// that only resolves once this fix is in (previously silently
// discarded as "header" bytes); Peggle's own first two "header" bytes
// decode to `id=3000->entry 0` (a real MP3 file) and `id=9000->entry
// 46` (a real 32768-byte raw texture block, matching this same file's
// own already-documented texture-block shape one paragraph below) --
// real, meaningful resources, not padding. The 16-byte skip was
// silently discarding real directory entries in every title parsed so
// far, not just the one that happened to surface it.
//
// At the real offset table (`offset 16`): `offset 20` consecutive
// uint32 LE *absolute file offsets*, strictly increasing, one per real
// resource, immediately followed by one more uint32 LE sentinel value
// equal to the total file size -- confirmed directly against this
// project's own real sample. Entry `i`'s size is `offsets[i+1] -
// offsets[i]` (the sentinel providing the last entry's own bound).
//
// Real resource content, verified by inspection (not decoded further
// by this class -- that's separate, per-format follow-on work, the
// same layering GgzArchive/PkgArchive already use for their own
// contained `.obm1`/`.wav`/zlib-stream payloads): most entries are
// standard files stored with **no container-level compression** --
// raw `RIFF`/WAVE audio, raw `ID3`-tagged MP3, and (for images) a real
// PNG file preceded by a tiny `[uint16 LE header length incl. itself]
// [null-terminated ASCII MIME string, e.g. "image/png"]` wrapper this
// class does not strip. A minority of entries are fixed-size (32768
// bytes in every case but the last of a run) raw blocks with no
// recognizable magic at all (very likely a proprietary uncompressed
// texture/tile format) and a handful of trailing entries are real,
// legible `^`-delimited UI/localization strings -- none of these are
// interpreted further here either.
class BarArchive {
 public:
  // Takes ownership of `data`. Throws std::runtime_error on a malformed
  // archive, including if the header's own redundant cross-check
  // fields (offset table start/size, data start/size) don't agree with
  // each other or with the file's actual size.
  static BarArchive Parse(std::vector<uint8_t> data);

  const std::vector<BarEntry>& Entries() const { return entries_; }
  const std::vector<BarResourceId>& ResourceIds() const { return resource_ids_; }

  // Returns entry's raw bytes, verbatim -- no decompression (there is
  // none at this layer) and no MIME-wrapper stripping.
  std::vector<uint8_t> Extract(const BarEntry& entry) const;

  // Real `ISHELL_LoadResDataEx`-shaped lookup: finds the entry a real
  // `(type, id)` request resolves to.
  //
  // Confirmed real algorithm (not a guess): a directory record marks
  // only the *first* id of a contiguous run; every id from there up to
  // (but not including) the next declared record of the same type
  // resolves to sequential entries by simple offset from the declared
  // one. Every title this project has probed has a real resource-ID
  // directory covering only a small fraction of its own archive's real
  // entries (e.g. Heavy Weapon: 1 of 366) -- originally read as
  // incomplete source dumps, until this cross-checked exactly: the
  // header's own redundant record-count field (bar.h's own doc comment,
  // "offset 4") always matches the directory's real size exactly, so
  // the sparse directory is authored that way on purpose, not
  // truncated. Confirmed directly, not inferred: Heavy Weapon's own
  // real code requests ids 9055 and 9159 (type 20480), 54 and 158 past
  // its one real directory record for that type (`id=9001 -> entry 0`)
  // -- entries 54 and 158 both start with the real PNG magic
  // (`\x89PNG\r\n\x1a\n`), landing exactly where this formula predicts,
  // not a coincidence. Real, possible outcome: nullptr if `id` precedes
  // every declared record of that type, or if the computed offset would
  // run into a different declared run's own entries.
  const BarEntry* Find(uint16_t type, uint16_t id) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<BarEntry> entries_;
  std::vector<BarResourceId> resource_ids_;
};

}  // namespace zeebulator
