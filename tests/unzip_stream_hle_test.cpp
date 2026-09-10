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
