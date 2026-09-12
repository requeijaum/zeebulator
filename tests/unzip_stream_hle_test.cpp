#include "core/brew/unzip_stream_hle.h"

#include <zlib.h>
#include <gtest/gtest.h>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::UnzipStreamHle;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kVtable = 0x80006000;
constexpr uint32_t kObjectRegion = 0x80007000;
constexpr uint32_t kScratch = 0x00090000;

enum UnzipStreamSlot {
  kAddRef = 0,
  kRelease = 1,
  kReadable = 2,
  kRead = 3,
  kCancel = 4,
  kSetStream = 5,
};

std::vector<uint8_t> DeflateBytes(const std::vector<uint8_t>& src) {
  z_stream strm{};
  deflateInit(&strm, Z_DEFAULT_COMPRESSION);
  strm.next_in = const_cast<Bytef*>(src.data());
  strm.avail_in = static_cast<uInt>(src.size());

  std::vector<uint8_t> out(src.size() * 2 + 128);
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());

  deflate(&strm, Z_FINISH);
  out.resize(strm.total_out);
  deflateEnd(&strm);
  return out;
}

}  // namespace

TEST(UnzipStreamHle, DecompressesDeflatedStreamData) {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  UnzipStreamHle unzip_hle{cpu.GetMemory(), hle, kObjectRegion};
  unzip_hle.Build(kVtable);

  std::vector<uint8_t> original = {'H', 'e', 'l', 'l', 'o', ' ', 'Z', 'e', 'e', 'b', 'o', '!'};
  std::vector<uint8_t> deflated = DeflateBytes(original);

  unzip_hle.SetStreamDrainer([&deflated](uint32_t) { return deflated; });

  uint32_t stream = unzip_hle.AllocateStream();
  ASSERT_NE(stream, 0u);

  // SetStream with a dummy source id
  uint32_t set_slot = cpu.GetMemory().Read32(kVtable + kSetStream * 4);
  hle.CallArmFunction(set_slot, stream, 0x1234);

  // Read uncompressed data
  uint32_t read_slot = cpu.GetMemory().Read32(kVtable + kRead * 4);
  uint32_t dest = kScratch;
  uint32_t read_bytes = hle.CallArmFunction(read_slot, stream, dest, 100);

  EXPECT_EQ(read_bytes, original.size());
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(dest + i), original[i]);
  }
}

TEST(UnzipStreamHle, ReadableNotifyIsDeliveredFromTickNotInline) {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  UnzipStreamHle unzip{cpu.GetMemory(), hle, kObjectRegion};
  unzip.Build(kVtable);

  int calls = 0;
  uint32_t notify = hle.Register([&](zeebulator::IArmCore& core) {
    ++calls;
    core.SetRegister(zeebulator::kR0, 0);
  });
  uint32_t readable = cpu.GetMemory().Read32(kVtable + kReadable * 4);
  hle.CallArmFunction(readable, unzip.AllocateStream(), notify, 0);
  EXPECT_EQ(calls, 0) << "BREW must not re-enter guest code inside the call";
  unzip.Tick();
  EXPECT_EQ(calls, 1);
}

TEST(UnzipStreamHle, FallbackSourceReadPreservesTheEnclosingHleCall) {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  UnzipStreamHle unzip{cpu.GetMemory(), hle, kObjectRegion};
  unzip.Build(kVtable);
  const std::vector<uint8_t> original = {'o', 'k'};
  const std::vector<uint8_t> compressed = DeflateBytes(original);
  bool delivered = false;
  const uint32_t source_vtable = 0x80008000, source = 0x80008100;
  const uint32_t source_read = hle.Register([&](zeebulator::IArmCore& c) {
    if (delivered) { c.SetRegister(zeebulator::kR0, 0); return; }
    const uint32_t dest = c.GetRegister(zeebulator::kR1);
    for (size_t i = 0; i < compressed.size(); ++i) c.GetMemory().Write8(dest + i, compressed[i]);
    delivered = true;
    c.SetRegister(zeebulator::kR0, static_cast<uint32_t>(compressed.size()));
  });
  cpu.GetMemory().Write32(source, source_vtable);
  cpu.GetMemory().Write32(source_vtable + 3 * 4, source_read);
  const uint32_t stream = unzip.AllocateStream();
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetStream * 4), stream, source);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kRead * 4), stream, kScratch, 8),
            original.size());
  EXPECT_EQ(cpu.GetMemory().Read8(kScratch), 'o');
  EXPECT_EQ(cpu.GetMemory().Read8(kScratch + 1), 'k');
}

TEST(UnzipStreamHle, RejectsSourceReturningMoreThanRequestedChunk) {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  UnzipStreamHle unzip{cpu.GetMemory(), hle, kObjectRegion};
  unzip.Build(kVtable);
  const uint32_t source_vtable = 0x80008000, source = 0x80008100;
  const uint32_t bad_read = hle.Register([](zeebulator::IArmCore& c) {
    c.SetRegister(zeebulator::kR0, 4097);
  });
  cpu.GetMemory().Write32(source, source_vtable);
  cpu.GetMemory().Write32(source_vtable + 3 * 4, bad_read);
  const uint32_t stream = unzip.AllocateStream();
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetStream * 4), stream, source);
  EXPECT_EQ(hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kRead * 4), stream, kScratch, 8),
            0xffffffffu);
}

TEST(UnzipStreamHle, ReleasePreventsQueuedReadableCallback) {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  UnzipStreamHle unzip{cpu.GetMemory(), hle, kObjectRegion};
  unzip.Build(kVtable);
  int calls = 0;
  const uint32_t notify = hle.Register([&](zeebulator::IArmCore&) { ++calls; });
  const uint32_t stream = unzip.AllocateStream();
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kReadable * 4), stream, notify, 0);
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kRelease * 4), stream);
  unzip.Tick();
  EXPECT_EQ(calls, 0);
}
