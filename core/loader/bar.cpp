#include "core/loader/bar.h"

#include <stdexcept>
#include <utility>

namespace zeebulator {

namespace {

constexpr uint32_t kHeaderSize = 32;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

constexpr uint32_t kResourceIdRecordSize = 8;

}  // namespace

BarArchive BarArchive::Parse(std::vector<uint8_t> data) {
  BarArchive archive;
  archive.data_ = std::move(data);
  const uint8_t* d = archive.data_.data();
  size_t size = archive.data_.size();

  if (size < kHeaderSize) throw std::runtime_error("BAR: file too small for a header");

  uint32_t table1_start = ReadU32LE(d + 8);
  uint32_t table1_size = ReadU32LE(d + 12);
  uint32_t table_start = ReadU32LE(d + 16);
  uint32_t entry_count = ReadU32LE(d + 20);
  uint32_t data_start = ReadU32LE(d + 24);
  uint32_t data_size = ReadU32LE(d + 28);

  if (table1_start != kHeaderSize || table_start != table1_start + table1_size) {
    throw std::runtime_error("BAR: inconsistent header sub-table offsets");
  }

  // entry_count real offsets, plus one trailing sentinel equal to the
  // file's own total size.
  uint64_t table_bytes = (static_cast<uint64_t>(entry_count) + 1) * 4;
  uint64_t table_end = static_cast<uint64_t>(table_start) + table_bytes;
  if (table_end > size) throw std::runtime_error("BAR: offset table runs past end of file");
  if (table_end != data_start) {
    throw std::runtime_error("BAR: offset table doesn't end where the header says data starts");
  }
  if (static_cast<uint64_t>(data_start) + data_size != size) {
    throw std::runtime_error("BAR: header data-size field doesn't match the real file size");
  }

  std::vector<uint32_t> offsets(entry_count + 1);
  for (uint32_t i = 0; i <= entry_count; ++i) {
    offsets[i] = ReadU32LE(d + table_start + i * 4);
    // Monotonically non-decreasing, not strictly increasing: a real,
    // shipped title (Alien Breaker Deluxe's own data.bar) has a
    // genuine zero-length resource entry (two consecutive offsets
    // equal, confirmed live -- not file corruption), which Extract()
    // already handles fine (an empty byte range, begin == end).
    if (i > 0 && offsets[i] < offsets[i - 1]) {
      throw std::runtime_error("BAR: offset table is not monotonically non-decreasing");
    }
  }
  if (offsets[0] != data_start) {
    throw std::runtime_error("BAR: first resource offset doesn't match the header's data start");
  }
  if (offsets[entry_count] != size) {
    throw std::runtime_error("BAR: final offset-table sentinel doesn't match the file size");
  }

  archive.entries_.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    archive.entries_.push_back(BarEntry{offsets[i], offsets[i + 1] - offsets[i]});
  }

  // The resource-ID directory: straight 8-byte records from the very
  // start of table1, no sub-header at all -- see bar.h's own doc
  // comment for how the previous "16-byte sub-header" theory was found
  // to be wrong (those 16 bytes decode as two more real, sensible
  // records in both real samples this project has: Alien Breaker
  // Deluxe's own id=9001->entry 0 and id=9037->entry 30, and Peggle's
  // own id=3000->entry 0 (a real MP3) and id=9000->entry 46 (a real
  // 32768-byte texture block, matching this same file's own already-
  // documented texture-block shape) -- not two coincidental false
  // positives, real evidence the original 16-byte skip was silently
  // discarding real directory entries this whole time.
  if (table1_size % kResourceIdRecordSize != 0) {
    throw std::runtime_error("BAR: resource-ID sub-table size doesn't fit whole records");
  }
  uint32_t resource_id_count = table1_size / kResourceIdRecordSize;
  archive.resource_ids_.reserve(resource_id_count);
  for (uint32_t i = 0; i < resource_id_count; ++i) {
    const uint8_t* rec = d + table1_start + i * kResourceIdRecordSize;
    BarResourceId res_id;
    res_id.type = ReadU16LE(rec + 0);
    res_id.requested_id = ReadU16LE(rec + 2);
    res_id.unknown = ReadU16LE(rec + 4);
    res_id.entry_index = ReadU16LE(rec + 6);
    if (res_id.entry_index >= entry_count) {
      throw std::runtime_error("BAR: resource-ID directory entry_index out of bounds");
    }
    archive.resource_ids_.push_back(res_id);
  }
  return archive;
}

std::vector<uint8_t> BarArchive::Extract(const BarEntry& entry) const {
  return std::vector<uint8_t>(data_.begin() + entry.offset,
                               data_.begin() + entry.offset + entry.size);
}

const BarEntry* BarArchive::Find(uint16_t type, uint16_t id) const {
  // Real directory records mark only the *first* id of a contiguous
  // run; the rest of that run's ids resolve to sequential entries by
  // simple offset -- see bar.h's own doc comment for the confirmed
  // evidence (two real, live-requested ids in Heavy Weapon, 54 and 158
  // entries past the nearest declared record, land exactly on two real
  // PNG files -- not a coincidence, a real algorithm).
  const BarResourceId* best = nullptr;
  for (const BarResourceId& res_id : resource_ids_) {
    if (res_id.type == type && res_id.requested_id <= id) {
      if (best == nullptr || res_id.requested_id > best->requested_id) best = &res_id;
    }
  }
  if (best == nullptr) return nullptr;
  uint32_t offset = id - best->requested_id;
  uint32_t candidate = best->entry_index + offset;
  // Bound the run so it can't wander into a different declared run's own
  // entries, or past the end of the archive.
  uint32_t bound = static_cast<uint32_t>(entries_.size());
  for (const BarResourceId& res_id : resource_ids_) {
    if (res_id.entry_index > best->entry_index && res_id.entry_index < bound) {
      bound = res_id.entry_index;
    }
  }
  if (candidate >= bound) return nullptr;
  return &entries_[candidate];
}

}  // namespace zeebulator
