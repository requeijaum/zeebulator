#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// MD5 digest engine (RFC 1321). Pure host-side state, no guest memory
// involved until GetDigest() is asked to publish the 16 bytes somewhere.
class Md5State {
 public:
  void Reset();
  void Update(const uint8_t* data, uint32_t len);
  std::array<uint8_t, 16> Finish() const;  // does not mutate state (matches
                                            // zeebx oracle: GetDigest can be
                                            // called more than once).

 private:
  void ProcessBlock(const uint8_t block[64]);

  uint32_t a_ = 0x67452301, b_ = 0xefcdab89, c_ = 0x98badcfe, d_ = 0x10325476;
  uint64_t total_len_ = 0;
  uint8_t buffer_[64] = {0};
  uint32_t buffer_len_ = 0;
};

// IHash interface (AEECLSID_MD5 = 0x01001015). Not in the official BREW SDK
// 4.0.2 headers (retired/undocumented API); vtable order confirmed against
// the zeebx oracle (src/aee_slots.rs HASH, src/machine.rs Interface::Hash),
// itself derived from observing Zeeboids -- the sole known caller across
// the 61-title reference corpus and the last missing API for it to boot.
// Slots: 0 AddRef, 1 Release, 2 QueryInterface, 3 Reset, 4 Update,
// 5 GetDigest, 6 GetDigestSize.
class HashHle {
 public:
  static constexpr uint32_t kClsidMd5 = 0x01001015u;

  HashHle(Memory& memory, HleRuntime& hle,
          std::function<uint32_t(uint32_t)> malloc_fn);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

 private:
  void AddRef(IArmCore& core);
  void Release(IArmCore& core);
  void QueryInterface(IArmCore& core);
  void ResetImpl(IArmCore& core);
  void UpdateImpl(IArmCore& core);
  void GetDigestImpl(IArmCore& core);
  void GetResultImpl(IArmCore& core);
  void SetKeyImpl(IArmCore& core);
  void GetDigestSizeImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  std::function<uint32_t(uint32_t)> malloc_fn_;
  Md5State md5_;
  uint32_t digest_addr_ = 0;  // lazily allocated, like the zeebx oracle
  uint32_t ref_count_ = 1;
};

}  // namespace zeebulator
