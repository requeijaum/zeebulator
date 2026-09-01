#include "core/memory/memory.h"

#include "core/control/debug_hooks.h"

#include <cstring>

namespace zeebulator {

Memory::Page& Memory::MutablePage(uint32_t page_index) {
  auto it = pages_.find(page_index);
  if (it != pages_.end()) return *it->second;
  auto page = std::make_unique<Page>();
  page->fill(0);
  auto [inserted, _] = pages_.emplace(page_index, std::move(page));
  return *inserted->second;
}

const Memory::Page* Memory::FindPage(uint32_t page_index) const {
  auto it = pages_.find(page_index);
  return it == pages_.end() ? nullptr : it->second.get();
}

uint8_t Memory::Read8(uint32_t address) const {
  DebugHooks::Instance().OnMemRead(address, 1);
  const Page* page = FindPage(address / kPageSize);
  return page ? (*page)[address & kPageMask] : 0;
}

uint16_t Memory::Read16(uint32_t address) const {
  return static_cast<uint16_t>(Read8(address)) |
         (static_cast<uint16_t>(Read8(address + 1)) << 8);
}

uint32_t Memory::Read32(uint32_t address) const {
  return static_cast<uint32_t>(Read8(address)) |
         (static_cast<uint32_t>(Read8(address + 1)) << 8) |
         (static_cast<uint32_t>(Read8(address + 2)) << 16) |
         (static_cast<uint32_t>(Read8(address + 3)) << 24);
 }

void Memory::Write8(uint32_t address, uint8_t value) {
  DebugHooks::Instance().OnMemWrite(address, 1);
  MutablePage(address / kPageSize)[address & kPageMask] = value;
}

void Memory::Write16(uint32_t address, uint16_t value) {
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void Memory::SetMediaBindingGuardRegion(uint32_t start, uint32_t end) {
  media_guard_start_ = start;
  media_guard_end_ = end;
  media_bound_slots_.clear();
}

void Memory::Write32(uint32_t address, uint32_t value) {
  // Phase-1 media-interface binding guard (replaces the old +0x28
  // address-heuristic redirect HACK). See memory.h and
  // research/sources/2026-08-31_dd-media-interface-contract.md.
  //
  // The proven bug: the game's Play helper (ddragonz.mod 0x11d04c) reads
  // media_source+8 -> [+8]=iface -> [iface]=vtable -> vtable[6]=Play and
  // calls it. Our HLE binds a real MediaHle interface object into that
  // +8 slot, but the game's own Release helper (0x11f424) zeroes +8
  // before the (input-gated) Play path runs, so vtable[6] dereferences
  // NULL -> BX 0 wander. Zeemu avoids this by owning the IMediaPCM
  // object's lifetime on the host side; we do the same minimally: once a
  // slot holds a pointer INTO the HLE media-object region, suppress a
  // later zero-clear of that exact slot so the binding survives to Play.
  if (media_guard_end_ > media_guard_start_) {
    if (value >= media_guard_start_ && value < media_guard_end_) {
      // Game (or HLE) binds a real media interface here -> remember slot.
      media_bound_slots_[address] = value;
    } else if (value == 0) {
      auto it = media_bound_slots_.find(address);
      if (it != media_bound_slots_.end()) {
        // Release-clear of a live HLE-owned media binding: skip it so the
        // Play path still finds the interface. (Genuine rebinding to a
        // different, non-zero pointer is handled by the branch above.)
        return;
      }
    }
  }
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
  Write8(address + 2, static_cast<uint8_t>(value >> 16));
  Write8(address + 3, static_cast<uint8_t>(value >> 24));
}

void Memory::Load(uint32_t address, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    Write8(address + static_cast<uint32_t>(i), data[i]);
  }
}

bool Memory::Serialize(std::ostream& out) const {
  uint32_t page_count = static_cast<uint32_t>(pages_.size());
  out.write(reinterpret_cast<const char*>(&page_count), sizeof(page_count));
  for (const auto& [page_index, page] : pages_) {
    out.write(reinterpret_cast<const char*>(&page_index), sizeof(page_index));
    out.write(reinterpret_cast<const char*>(page->data()), static_cast<std::streamsize>(page->size()));
  }
  return out.good();
}

bool Memory::Deserialize(std::istream& in) {
  uint32_t page_count = 0;
  in.read(reinterpret_cast<char*>(&page_count), sizeof(page_count));
  if (!in.good()) return false;

  std::unordered_map<uint32_t, std::unique_ptr<Page>> new_pages;
  new_pages.reserve(page_count);
  for (uint32_t i = 0; i < page_count; ++i) {
    uint32_t page_index = 0;
    in.read(reinterpret_cast<char*>(&page_index), sizeof(page_index));
    if (!in.good()) return false;
    auto page = std::make_unique<Page>();
    in.read(reinterpret_cast<char*>(page->data()), static_cast<std::streamsize>(page->size()));
    if (!in.good()) return false;
    new_pages.emplace(page_index, std::move(page));
  }
  pages_ = std::move(new_pages);
  return true;
}

}  // namespace zeebulator
