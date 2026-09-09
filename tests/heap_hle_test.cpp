#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "core/brew/heap_hle.h"
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"
#include "core/memory/memory.h"

namespace zeebulator {
namespace {

constexpr uint32_t kVtableAddr = 0x80001000;
constexpr uint32_t kObjectAddr = 0x80002000;

class HeapHleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hle_ = std::make_unique<HleRuntime>(cpu_, 0xF0000000, 0x10000);
    simulated_heap_cursor_ = 0x80100000;
    simulated_heap_total_ = 1024 * 1024; // 1MB
    simulated_used_ = 0;

    heap_hle_ = std::make_unique<HeapHle>(
        cpu_.GetMemory(), *hle_,
        [this](uint32_t size) -> uint32_t {
          uint32_t ptr = simulated_heap_cursor_;
          simulated_heap_cursor_ += size;
          simulated_used_ += size;
          return ptr;
        },
        [this](uint32_t ptr, uint32_t size) -> uint32_t {
          uint32_t new_ptr = simulated_heap_cursor_;
          simulated_heap_cursor_ += size;
          simulated_used_ += size;
          return new_ptr;
        },
        [](uint32_t /*ptr*/) {},
        [this]() -> uint32_t {
          return simulated_heap_total_ > simulated_used_
                     ? simulated_heap_total_ - simulated_used_
                     : 0;
        },
        [this]() -> uint32_t { return simulated_used_; });

    heap_obj_ = heap_hle_->Build(kVtableAddr, kObjectAddr);
  }

  uint32_t Call(uint32_t slot, uint32_t r1 = 0, uint32_t r2 = 0, uint32_t r3 = 0) {
    uint32_t method_addr = cpu_.GetMemory().Read32(kVtableAddr + slot * 4);
    return hle_->CallArmFunction(method_addr, heap_obj_, r1, r2, r3);
  }

  ArmInterpreter cpu_;
  std::unique_ptr<HleRuntime> hle_;
  std::unique_ptr<HeapHle> heap_hle_;
  uint32_t heap_obj_ = 0;
  uint32_t simulated_heap_cursor_ = 0;
  uint32_t simulated_heap_total_ = 0;
  uint32_t simulated_used_ = 0;
};

TEST_F(HeapHleTest, ObjectHeaderPointsToVtable) {
  EXPECT_EQ(cpu_.GetMemory().Read32(kObjectAddr), kVtableAddr);
}

TEST_F(HeapHleTest, AddRefAndRelease) {
  EXPECT_EQ(Call(0), 2u); // AddRef (initial was 1)
  EXPECT_EQ(Call(1), 1u); // Release
}

TEST_F(HeapHleTest, MallocMasksAllocNoZmem) {
  // Call Malloc with 128 bytes | ALLOC_NO_ZMEM
  uint32_t ptr = Call(2, 128 | 0x80000000u);
  EXPECT_EQ(ptr, 0x80100000u);
  EXPECT_EQ(simulated_used_, 128u);
}

TEST_F(HeapHleTest, CheckAvailReturnsTrueWhenMemoryFits) {
  // CheckAvail for 500KB should succeed
  EXPECT_EQ(Call(6, 500 * 1024), 1u);
  // CheckAvail for 2MB should fail
  EXPECT_EQ(Call(6, 2 * 1024 * 1024), 0u);
}

TEST_F(HeapHleTest, GetMemStatsReturnsUsedBytes) {
  Call(2, 256);
  EXPECT_EQ(Call(7), 256u);
}

TEST_F(HeapHleTest, GetModuleMemStatsWritesUsedMemory) {
  uint32_t p_max = 0x80003000;
  uint32_t p_cur = 0x80003004;
  Call(2, 512);

  Call(8, 0, p_max, p_cur);
  EXPECT_EQ(cpu_.GetMemory().Read32(p_max), 512u);
  EXPECT_EQ(cpu_.GetMemory().Read32(p_cur), 512u);
}

TEST_F(HeapHleTest, StrDupCopiesUtf16String) {
  uint32_t src = 0x80004000;
  // "Hi\0" in UTF-16
  cpu_.GetMemory().Write16(src + 0, 'H');
  cpu_.GetMemory().Write16(src + 2, 'i');
  cpu_.GetMemory().Write16(src + 4, 0);

  uint32_t dst = Call(5, src);
  EXPECT_NE(dst, 0u);
  EXPECT_EQ(cpu_.GetMemory().Read16(dst + 0), 'H');
  EXPECT_EQ(cpu_.GetMemory().Read16(dst + 2), 'i');
  EXPECT_EQ(cpu_.GetMemory().Read16(dst + 4), 0);
}

}  // namespace
}  // namespace zeebulator
