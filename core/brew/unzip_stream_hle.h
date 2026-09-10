#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IUnzipAStream interface (AEECLSID_UNZIPSTREAM = 0x01001014).
// Qualcomm BREW SDK stream that wraps a compressed IAStream
// and produces uncompressed bytes on Read.
// Vtable slots (6 total):
//   0: AddRef (IBase)
//   1: Release (IBase)
//   2: Readable (IAStream)
//   3: Read (IAStream)
//   4: Cancel (IAStream)
//   5: SetStream (IUnzipAStream)
class UnzipStreamHle {
 public:
  static constexpr uint32_t kClsidUnzipStream = 0x01001014u;

  UnzipStreamHle(Memory& memory, HleRuntime& hle, uint32_t object_region_start,
                 std::function<uint32_t(uint32_t)> read_source_fn = {});

  uint32_t Build(uint32_t vtable_address);
  uint32_t AllocateStream();

  // Helper hook to drain data from a source stream object in guest memory
  using StreamDrainer = std::function<std::vector<uint8_t>(uint32_t source_obj)>;
  void SetStreamDrainer(StreamDrainer drainer) { drainer_ = std::move(drainer); }

 private:
  struct UnzipState {
    uint32_t source_stream = 0;
    std::vector<uint8_t> uncompressed;
    uint32_t position = 0;
    bool expanded = false;
    uint32_t ref_count = 1;
  };

  void AddRef(IArmCore& core);
  void Release(IArmCore& core);
  void Readable(IArmCore& core);
  void Read(IArmCore& core);
  void Cancel(IArmCore& core);
  void SetStream(IArmCore& core);

  bool Expand(UnzipState& state);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t vtable_address_ = 0;
  uint32_t next_object_address_;
  StreamDrainer drainer_;
  std::unordered_map<uint32_t, UnzipState> streams_;
};

}  // namespace zeebulator
