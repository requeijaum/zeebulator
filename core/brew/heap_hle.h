#pragma once

#include <cstdint>
#include <functional>
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IHeap interface (AEECLSID_HEAP = 0x01001002, sdk/inc/AEEHeap.h)
// 9 vtable slots (uses DECLARE_IBASE, so no QueryInterface):
// 0: AddRef, 1: Release, 2: Malloc, 3: Realloc, 4: Free,
// 5: StrDup, 6: CheckAvail, 7: GetMemStats, 8: GetModuleMemStats
class HeapHle {
 public:
  static constexpr uint32_t kClsidHeap = 0x01001002u;

  HeapHle(Memory& memory, HleRuntime& hle,
          std::function<uint32_t(uint32_t)> malloc_fn,
          std::function<uint32_t(uint32_t, uint32_t)> realloc_fn,
          std::function<void(uint32_t)> free_fn,
          std::function<uint32_t()> avail_fn,
          std::function<uint32_t()> used_fn);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

 private:
  void AddRef(IArmCore& core);
  void Release(IArmCore& core);
  void Malloc(IArmCore& core);
  void Realloc(IArmCore& core);
  void Free(IArmCore& core);
  void StrDup(IArmCore& core);
  void CheckAvail(IArmCore& core);
  void GetMemStats(IArmCore& core);
  void GetModuleMemStats(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  std::function<uint32_t(uint32_t)> malloc_fn_;
  std::function<uint32_t(uint32_t, uint32_t)> realloc_fn_;
  std::function<void(uint32_t)> free_fn_;
  std::function<uint32_t()> avail_fn_;
  std::function<uint32_t()> used_fn_;
  uint32_t ref_count_ = 1;
};

}  // namespace zeebulator
