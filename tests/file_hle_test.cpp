#include "core/brew/file_hle.h"

#include <sstream>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::FileHle;
using zeebulator::HleRuntime;
using zeebulator::VirtualFilesystem;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kMgrVtable = 0x80000000;
constexpr uint32_t kMgrObject = 0x80001000;
constexpr uint32_t kFileVtable = 0x80002000;
constexpr uint32_t kFileObjectRegion = 0x80003000;
constexpr uint32_t kScratch = 0x00090000;

void WriteCString(zeebulator::Memory& mem, uint32_t addr, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    mem.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
  }
  mem.Write8(addr + static_cast<uint32_t>(s.size()), 0);
}

// Every vtable slot for both interfaces, in the real verified order.
enum FileMgrSlot {
  kMgrOpenFile = 2,
  kMgrGetInfo = 3,
  kMgrRemove = 4,
  kMgrMkDir = 5,
  kMgrRmDir = 6,
  kMgrTest = 7,
  kMgrGetFreeSpace = 8,
  kMgrEnumInit = 10,
  kMgrEnumNext = 11,
  kMgrRename = 12,
};
enum FileSlot {
  kFileRead = 3,
  kFileWrite = 5,
  kFileGetInfo = 6,
  kFileSeek = 7,
  kFileTruncate = 8,
};

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  VirtualFilesystem vfs;
  FileHle file_hle{cpu.GetMemory(), hle, vfs, kFileObjectRegion};
  uint32_t mgr;

  Fixture() {
    vfs.AddFile("foo.txt", {'h', 'e', 'l', 'l', 'o'});
    vfs.AddFile("bar.bin", std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
    mgr = file_hle.Build(kMgrVtable, kMgrObject, kFileVtable);
  }

  uint32_t MgrSlot(FileMgrSlot slot) {
    return cpu.GetMemory().Read32(kMgrVtable + slot * 4);
  }
  uint32_t FileSlotAddr(FileSlot slot) {
    return cpu.GetMemory().Read32(kFileVtable + slot * 4);
  }
};

}  // namespace

TEST(FileHle, OpenExistingFileReturnsNonNullHandle) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  EXPECT_NE(handle, 0u);
}

TEST(FileHle, OpenMissingFileReturnsNull) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "nope.txt");
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  EXPECT_EQ(handle, 0u);
}

TEST(FileHle, ReadReturnsCorrectBytesAndAdvancesPosition) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);

  uint32_t dest = kScratch + 0x100;
  uint32_t n = f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle, dest, 3);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 'h');
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 1), 'e');
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 2), 'l');

  // Second read continues from where the first left off.
  uint32_t n2 = f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle, dest, 100);
  EXPECT_EQ(n2, 2u) << "only 2 bytes ('l','o') remain, even though 100 were requested";
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 'l');
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 1), 'o');
}

TEST(FileHle, SeekReturnsSuccessNotThePositionAndActuallyMoves) {
  // Real IFILE_Seek() returns AEE_SUCCESS(0)/AEE_EFAILED(1), not the
  // resulting position -- confirmed against the real AEEFile.h contract
  // (see PHASE8_LOG.md for how a wrong assumption here, returning the
  // position, was caught: Double Dragon's own real code checks Seek's
  // result against 0, so any nonzero real position was misread as a
  // failure).
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");  // 8 bytes: 1..8
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);

  // _SEEK_START = 0
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, 0, 3), 0u);
  uint32_t dest = kScratch + 0x100;
  f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle, dest, 1);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 4) << "byte at index 3 (0-based) is value 4";

  // _SEEK_CURRENT = 2 (position is now 4 after that read)
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, 2, 2), 0u);
  f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle, dest, 1);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 7) << "position should now be 6 (4+2)";

  // _SEEK_END = 1
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, 1, 0), 0u);
}

TEST(FileHle, SeekCurrentWithZeroDistanceTellsThePosition) {
  // The one real documented exception: _SEEK_CURRENT with moveDistance
  // 0 acts as "tell" and returns the current position.
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");  // 8 bytes
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);

  f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, 5);
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_CURRENT=*/2, 0), 5u);
}

TEST(FileHle, SeekOutOfBoundsFailsOnAReadOnlyFile) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");  // 8 bytes
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);

  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, 9), 1u)
      << "past EOF on a read-only file must fail";
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, -1), 1u)
      << "before the start must fail";
}

TEST(FileHle, SeekPastEofOnAWritableFileExtendsItInsteadOfFailing) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle =
      f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  ASSERT_NE(handle, 0u);

  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, 10), 0u);
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_CURRENT=*/2, 0), 10u)
      << "file grew to reach the seek target";
}

TEST(FileHle, TwoOpenFilesHaveIndependentPositions) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");
  WriteCString(f.cpu.GetMemory(), kScratch + 0x40, "bar.bin");
  uint32_t h1 = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  uint32_t h2 = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch + 0x40, 0);
  ASSERT_NE(h1, h2);

  uint32_t dest = kScratch + 0x100;
  f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), h1, dest, 2);  // advances h1 to pos 2
  uint32_t n2 = f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), h2, dest, 1);  // h2 still at pos 0
  EXPECT_EQ(n2, 1u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 1) << "bar.bin's first byte, unaffected by h1's reads";
}

TEST(FileHle, MgrGetInfoAndFileGetInfoReportCorrectSize) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");
  uint32_t info_addr = kScratch + 0x100;

  uint32_t result = f.hle.CallArmFunction(f.MgrSlot(kMgrGetInfo), f.mgr, kScratch, info_addr);
  EXPECT_EQ(result, 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(info_addr + 8), 8u) << "dwSize field at offset 8";

  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  f.cpu.GetMemory().Write32(info_addr + 8, 0xDEADBEEF);  // clobber to prove it gets rewritten
  f.hle.CallArmFunction(f.FileSlotAddr(kFileGetInfo), handle, info_addr);
  EXPECT_EQ(f.cpu.GetMemory().Read32(info_addr + 8), 8u);
}

TEST(FileHle, TestReturnsZeroForExistingNonzeroForMissing) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");
  EXPECT_EQ(f.hle.CallArmFunction(f.MgrSlot(kMgrTest), f.mgr, kScratch), 0u);

  WriteCString(f.cpu.GetMemory(), kScratch, "nope.txt");
  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrTest), f.mgr, kScratch), 0u);
}

TEST(FileHle, EnumInitThenEnumNextWalksAllFilesThenReturnsFalse) {
  Fixture f;
  f.hle.CallArmFunction(f.MgrSlot(kMgrEnumInit), f.mgr, 0, 0);

  uint32_t info_addr = kScratch + 0x100;
  uint32_t r1 = f.hle.CallArmFunction(f.MgrSlot(kMgrEnumNext), f.mgr, info_addr);
  EXPECT_EQ(r1, 1u);
  uint32_t r2 = f.hle.CallArmFunction(f.MgrSlot(kMgrEnumNext), f.mgr, info_addr);
  EXPECT_EQ(r2, 1u);
  uint32_t r3 = f.hle.CallArmFunction(f.MgrSlot(kMgrEnumNext), f.mgr, info_addr);
  EXPECT_EQ(r3, 0u) << "only 2 files were added; the 3rd call must signal end-of-enumeration";
}

TEST(FileHle, ReadOnlyMethodsAllReturnAnError) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");

  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrRemove), f.mgr, kScratch), 0u);
  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrMkDir), f.mgr, kScratch), 0u);
  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrRmDir), f.mgr, kScratch), 0u);
  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrRename), f.mgr, kScratch, kScratch), 0u);

  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);
  EXPECT_NE(f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle, kScratch, 1), 0u);
  EXPECT_NE(f.hle.CallArmFunction(f.FileSlotAddr(kFileTruncate), handle, 0), 0u);
}

TEST(FileHle, OpenFileWithCreateModeMakesANewWritableFile) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle =
      f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  EXPECT_NE(handle, 0u);
}

TEST(FileHle, WriteThenReadBackRoundTripsOnACreatedFile) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle =
      f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  ASSERT_NE(handle, 0u);

  uint32_t payload_addr = kScratch + 0x100;
  const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  for (int i = 0; i < 4; ++i) {
    f.cpu.GetMemory().Write8(payload_addr + i, payload[i]);
  }
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle, payload_addr, 4), 4u);

  // Seek back to start (position is at EOF right after the write).
  f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, 0);
  uint32_t read_addr = kScratch + 0x200;
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle, read_addr, 4), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(f.cpu.GetMemory().Read8(read_addr + i), payload[i]) << "byte " << i;
  }
}

TEST(FileHle, TestRecognizesAFileCreatedAtRuntime) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  EXPECT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrTest), f.mgr, kScratch), 0u)
      << "doesn't exist yet";

  constexpr uint32_t kOfmCreate = 4;
  ASSERT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate), 0u);
  EXPECT_EQ(f.hle.CallArmFunction(f.MgrSlot(kMgrTest), f.mgr, kScratch), 0u)
      << "exists after being created";
}

TEST(FileHle, ReopeningACreatedFileByNameSeesThePreviouslyWrittenBytes) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle1 =
      f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  ASSERT_NE(handle1, 0u);
  uint32_t payload_addr = kScratch + 0x100;
  f.cpu.GetMemory().Write8(payload_addr, 0x42);
  f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle1, payload_addr, 1);

  uint32_t handle2 = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  ASSERT_NE(handle2, 0u);
  uint32_t read_addr = kScratch + 0x200;
  EXPECT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileRead), handle2, read_addr, 1), 1u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(read_addr), 0x42u);
}

TEST(FileHle, GetFreeSpaceReturnsAPlausibleNonzeroAmount) {
  Fixture f;
  uint32_t free_bytes = f.hle.CallArmFunction(f.MgrSlot(kMgrGetFreeSpace), f.mgr, 0);
  EXPECT_GT(free_bytes, 0u);
}

TEST(FileHle, GetFreeSpaceWritesTotalWhenPointerIsNonNull) {
  Fixture f;
  uint32_t total_addr = kScratch + 0x300;
  f.cpu.GetMemory().Write32(total_addr, 0xDEADBEEF);
  uint32_t free_bytes =
      f.hle.CallArmFunction(f.MgrSlot(kMgrGetFreeSpace), f.mgr, total_addr);
  EXPECT_EQ(f.cpu.GetMemory().Read32(total_addr), free_bytes);
}

TEST(FileHle, LastOpenedFileProxyReadsFromWhicheverFileWasMostRecentlyOpened) {
  Fixture f;
  constexpr uint32_t kProxyVtable = 0x80004000;
  constexpr uint32_t kProxyObject = 0x80005000;
  uint32_t proxy = f.file_hle.BuildLastOpenedFileProxy(kProxyVtable, kProxyObject);
  uint32_t proxy_read = f.cpu.GetMemory().Read32(kProxyVtable + kFileRead * 4);

  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");  // 8 bytes: 1..8
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  ASSERT_NE(handle, 0u);
  f.hle.CallArmFunction(f.FileSlotAddr(kFileSeek), handle, /*_SEEK_START=*/0, 3);

  // Read through the proxy, not the real handle -- it should still see
  // bar.bin's real bytes starting from the position the real handle
  // was left at.
  uint32_t dest = kScratch + 0x100;
  EXPECT_EQ(f.hle.CallArmFunction(proxy_read, proxy, dest, 2), 2u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 4) << "byte at index 3 (0-based) is value 4";
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest + 1), 5);
}

TEST(FileHle, LastOpenedFileProxyFollowsANewerOpenFile) {
  Fixture f;
  constexpr uint32_t kProxyVtable = 0x80004000;
  constexpr uint32_t kProxyObject = 0x80005000;
  uint32_t proxy = f.file_hle.BuildLastOpenedFileProxy(kProxyVtable, kProxyObject);
  uint32_t proxy_read = f.cpu.GetMemory().Read32(kProxyVtable + kFileRead * 4);

  WriteCString(f.cpu.GetMemory(), kScratch, "bar.bin");
  f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0);
  WriteCString(f.cpu.GetMemory(), kScratch + 0x40, "foo.txt");  // "hello"
  f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch + 0x40, 0);

  uint32_t dest = kScratch + 0x100;
  EXPECT_EQ(f.hle.CallArmFunction(proxy_read, proxy, dest, 1), 1u);
  EXPECT_EQ(f.cpu.GetMemory().Read8(dest), 'h') << "proxy follows the newer open, not the first";
}

TEST(FileHle, LastOpenedFileProxyReturnsZeroWhenNothingHasBeenOpenedYet) {
  Fixture f;
  constexpr uint32_t kProxyVtable = 0x80004000;
  constexpr uint32_t kProxyObject = 0x80005000;
  uint32_t proxy = f.file_hle.BuildLastOpenedFileProxy(kProxyVtable, kProxyObject);
  uint32_t proxy_read = f.cpu.GetMemory().Read32(kProxyVtable + kFileRead * 4);

  uint32_t dest = kScratch + 0x100;
  EXPECT_EQ(f.hle.CallArmFunction(proxy_read, proxy, dest, 1), 0u);
}

// --- Real save-game persistence past this process's own lifetime ---
// (see FileHle::Serialize's own doc comment for the real bug this
// fixes: writable_files_ silently vanishing on every cold relaunch,
// indistinguishable from the game "not saving" at all).

TEST(FileHle, HasUnsavedWritesIsFalseUntilARealWriteHappens) {
  Fixture f;
  EXPECT_FALSE(f.file_hle.HasUnsavedWrites());

  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  ASSERT_NE(handle, 0u);
  EXPECT_FALSE(f.file_hle.HasUnsavedWrites()) << "creating a file alone isn't a write";

  f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle, kScratch, 1);
  EXPECT_TRUE(f.file_hle.HasUnsavedWrites());
}

TEST(FileHle, SerializeClearsHasUnsavedWrites) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle, kScratch, 1);
  ASSERT_TRUE(f.file_hle.HasUnsavedWrites());

  std::stringstream out;
  ASSERT_TRUE(f.file_hle.Serialize(out));
  EXPECT_FALSE(f.file_hle.HasUnsavedWrites());
}

TEST(FileHle, SerializeThenDeserializeRoundTripsRealSaveData) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "./udata/save.dat");
  constexpr uint32_t kOfmCreate = 4;
  uint32_t handle = f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, kOfmCreate);
  uint32_t payload_addr = kScratch + 0x100;
  const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  for (int i = 0; i < 4; ++i) f.cpu.GetMemory().Write8(payload_addr + i, payload[i]);
  ASSERT_EQ(f.hle.CallArmFunction(f.FileSlotAddr(kFileWrite), handle, payload_addr, 4), 4u);

  std::stringstream stream;
  ASSERT_TRUE(f.file_hle.Serialize(stream));

  // A completely fresh instance, matching a real cold relaunch.
  ArmInterpreter cpu2;
  HleRuntime hle2(cpu2, kTrapBase, kTrapSize);
  VirtualFilesystem vfs2;
  FileHle file_hle2(cpu2.GetMemory(), hle2, vfs2, kFileObjectRegion);
  uint32_t mgr2 = file_hle2.Build(kMgrVtable, kMgrObject, kFileVtable);
  ASSERT_TRUE(file_hle2.Deserialize(stream));

  WriteCString(cpu2.GetMemory(), kScratch, "./udata/save.dat");
  uint32_t handle2 = hle2.CallArmFunction(cpu2.GetMemory().Read32(kMgrVtable + kMgrOpenFile * 4),
                                           mgr2, kScratch, 0);
  ASSERT_NE(handle2, 0u) << "real save data should already exist after Deserialize";
  uint32_t read_addr = kScratch + 0x200;
  uint32_t file_read2 = cpu2.GetMemory().Read32(kFileVtable + kFileRead * 4);
  EXPECT_EQ(hle2.CallArmFunction(file_read2, handle2, read_addr, 4), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(cpu2.GetMemory().Read8(read_addr + i), payload[i]) << "byte " << i;
  }
}

TEST(FileHle, DeserializeOnATruncatedStreamFailsWithoutCrashing) {
  Fixture f;
  std::stringstream stream;
  stream.write("\x05\x00\x00\x00", 4);  // claims 5 entries, then nothing else
  EXPECT_FALSE(f.file_hle.Deserialize(stream));
}

TEST(FileHle, DeserializeRefusesWhenFilesAreAlreadyOpen) {
  Fixture f;
  WriteCString(f.cpu.GetMemory(), kScratch, "foo.txt");
  ASSERT_NE(f.hle.CallArmFunction(f.MgrSlot(kMgrOpenFile), f.mgr, kScratch, 0), 0u);

  std::stringstream stream;
  ASSERT_TRUE(f.file_hle.Serialize(stream));
  EXPECT_FALSE(f.file_hle.Deserialize(stream))
      << "an already-open file handle would be left dangling";
}
