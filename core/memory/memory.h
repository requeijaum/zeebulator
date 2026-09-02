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

  // Phase-1 media-interface binding guard (replaces the old +0x28
  // address-heuristic redirect HACK). Opt-in: pass the guest region
  // MediaHle allocates its interface objects in ([start, end)). Once a
  // game stores a pointer INTO that region (i.e. binds a real,
  // HLE-owned IMedia interface) into some slot, a subsequent zero-write
  // to that same slot (the game's Release helper clearing media_source+8,
  // ddragonz.mod 0x11f424) is SKIPPED so the binding survives into the
  // Play path (0x11d04c reads media_source+8 -> vtable[6] = Play).
  // This mirrors Zeemu's host-owned IMediaPCM vtable model: the media
  // object's lifetime is owned by the HLE, so the guest's Release can't
  // strand it. Rebinding to another media object is always allowed;
  // only the stranding zero-clear is suppressed. Region (0,0) = disabled
  // (default), so existing behavior/tests are unaffected unless opted in.
  void SetMediaBindingGuardRegion(uint32_t start, uint32_t end);

  // Diagnostic +0x63c-callback seed (backend-independent; works under BOTH
  // interp and JIT, unlike the one-shot post-event-loop writes). The
  // Data East / cninja optional-callback guard reads [obj+0x63c]; on the
  // real device this is either a registered fn pointer or the framework's
  // "no callback" sentinel, never 0. Our register-step is type-gated and
  // doesn't run for these titles, so the slot stays 0 -> guard's `blx r3`
  // wanders to 0. This PERSISTENT read-override makes Read32(addr) return
  // `value` whenever the stored word is 0, so it survives the constructor
  // re-zeroing the slot every timer tick (the one-shot seed could not).
  // (0,_) = disabled. Value 0xF0000000 (trap_base) turns the optional
  // callback into a clean HLE no-op. Opt-in via ZEEB_SEED_READ=addr:value.
  void SetSeedRead(uint32_t address, uint32_t value);

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

  // Media-interface binding guard state (see SetMediaBindingGuardRegion).
  // media_guard_{start,end}_ define the HLE media-object region; when a
  // guarded pointer is stored to a slot, that slot address is recorded
  // in media_bound_slots_ so a later zero-clear of it can be suppressed.
  uint32_t media_guard_start_ = 0;
  uint32_t media_guard_end_ = 0;
  std::unordered_map<uint32_t, uint32_t> media_bound_slots_;

  // Persistent +0x63c seed read-override (see SetSeedRead). Disabled when
  // seed_read_addr_ == 0.
  uint32_t seed_read_addr_ = 0;
  uint32_t seed_read_value_ = 0;
  mutable uint64_t seed_read_hits_ = 0;

 public:
  uint64_t seed_read_hits() const { return seed_read_hits_; }
};

}  // namespace zeebulator
