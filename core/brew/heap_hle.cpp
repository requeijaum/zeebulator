#include "core/brew/heap_hle.h"
#include <vector>
#include "core/brew/interface_object.h"

namespace zeebulator {

HeapHle::HeapHle(Memory& memory, HleRuntime& hle,
                 std::function<uint32_t(uint32_t)> malloc_fn,
                 std::function<uint32_t(uint32_t, uint32_t)> realloc_fn,
                 std::function<void(uint32_t)> free_fn,
                 std::function<uint32_t()> avail_fn,
                 std::function<uint32_t()> used_fn)
    : memory_(memory),
      hle_(hle),
      malloc_fn_(std::move(malloc_fn)),
      realloc_fn_(std::move(realloc_fn)),
      free_fn_(std::move(free_fn)),
      avail_fn_(std::move(avail_fn)),
      used_fn_(std::move(used_fn)) {}

uint32_t HeapHle::Build(uint32_t vtable_address, uint32_t object_address) {
  std::vector<HleRuntime::HleFunction> methods = {
      [this](IArmCore& c) { AddRef(c); },             // 0 AddRef
      [this](IArmCore& c) { Release(c); },            // 1 Release
      [this](IArmCore& c) { Malloc(c); },             // 2 Malloc
      [this](IArmCore& c) { Realloc(c); },            // 3 Realloc
      [this](IArmCore& c) { Free(c); },               // 4 Free
      [this](IArmCore& c) { StrDup(c); },             // 5 StrDup
      [this](IArmCore& c) { CheckAvail(c); },         // 6 CheckAvail
      [this](IArmCore& c) { GetMemStats(c); },         // 7 GetMemStats
      [this](IArmCore& c) { GetModuleMemStats(c); },   // 8 GetModuleMemStats
  };

  const std::vector<const char*> slot_names = {
      "AddRef", "Release", "Malloc", "Realloc", "Free",
      "StrDup", "CheckAvail", "GetMemStats", "GetModuleMemStats"
  };

  return BuildInterfaceObjectLabeled(memory_, hle_, vtable_address,
                                     object_address, methods, "IHeap",
                                     slot_names);
}

void HeapHle::AddRef(IArmCore& core) {
  ++ref_count_;
  core.SetRegister(kR0, ref_count_);
}

void HeapHle::Release(IArmCore& core) {
  if (ref_count_ > 0) {
    --ref_count_;
  }
  core.SetRegister(kR0, ref_count_);
}

void HeapHle::Malloc(IArmCore& core) {
  uint32_t size = core.GetRegister(kR1) & ~0x80000000u; // Mask ALLOC_NO_ZMEM
  uint32_t ptr = malloc_fn_ ? malloc_fn_(size ? size : 1) : 0;
  core.SetRegister(kR0, ptr);
}

void HeapHle::Realloc(IArmCore& core) {
  uint32_t p_old = core.GetRegister(kR1);
  uint32_t size = core.GetRegister(kR2) & ~0x80000000u;
  uint32_t ptr = realloc_fn_ ? realloc_fn_(p_old, size ? size : 1) : 0;
  core.SetRegister(kR0, ptr);
}

void HeapHle::Free(IArmCore& core) {
  uint32_t p = core.GetRegister(kR1);
  if (free_fn_ && p != 0) {
    free_fn_(p);
  }
  core.SetRegister(kR0, 0);
}

void HeapHle::StrDup(IArmCore& core) {
  // AECHAR is UTF-16 (uint16_t), terminated by 0x0000
  uint32_t src = core.GetRegister(kR1);
  if (src == 0) {
    core.SetRegister(kR0, 0);
    return;
  }
  uint32_t len = 0;
  while (memory_.Read16(src + len * 2) != 0) {
    ++len;
  }
  uint32_t total_bytes = (len + 1) * 2;
  uint32_t dst = malloc_fn_ ? malloc_fn_(total_bytes) : 0;
  if (dst != 0) {
    for (uint32_t i = 0; i <= len; ++i) {
      memory_.Write16(dst + i * 2, memory_.Read16(src + i * 2));
    }
  }
  core.SetRegister(kR0, dst);
}

void HeapHle::CheckAvail(IArmCore& core) {
  uint32_t req = core.GetRegister(kR1) & ~0x80000000u;
  uint32_t avail = avail_fn_ ? avail_fn_() : 0;
  core.SetRegister(kR0, avail >= req ? 1 : 0);
}

void HeapHle::GetMemStats(IArmCore& core) {
  // BREW SDK AEEHeap.h: returns current used memory bytes
  uint32_t used = used_fn_ ? used_fn_() : 0;
  core.SetRegister(kR0, used);
}

void HeapHle::GetModuleMemStats(IArmCore& core) {
  // R2 = uint32* pMaxUsed, R3 = uint32* pCurUsed
  uint32_t p_max = core.GetRegister(kR2);
  uint32_t p_cur = core.GetRegister(kR3);
  uint32_t used = used_fn_ ? used_fn_() : 0;
  if (p_max != 0) {
    memory_.Write32(p_max, used);
  }
  if (p_cur != 0) {
    memory_.Write32(p_cur, used);
  }
  core.SetRegister(kR0, 0);
}

}  // namespace zeebulator
