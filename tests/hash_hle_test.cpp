#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "core/brew/hash_hle.h"
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"
#include "core/memory/memory.h"

namespace zeebulator {
namespace {

constexpr uint32_t kVtableAddr = 0x80001000;
constexpr uint32_t kObjectAddr = 0x80002000;
constexpr uint32_t kScratch = 0x80100000;

class HashHleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hle_ = std::make_unique<HleRuntime>(cpu_, 0xF0000000, 0x10000);
    heap_cursor_ = 0x80200000;
    hash_hle_ = std::make_unique<HashHle>(
        cpu_.GetMemory(), *hle_, [this](uint32_t size) -> uint32_t {
          uint32_t ptr = heap_cursor_;
          heap_cursor_ += size;
          return ptr;
        });
    hash_obj_ = hash_hle_->Build(kVtableAddr, kObjectAddr);
  }

  uint32_t Call(uint32_t slot, uint32_t r1 = 0, uint32_t r2 = 0, uint32_t r3 = 0) {
    uint32_t method_addr = cpu_.GetMemory().Read32(kVtableAddr + slot * 4);
    return hle_->CallArmFunction(method_addr, hash_obj_, r1, r2, r3);
  }

  void WriteBytes(uint32_t addr, const char* text) {
    uint32_t len = static_cast<uint32_t>(std::strlen(text));
    for (uint32_t i = 0; i < len; ++i) {
      cpu_.GetMemory().Write8(addr + i, static_cast<uint8_t>(text[i]));
    }
  }

  std::string DigestHex(uint32_t addr) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
      uint8_t b = cpu_.GetMemory().Read8(addr + i);
      out.push_back(kHex[b >> 4]);
      out.push_back(kHex[b & 0xf]);
    }
    return out;
  }

  ArmInterpreter cpu_;
  std::unique_ptr<HleRuntime> hle_;
  std::unique_ptr<HashHle> hash_hle_;
  uint32_t hash_obj_ = 0;
  uint32_t heap_cursor_ = 0;
};

TEST_F(HashHleTest, ObjectHeaderPointsToVtable) {
  EXPECT_EQ(cpu_.GetMemory().Read32(kObjectAddr), kVtableAddr);
}

TEST_F(HashHleTest, AddRefAndRelease) {
  EXPECT_EQ(Call(0), 2u);  // AddRef
  EXPECT_EQ(Call(1), 1u);  // Release
}

// Slot order comes from the SDK (platform/deprecated/inc/AEESecurity.h):
// 0 AddRef, 1 Release, 2 Update, 3 GetResult, 4 Restart, 5 SetKey. IHash has no
// QueryInterface and no GetDigestSize; GetResult reports the size instead.
TEST_F(HashHleTest, GetResultReportsSixteenByteDigest) {
  uint32_t len_out = kScratch + 0x100;
  EXPECT_EQ(Call(3, kScratch, len_out), 0u);  // GetResult(pbData, pcbData)
  EXPECT_EQ(cpu_.GetMemory().Read32(len_out), 16u);
}

TEST_F(HashHleTest, EmptyInputMatchesKnownMd5Vector) {
  // MD5("") == d41d8cd98f00b204e9800998ecf8427e (RFC 1321 test vector)
  Call(4);  // Restart
  EXPECT_EQ(Call(3, kScratch), 0u);  // GetResult
  EXPECT_EQ(DigestHex(kScratch), "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_F(HashHleTest, AbcMatchesKnownMd5Vector) {
  // MD5("abc") == 900150983cd24fb0d6963f7d28e17f72 (RFC 1321 test vector)
  Call(4);  // Restart
  uint32_t src = 0x80003000;
  WriteBytes(src, "abc");
  Call(2, src, 3);  // Update(pData, nLen=3)
  EXPECT_EQ(Call(3, kScratch), 0u);  // GetResult
  EXPECT_EQ(DigestHex(kScratch), "900150983cd24fb0d6963f7d28e17f72");
}

TEST_F(HashHleTest, MultipleUpdatesAccumulateAcrossBlockBoundary) {
  // MD5 of the 56-byte RFC 1321 test string, split into pieces crossing the
  // internal 64-byte block boundary, must match feeding it in one shot.
  const char* full =
      "12345678901234567890123456789012345678901234567890123456789012345678901"
      "234567890";
  Call(4);  // Restart
  uint32_t src = 0x80004000;
  WriteBytes(src, full);
  uint32_t len = static_cast<uint32_t>(std::strlen(full));
  Call(2, src, len);            // Update
  Call(3, kScratch);            // GetResult
  std::string one_shot = DigestHex(kScratch);

  Call(4);  // Restart
  uint32_t piece1_len = 40, piece2_len = len - 40;
  Call(2, src, piece1_len);
  Call(2, src + piece1_len, piece2_len);
  Call(3, kScratch + 32);
  EXPECT_EQ(DigestHex(kScratch + 32), one_shot);
}

TEST_F(HashHleTest, GetResultIsIdempotentWithoutReUpdate) {
  Call(4);
  uint32_t src = 0x80005000;
  WriteBytes(src, "abc");
  Call(2, src, 3);
  Call(3, kScratch);
  std::string first_hex = DigestHex(kScratch);
  Call(3, kScratch + 32);
  EXPECT_EQ(DigestHex(kScratch + 32), first_hex);
}

// Plain MD5 has no key. The SDK reserves SetKey for the HMAC variants, so the
// honest answer is AEE_EUNSUPPORTED rather than a silent success.
TEST_F(HashHleTest, SetKeyReportsUnsupportedForPlainMd5) {
  EXPECT_EQ(Call(5, 0x80006000, 4), 20u);
}

}  // namespace
}  // namespace zeebulator
