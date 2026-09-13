#include "core/memory/memory.h"

#include <exception>

#include "core/control/debug_hooks.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

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
  // Mesmo motivo do Read32: duas buscas de pagina viravam uma. Isto esta no
  // caminho de busca de instrucao do modo Thumb -- ArmInterpreter::Step chama
  // Read16 uma vez por instrucao Thumb executada.
  const uint32_t offset = address & kPageMask;
  if (offset <= kPageSize - 2) {
    DebugHooks& hooks = DebugHooks::Instance();
    hooks.OnMemRead(address, 1);
    hooks.OnMemRead(address + 1, 1);
    const Page* page = FindPage(address / kPageSize);
    if (page == nullptr) return 0;
    const uint8_t* p = page->data() + offset;
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
  }
  return static_cast<uint16_t>(Read8(address)) |
         (static_cast<uint16_t>(Read8(address + 1)) << 8);
}

uint32_t Memory::Read32(uint32_t address) const {
  // Caminho rapido: uma unica busca de pagina em vez de quatro.
  //
  // Read32 chamava Read8 quatro vezes, e cada Read8 faz um `pages_.find()`
  // numa unordered_map -- ou seja, TODO acesso de 32 bits custava quatro
  // buscas em tabela hash. Num interpretador, acesso a memoria e o caminho
  // mais quente que existe, e a leitura de instrucao sozinha ja e um Read32
  // por instrucao executada.
  //
  // Um acesso de 4 bytes que nao cruza a fronteira da pagina pode resolver a
  // pagina uma vez e ler os quatro bytes direto. A condicao e verificada, nao
  // assumida: se o acesso cruzar a pagina (desalinhado no fim dela), cai no
  // caminho antigo byte a byte, que continua correto.
  //
  // As chamadas de DebugHooks sao preservadas byte a byte de proposito -- os
  // watchpoints (ZEEB_WWATCH) registram por byte, e agrupa-las mudaria o que
  // as ferramentas de depuracao mostram.
  uint32_t v;
  const uint32_t offset = address & kPageMask;
  if (offset <= kPageSize - 4) {
    DebugHooks& hooks = DebugHooks::Instance();
    hooks.OnMemRead(address, 1);
    hooks.OnMemRead(address + 1, 1);
    hooks.OnMemRead(address + 2, 1);
    hooks.OnMemRead(address + 3, 1);
    const Page* page = FindPage(address / kPageSize);
    if (page == nullptr) {
      v = 0;
    } else {
      const uint8_t* p = page->data() + offset;
      v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
  } else {
    v = static_cast<uint32_t>(Read8(address)) |
        (static_cast<uint32_t>(Read8(address + 1)) << 8) |
        (static_cast<uint32_t>(Read8(address + 2)) << 16) |
        (static_cast<uint32_t>(Read8(address + 3)) << 24);
  }
  // Persistent +0x63c optional-callback seed: if this exact word is the
  // (impossible-on-hardware) 0 and a seed is armed for it, substitute the
  // seed value so the guard's `blx r3` targets a valid no-op instead of 0.
  // Backend-independent (fires on every guard read, interp or JIT).
  if (seed_read_addr_ != 0 && address == seed_read_addr_ && v == 0) {
    v = seed_read_value_;
    if (seed_read_hits_ == 0) {
      std::fprintf(stderr, "[seedread] FIRST HIT [0x%08x]->0x%08x\n",
                   address, v);
    }
    ++seed_read_hits_;
  }
  return v;
 }

void Memory::SetSeedRead(uint32_t address, uint32_t value) {
  seed_read_addr_ = address;
  seed_read_value_ = value;
}

void Memory::Write8(uint32_t address, uint8_t value) {
  DebugHooks::Instance().OnMemWrite(address, 1);
  MutablePage(address / kPageSize)[address & kPageMask] = value;
}

void Memory::Write16(uint32_t address, uint16_t value) {
  const uint32_t offset = address & kPageMask;
  if (offset <= kPageSize - 2) {
    DebugHooks& hooks = DebugHooks::Instance();
    hooks.OnMemWrite(address, 1);
    hooks.OnMemWrite(address + 1, 1);
    uint8_t* p = MutablePage(address / kPageSize).data() + offset;
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    return;
  }
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
      if (std::getenv("ZEEB_LOG_MEDIAGUARD")) {
        std::fprintf(stderr, "[mediaguard] BIND slot[0x%08x]=0x%08x\n",
                     address, value);
      }
    } else if (value == 0 && !media_bound_slots_.empty()) {
      // A checagem de vazio evita uma busca em tabela hash a CADA escrita de
      // zero. Jogos zeram memoria o tempo todo (limpar buffers, inicializar
      // estruturas), e a maioria dos titulos nunca liga um ponteiro de midia --
      // para esses o mapa fica vazio a execucao inteira e a busca era puro
      // custo no caminho mais quente do emulador.
      auto it = media_bound_slots_.find(address);
      if (it != media_bound_slots_.end()) {
        // Only suppress while the binding is still LIVE, i.e. the slot
        // still holds the exact pointer we recorded. `media_bound_slots_`
        // is keyed by raw guest address, and guest addresses get recycled
        // -- most brutally on the stack. Real case found in Crash Nitro
        // Kart 2 (cnk2.mod): a transient stack slot at 0x0039e280 once
        // held a media pointer, the frame died, and ~95M instructions
        // later zlib's `inflate` reused that exact address for its local
        // `ret`. Its `ret = Z_OK` store (0x00175214 `str r1,[fp,#-64]`)
        // was silently swallowed here, so `inflate` returned the stale 2,
        // the .pof asset loader treated it as fatal, applet init bailed
        // out, and the game finally wandered to PC=0. Checking liveness
        // keeps the Double Dragon Release-clear case working (there the
        // slot really does still hold the bound interface) while letting
        // every recycled slot be written normally.
        static const bool legacy_guard = std::getenv("ZEEB_MEDIAGUARD_LEGACY") != nullptr;
        if (legacy_guard || Read32(address) == it->second) {
          if (std::getenv("ZEEB_LOG_MEDIAGUARD")) {
            std::fprintf(stderr, "[mediaguard] SUPPRESS-ZERO slot[0x%08x] (keep 0x%08x)\n",
                         address, it->second);
          }
          return;
        }
        // Slot was recycled for something else: the binding is dead.
        if (std::getenv("ZEEB_LOG_MEDIAGUARD")) {
          std::fprintf(stderr, "[mediaguard] STALE slot[0x%08x] (bound 0x%08x, now 0x%08x) -> allow\n",
                       address, it->second, Read32(address));
        }
        media_bound_slots_.erase(it);
      }
    }
  }
  // Mesmo caminho rapido do Read32: uma busca de pagina em vez de quatro,
  // com a condicao de nao cruzar a fronteira verificada e nao assumida.
  const uint32_t offset = address & kPageMask;
  if (offset <= kPageSize - 4) {
    DebugHooks& hooks = DebugHooks::Instance();
    hooks.OnMemWrite(address, 1);
    hooks.OnMemWrite(address + 1, 1);
    hooks.OnMemWrite(address + 2, 1);
    hooks.OnMemWrite(address + 3, 1);
    uint8_t* p = MutablePage(address / kPageSize).data() + offset;
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
    return;
  }
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
  Write8(address + 2, static_cast<uint8_t>(value >> 16));
  Write8(address + 3, static_cast<uint8_t>(value >> 24));
}

void Memory::WriteBlock16(uint32_t address, const uint16_t* values, size_t count) {
  size_t i = 0;
  while (i < count) {
    const uint32_t addr = address + static_cast<uint32_t>(i * 2u);
    const uint32_t offset = addr & kPageMask;
    // Quantas halfwords cabem ate o fim desta pagina (sem atravessar).
    const size_t espaco = (kPageSize - offset) / 2u;
    const size_t n = (count - i < espaco) ? (count - i) : espaco;
    if (n == 0) break;  // offset impar na ultima posicao da pagina
    const size_t bytes = n * 2u;
    DebugHooks& hooks = DebugHooks::Instance();
    hooks.OnMemWrite(addr, static_cast<uint32_t>(bytes));
    uint8_t* p = MutablePage(addr / kPageSize).data() + offset;
    for (size_t k = 0; k < n; ++k) {
      const uint16_t v = values[i + k];
      p[k * 2u] = static_cast<uint8_t>(v);
      p[k * 2u + 1u] = static_cast<uint8_t>(v >> 8);
    }
    i += n;
  }
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
  if (!in.good() || page_count > 65536u) return false;  // 256 MiB max snapshot

  std::unordered_map<uint32_t, std::unique_ptr<Page>> new_pages;
  try { new_pages.reserve(page_count); } catch (const std::exception&) { return false; }
  for (uint32_t i = 0; i < page_count; ++i) {
    uint32_t page_index = 0;
    in.read(reinterpret_cast<char*>(&page_index), sizeof(page_index));
    if (!in.good() || page_index >= (1u << 20) || new_pages.find(page_index) != new_pages.end())
      return false;
    std::unique_ptr<Page> page;
    try { page = std::make_unique<Page>(); } catch (const std::exception&) { return false; }
    in.read(reinterpret_cast<char*>(page->data()), static_cast<std::streamsize>(page->size()));
    if (!in.good()) return false;
    new_pages.emplace(page_index, std::move(page));
  }
  pages_ = std::move(new_pages);
  return true;
}

}  // namespace zeebulator
