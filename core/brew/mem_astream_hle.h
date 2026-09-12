#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IMemAStream interface (AEECLSID_MEMASTREAM = 0x0100100c).
// Documented in Qualcomm BREW SDK (AEE.h, AEEIAStream.h).
// Wraps a block of guest memory into an asynchronous stream.
// Vtable slots (7 total):
//   0: AddRef (IBase)
//   1: Release (IBase)
//   2: Readable (IAStream)
//   3: Read (IAStream)
//   4: Cancel (IAStream)
//   5: Set (IMemAStream)
//   6: SetEx (IMemAStream)
class MemAStreamHle {
 public:
  // BREW IAStream_Readable registers an asynchronous notification. Delivering
  // it inline would re-enter guest code inside the guest's own call. Queue it
  // and drain here, from the host event loop.
  void Tick();

  static constexpr uint32_t kClsidMemAStream = 0x0100100cu;

  MemAStreamHle(Memory& memory, HleRuntime& hle, uint32_t stream_object_region_start);

  uint32_t Build(uint32_t vtable_address);
  uint32_t AllocateStream();

 private:
  struct StreamState {
    uint32_t buffer = 0;
    uint32_t size = 0;
    uint32_t position = 0;
    uint32_t ref_count = 1;
  };

  void AddRef(IArmCore& core);
  void Release(IArmCore& core);
  void Readable(IArmCore& core);
  void Read(IArmCore& core);
  void Cancel(IArmCore& core);
  void Set(IArmCore& core);
  void SetEx(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t vtable_address_ = 0;
  uint32_t next_object_address_;
  std::unordered_map<uint32_t, StreamState> streams_;
  struct PendingReadable {
    uint32_t stream;
    uint32_t fn;
    uint32_t user;
  };
  std::vector<PendingReadable> pending_readable_;
};

}  // namespace zeebulator
