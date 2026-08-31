#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <unordered_map>

namespace zeebulator {

// Flat 32-bit address space backed by on-demand-allocated pages, so we
// don't pay for the full 4GB range up front. All accesses are
// little-endian, matching the ARM1136J-S as configured in the Zeebo
// (ARCHITECTURE.md 3.2).
class Memory {
 public:
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint32_t kPageMask = kPageSize - 1;

  uint8_t Read8(uint32_t address) const;
  uint16_t Read16(uint32_t address) const;
  uint32_t Read32(uint32_t address) const;

  void Write8(uint32_t address, uint8_t value);
  void Write16(uint32_t address, uint16_t value);
  void Write32(uint32_t address, uint32_t value);

  // Copies `size` bytes from `data` into the address space starting at
  // `address`. Used by the loader to map code/data segments in.
  void Load(uint32_t address, const uint8_t* data, size_t size);

  // Save-state support (TASKS_TOOLING.md Phase B, stage 1): writes only
  // the pages actually allocated so far (page index + full 4KB
  // contents each), not the whole sparse 4GB address space. Format is
  // this class's own private concern -- callers just get a bool back
  // (false on any stream failure) and treat the written/read range as
  // opaque. `Deserialize` replaces the current contents entirely
  // (existing pages not present in the stream are dropped), so it's
  // meant for loading into a memory instance that's about to become
  // "the" resumed state, not merging into a live one.
  bool Serialize(std::ostream& out) const;
  bool Deserialize(std::istream& in);

 private:
  using Page = std::array<uint8_t, kPageSize>;

  Page& MutablePage(uint32_t page_index);
  const Page* FindPage(uint32_t page_index) const;

  std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
};

}  // namespace zeebulator
