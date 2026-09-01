#include "core/memory/memory.h"

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

// TEMPORARY debug: set by ArmInterpreter::Step (arm_interpreter.cpp).
extern uint32_t g_watch_pc;

void Memory::Write8(uint32_t address, uint8_t value) {
  // TEMPORARY debug watch: the game's per-sound struct (module+0x1c8,
  // the crash object) — log every byte write with the interpreter PC.
  // REMOVE after root-causing the +0x28 media-source write.
  if (address >= 0x803001c8 && address < 0x80300200) {
    std::fprintf(stderr, "[watch+0x%03x] pc=0x%08x addr=0x%08x val=0x%02x\n",
                 address - 0x803001c8, zeebulator::g_watch_pc, address, value);
  }
  MutablePage(address / kPageSize)[address & kPageMask] = value;
}

void Memory::Write16(uint32_t address, uint16_t value) {
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void Memory::Write32(uint32_t address, uint32_t value) {
  // TEMPORARY experiment (fix candidate a): the game stores module+0x7b8
  // (code) as its struct +0x28 media source → vtable[6] garbage → crash.
  // Redirect to a valid MediaHle object (the one the game already holds
  // at struct+8). REMOVE after the experiment.
  if (address == 0x803001f0) {
    std::fprintf(stderr, "[redirect+0x28] game wrote 0x%08x → 0x80200080\n", value);
    value = 0x80200080;
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
