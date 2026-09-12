#include "core/brew/mem_astream_hle.h"

#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::MemAStreamHle;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kVtable = 0x80004000;
constexpr uint32_t kObjectRegion = 0x80005000;
constexpr uint32_t kScratch = 0x00090000;

enum MemAStreamSlot {
  kAddRef = 0,
  kRelease = 1,
  kReadable = 2,
  kRead = 3,
  kCancel = 4,
  kSet = 5,
  kSetEx = 6,
};

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  MemAStreamHle stream_hle{cpu.GetMemory(), hle, kObjectRegion};

  Fixture() {
    stream_hle.Build(kVtable);
  }

  uint32_t SlotAddr(MemAStreamSlot slot) {
    return cpu.GetMemory().Read32(kVtable + slot * 4);
  }
};

}  // namespace

TEST(MemAStreamHle, AllocateAndReadStreamData) {
  Fixture f;
  uint32_t stream = f.stream_hle.AllocateStream();
  ASSERT_NE(stream, 0u);

  // Write 10 bytes into memory
  uint32_t buf = kScratch;
  std::vector<uint8_t> data = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
  for (size_t i = 0; i < data.size(); ++i) {
    f.cpu.GetMemory().Write8(buf + i, data[i]);
  }

  // Call Set(stream, buf, 10, 0, false)
  f.hle.CallArmFunction(f.SlotAddr(kSet), stream, buf, 10, 0);

  // Read 4 bytes
  uint32_t dest = kScratch + 0x100;
  uint32_t read1 = f.hle.CallArmFunction(f.SlotAddr(kRead), stream, dest, 4);
  EXPECT_EQ(read1, 4u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 10u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 1), 20u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 2), 30u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 3), 40u);

  // Read next 8 bytes (should read remaining 6)
  uint32_t read2 = f.hle.CallArmFunction(f.SlotAddr(kRead), stream, dest + 4, 8);
  EXPECT_EQ(read2, 6u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 4), 50u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 9), 100u);

  // Subsequent read returns 0 (EOF)
  uint32_t read3 = f.hle.CallArmFunction(f.SlotAddr(kRead), stream, dest, 4);
  EXPECT_EQ(read3, 0u);
}

TEST(MemAStreamHle, AddRefAndReleaseCycle) {
  Fixture f;
  uint32_t stream = f.stream_hle.AllocateStream();
  ASSERT_NE(stream, 0u);

  uint32_t ref = f.hle.CallArmFunction(f.SlotAddr(kAddRef), stream);
  EXPECT_EQ(ref, 2u);

  uint32_t rel1 = f.hle.CallArmFunction(f.SlotAddr(kRelease), stream);
  EXPECT_EQ(rel1, 1u);

  uint32_t rel2 = f.hle.CallArmFunction(f.SlotAddr(kRelease), stream);
  EXPECT_EQ(rel2, 0u);
}

TEST(MemAStreamHle, ReadableNotifyIsDeliveredFromTickNotInline) {
  Fixture f;
  int calls = 0;
  uint32_t notify = f.hle.Register([&](zeebulator::IArmCore& core) {
    ++calls;
    core.SetRegister(zeebulator::kR0, 0);
  });
  const uint32_t stream = f.stream_hle.AllocateStream();
  f.hle.CallArmFunction(f.SlotAddr(kReadable), stream, notify, 0);
  EXPECT_EQ(calls, 0) << "BREW must not re-enter guest code inside the call";
  f.stream_hle.Tick();
  EXPECT_EQ(calls, 1);
  f.stream_hle.Tick();
  EXPECT_EQ(calls, 1) << "one registration, one notification";
}

TEST(MemAStreamHle, CancelPreventsQueuedReadableCallback) {
  Fixture f;
  int calls = 0;
  const uint32_t notify = f.hle.Register([&](zeebulator::IArmCore&) { ++calls; });
  const uint32_t stream = f.stream_hle.AllocateStream();
  f.hle.CallArmFunction(f.SlotAddr(kReadable), stream, notify, 7);
  f.hle.CallArmFunction(f.SlotAddr(kCancel), stream, 0, 0);
  f.stream_hle.Tick();
  EXPECT_EQ(calls, 0);
}
