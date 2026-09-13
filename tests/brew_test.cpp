#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/interface_object.h"
#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/bar.h"

using zeebulator::ArmInterpreter;
using zeebulator::Backend;
using zeebulator::HleRuntime;
using zeebulator::IDisplayHle;
using zeebulator::IShellHle;
using zeebulator::PixelFormat;
using zeebulator::ZPadState;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x1000;
constexpr uint32_t kVtableAddr = 0x80000000;
constexpr uint32_t kObjectAddr = 0x80001000;

class TestBackend : public Backend {
 public:
  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override {
    push_count++;
    last_width = width;
    last_height = height;
    last_format = format;
    last_frame.assign(static_cast<const uint16_t*>(framebuffer),
                       static_cast<const uint16_t*>(framebuffer) +
                           static_cast<size_t>(width) * height);
  }
  void PushAudioSamples(const int16_t*, size_t, int) override {}
  ZPadState PollInput() override { return {}; }

  int push_count = 0;
  int last_width = 0;
  int last_height = 0;
  PixelFormat last_format = PixelFormat::kRGB565;
  std::vector<uint16_t> last_frame;
};

// AECHAR is a real single 8-bit byte per character on this real Zeebo/BREW
// build, not the 16-bit UTF-16 code unit real BREW's AEEText.h documents
// as the general case -- see IDisplayHle::DrawText's own doc comment for
// the real evidence (a real Double Dragon DrawText call site's in-memory
// string only decodes to legible English text when read one byte per
// character).
void WriteAeeCharString(zeebulator::Memory& mem, uint32_t addr, const std::string& text) {
  for (size_t i = 0; i < text.size(); ++i) {
    mem.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  mem.Write8(addr + static_cast<uint32_t>(text.size()), 0);
}

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

void AppendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

// A minimal, well-formed synthetic ".bar" archive with one directory
// record and one resource -- see core/loader/bar.h/tests/bar_test.cpp
// for the full real layout this mirrors.
std::vector<uint8_t> BuildBarWithOneResource(uint16_t type, uint16_t id,
                                              const std::vector<uint8_t>& resource) {
  constexpr uint32_t kHeaderSize = 32;
  constexpr uint32_t kSubHeaderSize = 16;
  uint32_t table1_size = kSubHeaderSize + 8;  // one directory record
  uint32_t table_start = kHeaderSize + table1_size;
  uint32_t data_start = table_start + 2 * 4;  // one offset + one sentinel
  uint32_t data_size = static_cast<uint32_t>(resource.size());

  std::vector<uint8_t> out;
  AppendU32LE(out, 0x00010011);
  AppendU32LE(out, 0x003e0001);
  AppendU32LE(out, kHeaderSize);
  AppendU32LE(out, table1_size);
  AppendU32LE(out, table_start);
  AppendU32LE(out, 1);  // entry_count
  AppendU32LE(out, data_start);
  AppendU32LE(out, data_size);
  out.resize(kHeaderSize + kSubHeaderSize, 0);
  AppendU16LE(out, type);
  AppendU16LE(out, id);
  AppendU16LE(out, 0);  // unknown
  AppendU16LE(out, 0);  // entry_index
  AppendU32LE(out, data_start);
  AppendU32LE(out, data_start + data_size);  // sentinel
  out.insert(out.end(), resource.begin(), resource.end());
  return out;
}

}  // namespace

TEST(IShellHle, AllStubbedMethodsReturnZeroWithoutCrashing) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  uint32_t shell = shell_hle.Build(kVtableAddr, kObjectAddr);
  EXPECT_EQ(shell, kObjectAddr);

  // AddRef = slot 0, Release = slot 1 -- CreateInstance (slot 2) has real
  // behavior now, tested separately below.
  for (uint32_t slot : {0u, 1u}) {
    uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr), 0u);
  }
}

TEST(IShellHle, CreateInstanceReturnsFailedForAnUnregisteredClass) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr = 0x90000;
  cpu.GetMemory().Write32(kPpObjAddr, 0xDEADBEEF);
  // int CreateInstance(IShell *po, AEECLSID cls, void **ppo)
  // Real Qualcomm BREW returns ECLASSNOTSUPPORT (3, per AEEError.h) and zeroes *ppo on unknown class
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, /*cls=*/0x1234, kPpObjAddr), 3u);
  EXPECT_EQ(cpu.GetMemory().Read32(kPpObjAddr), 0u);
}

TEST(IShellHle, CreateInstanceReturnsARegisteredInstance) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  constexpr uint32_t kClsId = 0x01001001;  // AEECLSID_DISPLAY
  constexpr uint32_t kDisplayObj = 0x80003000;
  shell_hle.RegisterInstance(kClsId, kDisplayObj);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr = 0x90000;
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, kClsId, kPpObjAddr), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(kPpObjAddr), kDisplayObj);
}

TEST(IShellHle, GetHandlerReturnsTheClassItselfForTheRealAudioMediaClass) {
  // AEECLSID GetHandler(IShell *ps, AEECLSID cls, const char *pszMIME) --
  // slot 32. Real evidence (TASKS.md/PHASE8_LOG.md Phase 8, the sound
  // investigation): live-captured Double Dragon calling
  // ISHELL_GetHandler(shell, 0x01005500, pszMIME) and immediately
  // feeding the return value into ISHELL_CreateInstance -- see
  // ishell.h's own doc comment for the full derivation.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 32 * 4);
  constexpr uint32_t kAudioMediaCls = 0x01005500;
  constexpr uint32_t kMimeAddr = 0x90000;
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, kAudioMediaCls, kMimeAddr), kAudioMediaCls);
}

TEST(IShellHle, GetHandlerReturnsZeroForAnUnrecognizedClass) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 32 * 4);
  constexpr uint32_t kMimeAddr = 0x90000;
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, /*cls=*/0x1234, kMimeAddr), 0u);
}

TEST(IShellHle, GetHandlerResultChainsIntoCreateInstanceForTheRegisteredMediaObject) {
  // The real end-to-end shape (see ishell.h's doc comment): real code
  // calls GetHandler, then immediately CreateInstance()s whatever it
  // returned. Confirms the two slots compose correctly for the real
  // AEECLSID_MEDIA value, exactly like tools/game_probe.cpp wires
  // MediaHle's object in.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  constexpr uint32_t kAudioMediaCls = 0x01005500;
  constexpr uint32_t kMediaObj = 0x80003000;
  shell_hle.RegisterInstance(kAudioMediaCls, kMediaObj);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t get_handler_sentinel = cpu.GetMemory().Read32(kVtableAddr + 32 * 4);
  constexpr uint32_t kMimeAddr = 0x90000;
  uint32_t cls =
      hle.CallArmFunction(get_handler_sentinel, kObjectAddr, kAudioMediaCls, kMimeAddr);

  uint32_t create_instance_sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr = 0x90004;
  EXPECT_EQ(hle.CallArmFunction(create_instance_sentinel, kObjectAddr, cls, kPpObjAddr), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(kPpObjAddr), kMediaObj);
}

TEST(IShellHle, RegisterFactoryReturnsAFreshObjectFromEachCreateInstanceCall) {
  // Real evidence this matters, not just defensive design (see
  // RegisterFactory's own doc comment): the real GetHandler-
  // >CreateInstance call pair for AEECLSID_MEDIA runs once per cached
  // sound.ggz resource activated into a playback slot, i.e. once per
  // sound -- tools/game_probe.cpp uses RegisterFactory rather than
  // RegisterInstance for exactly this class so each sound gets its own
  // IMedia instance instead of all of them sharing (and stomping) one.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  constexpr uint32_t kAudioMediaCls = 0x01005500;
  uint32_t next_obj = 0x80003000;
  shell_hle.RegisterFactory(kAudioMediaCls, [&next_obj]() {
    uint32_t obj = next_obj;
    next_obj += 4;
    return obj;
  });
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t create_instance_sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr1 = 0x90000;
  constexpr uint32_t kPpObjAddr2 = 0x90004;
  EXPECT_EQ(hle.CallArmFunction(create_instance_sentinel, kObjectAddr, kAudioMediaCls, kPpObjAddr1),
            0u);
  EXPECT_EQ(hle.CallArmFunction(create_instance_sentinel, kObjectAddr, kAudioMediaCls, kPpObjAddr2),
            0u);
  uint32_t first = cpu.GetMemory().Read32(kPpObjAddr1);
  uint32_t second = cpu.GetMemory().Read32(kPpObjAddr2);
  EXPECT_NE(first, second);
}

TEST(IShellHle, GetDeviceInfoWritesRealScreenDimensions) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle, /*screen_width=*/640, /*screen_height=*/480);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 4 * 4);
  constexpr uint32_t kDeviceInfoAddr = 0x90000;
  cpu.GetMemory().Write32(kDeviceInfoAddr, 0xDEADBEEF);  // poison, confirms a real write happens
  // void GetDeviceInfo(IShell *po, AEEDeviceInfo *pdi)
  hle.CallArmFunction(sentinel, kObjectAddr, kDeviceInfoAddr);
  // Real AEEDeviceInfo starts with uint16 cxScreen; uint16 cyScreen; --
  // see GetDeviceInfoImpl's own doc comment for the real header/
  // disassembly evidence.
  EXPECT_EQ(cpu.GetMemory().Read16(kDeviceInfoAddr + 0), 640u);
  EXPECT_EQ(cpu.GetMemory().Read16(kDeviceInfoAddr + 2), 480u);
}

static std::string ReadCStringHelper(zeebulator::Memory& memory, uint32_t addr) {
  std::string s;
  while (true) {
    uint8_t c = memory.Read8(addr++);
    if (c == 0) break;
    s.push_back(static_cast<char>(c));
  }
  return s;
}

TEST(IShellHle, Slot43FirstCallReturnsTheConfirmedRealLiteral35) {
  // Real, confirmed return value -- ISHELL_DetectType contract from BREW SDK 4.0.2
  // and zeebx machine.rs:2209. When cpBuf == NULL && cpszName == NULL, guest asks
  // how many bytes are needed. It returns ENEEDMORE (35) and sets *pdwSize = 16.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kSizePtr = 0x90000;
  cpu.GetMemory().Write32(kSizePtr, 0);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  // Args: R0=shell, R1=cpBuf(0), R2=pdwSize(kSizePtr), R3=cpszName(0)
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0, kSizePtr, 0), 35u);
  EXPECT_EQ(cpu.GetMemory().Read32(kSizePtr), 16u);
}

TEST(IShellHle, Slot43SecondCallWithBufferReturnsZeroAndDetectsMime) {
  // Second call passes the 16 bytes read and receives 0 (SUCCESS) with MIME pointer.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kBufAddr = 0x91000;
  constexpr uint32_t kSizePtr = 0x90000;
  constexpr uint32_t kMimeOutPtr = 0x90004;

  // PNG magic bytes
  const uint8_t png_header[16] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 16; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), png_header[i]);
  }
  cpu.GetMemory().Write32(kSizePtr, 16);
  cpu.GetMemory().Write32(kMimeOutPtr, 0);

  // Set SP with 5th argument pcpszMIME pointing to kMimeOutPtr
  cpu.SetRegister(zeebulator::kSP, 0x80000000);
  cpu.GetMemory().Write32(0x80000000, kMimeOutPtr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  // R0=shell, R1=cpBuf, R2=pdwSize, R3=cpszName(0), stack=pcpszMIME
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, kBufAddr, kSizePtr, 0), 0u);
  uint32_t mime_str_addr = cpu.GetMemory().Read32(kMimeOutPtr);
  EXPECT_NE(mime_str_addr, 0u);
  EXPECT_EQ(ReadCStringHelper(cpu.GetMemory(), mime_str_addr), "image/png");
}

TEST(IShellHle, Slot43DetectsMimeByNameFallback) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kNameAddr = 0x92000;
  constexpr uint32_t kMimeOutPtr = 0x90004;

  const std::string filename = "music.mid";
  for (size_t i = 0; i <= filename.size(); ++i) {
    cpu.GetMemory().Write8(kNameAddr + static_cast<uint32_t>(i), static_cast<uint8_t>(filename[i]));
  }
  cpu.SetRegister(zeebulator::kSP, 0x80000000);
  cpu.GetMemory().Write32(0x80000000, kMimeOutPtr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  // cpBuf=0, size_ptr=0, name_ptr=kNameAddr
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0, 0, kNameAddr), 0u);
  uint32_t mime_str_addr = cpu.GetMemory().Read32(kMimeOutPtr);
  EXPECT_NE(mime_str_addr, 0u);
  EXPECT_EQ(ReadCStringHelper(cpu.GetMemory(), mime_str_addr), "audio/mid");
}

TEST(IShellHle, Slot43ReturnsENOTYPEWhenUnrecognized) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kBufAddr = 0x91000;
  constexpr uint32_t kSizePtr = 0x90000;
  constexpr uint32_t kNameAddr = 0x92000;

  for (size_t i = 0; i < 16; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), 0);
  }
  cpu.GetMemory().Write32(kSizePtr, 16);
  const std::string filename = "unknown.xyz";
  for (size_t i = 0; i <= filename.size(); ++i) {
    cpu.GetMemory().Write8(kNameAddr + static_cast<uint32_t>(i), static_cast<uint8_t>(filename[i]));
  }

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  // ENOTYPE is 34
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, kBufAddr, kSizePtr, kNameAddr), 34u);
}

TEST(IShellHle, ResumeQueuesCallbackTimerImmediately) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  uint32_t shell = shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kCallbackStructAddr = 0x80002500;
  constexpr uint32_t kNotifyFnAddr = 0x00105000;
  constexpr uint32_t kNotifyData = 0x80009999;

  // AEECallback struct: +16 = pfnNotify, +20 = pNotifyData
  cpu.GetMemory().Write32(kCallbackStructAddr + 16, kNotifyFnAddr);
  cpu.GetMemory().Write32(kCallbackStructAddr + 20, kNotifyData);

  // Call IShell::Resume(shell, pCallback) (slot 36)
  uint32_t resume_fn = cpu.GetMemory().Read32(kVtableAddr + 36 * 4);
  EXPECT_EQ(hle.CallArmFunction(resume_fn, shell, kCallbackStructAddr), 0u);

  // Tick(0) should immediately return the expired timer
  auto expired = shell_hle.Tick(0);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].callback, kNotifyFnAddr);
  EXPECT_EQ(expired[0].user_data, kNotifyData);
}

TEST(IShellHle, SetTimerThenTickFiresAfterElapsedTimeReachesDeadline) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  // int SetTimer(IShell *ps, uint32 dwCount, PFNNOTIFY pfnNotify, void *pUser)
  EXPECT_EQ(hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData), 0u);

  EXPECT_TRUE(shell_hle.Tick(20).empty()) << "shouldn't fire before its deadline";
  auto expired = shell_hle.Tick(13);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].callback, kCallback);
  EXPECT_EQ(expired[0].user_data, kUserData);
  EXPECT_FALSE(expired[0].r0_override.has_value())
      << "a real ISHELL_SetTimer call keeps the standard PFNNOTIFY(pUser) firing convention";
  EXPECT_TRUE(shell_hle.Tick(1000).empty()) << "one-shot timers don't recur on their own";
}

TEST(IShellHle, ScheduleTimerWithR0OverrideCarriesItThroughToTheExpiredTimer) {
  // See ScheduleTimer's own doc comment: real evidence (Zeebo Sports
  // Tênis/Zeeboids) that this experimental registration path's real
  // callback shape takes a real first argument distinct from pUser,
  // not the plain ISHELL_SetTimer PFNNOTIFY(pUser) contract.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  constexpr uint32_t kR0Override = 0x00080000;
  shell_hle.ScheduleTimer(33, kCallback, kUserData, kR0Override);

  auto expired = shell_hle.Tick(33);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].callback, kCallback);
  EXPECT_EQ(expired[0].user_data, kUserData);
  ASSERT_TRUE(expired[0].r0_override.has_value());
  EXPECT_EQ(*expired[0].r0_override, kR0Override);
}

TEST(IShellHle, ReschedulingWithScheduleTimerUpdatesTheR0Override) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  shell_hle.ScheduleTimer(33, kCallback, kUserData, 0x00080000);
  shell_hle.ScheduleTimer(33, kCallback, kUserData, 0x00090000);  // real re-arm, same identity

  auto expired = shell_hle.Tick(33);
  ASSERT_EQ(expired.size(), 1u) << "re-arming shouldn't create a second pending timer";
  ASSERT_TRUE(expired[0].r0_override.has_value());
  EXPECT_EQ(*expired[0].r0_override, 0x00090000u);
}

TEST(IShellHle, SetTimerAgainWithSameCallbackReschedulesRatherThanDuplicates) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);

  EXPECT_EQ(shell_hle.Tick(33).size(), 1u) << "re-arming shouldn't create a second pending timer";
}

TEST(IShellHle, CancelTimerRemovesAMatchingPendingTimer) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  uint32_t cancel_timer = cpu.GetMemory().Read32(kVtableAddr + 12 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);

  // int CancelTimer(IShell *ps, PFNNOTIFY pfnNotify, void *pUser)
  EXPECT_EQ(hle.CallArmFunction(cancel_timer, kObjectAddr, kCallback, kUserData), 0u);
  EXPECT_TRUE(shell_hle.Tick(1000).empty());
}

TEST(IShellHle, CancelTimerFailsForNoMatchingTimer) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t cancel_timer = cpu.GetMemory().Read32(kVtableAddr + 12 * 4);
  EXPECT_EQ(hle.CallArmFunction(cancel_timer, kObjectAddr, /*pfn=*/0x1234, /*pUser=*/0x5678), 1u);
}

TEST(IShellHle, LoadResDataExWithSizeSentinelReportsRealSizeWithoutCopying) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  std::vector<uint8_t> resource = {1, 2, 3, 4, 5};
  shell_hle.RegisterResourceFile("resources.bar", BuildBarWithOneResource(1, 4000, resource));
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t name_addr = 0x90000;
  WriteAeeCharString(cpu.GetMemory(), name_addr, "resources.bar");
  constexpr uint32_t kLenAddr = 0x90100;
  cpu.GetMemory().Write32(kLenAddr, 0xDEADBEEF);
  constexpr uint32_t kSpAddr = 0x90200;
  cpu.SetRegister(zeebulator::kSP, kSpAddr);
  cpu.GetMemory().Write32(kSpAddr, 0xFFFFFFFF);  // real "-1" size-only sentinel
  cpu.GetMemory().Write32(kSpAddr + 4, kLenAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  // SDK AEEIShell.h: pBuffer == (void*)-1 devolve o proprio sentinel e escreve
  // o tamanho em pnLen. Nao e AEEResult.
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1),
            0xFFFFFFFFu);
  EXPECT_EQ(cpu.GetMemory().Read32(kLenAddr), 5u);
}

TEST(IShellHle, LoadResDataExWithARealBufferCopiesTheResourceBytes) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  std::vector<uint8_t> resource = {10, 20, 30, 40};
  shell_hle.RegisterResourceFile("resources.bar", BuildBarWithOneResource(1, 4000, resource));
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t name_addr = 0x90000;
  WriteAeeCharString(cpu.GetMemory(), name_addr, "resources.bar");
  constexpr uint32_t kBufferAddr = 0x90300;
  constexpr uint32_t kLenAddr = 0x90100;
  constexpr uint32_t kSpAddr = 0x90200;
  cpu.SetRegister(zeebulator::kSP, kSpAddr);
  cpu.GetMemory().Write32(kSpAddr, kBufferAddr);
  cpu.GetMemory().Write32(kSpAddr + 4, kLenAddr);
  cpu.GetMemory().Write32(kLenAddr, 4);  // capacidade de entrada, em bytes

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1),
            kBufferAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kLenAddr), 4u);
  for (uint32_t i = 0; i < resource.size(); ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kBufferAddr + i), resource[i]);
  }
}

TEST(IShellHle, LoadResDataExFailsForAnUnregisteredResourceFile) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t name_addr = 0x90000;
  WriteAeeCharString(cpu.GetMemory(), name_addr, "resources.bar");
  cpu.SetRegister(zeebulator::kSP, 0x90200);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1), 0u);
}

TEST(IShellHle, LoadResDataExRejectsASmallCallerBufferWithoutWritingPastIt) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.RegisterResourceFile("resources.bar", BuildBarWithOneResource(1, 4000, {10, 20, 30, 40}));
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kName = 0x90000, kLen = 0x90100, kSp = 0x90200, kBuf = 0x90300;
  WriteAeeCharString(cpu.GetMemory(), kName, "resources.bar");
  cpu.GetMemory().Write32(kBuf, 0xA5A5A5A5);  // sentinela: nao pode ser tocada
  cpu.GetMemory().Write32(kLen, 3);           // capacidade insuficiente para 4 bytes
  cpu.SetRegister(zeebulator::kSP, kSp);
  cpu.GetMemory().Write32(kSp, kBuf);
  cpu.GetMemory().Write32(kSp + 4, kLen);

  const uint32_t fn = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(fn, kObjectAddr, kName, 4000, 1), 0u);  // NULL
  EXPECT_EQ(cpu.GetMemory().Read32(kLen), 4u);       // tamanho necessario
  EXPECT_EQ(cpu.GetMemory().Read32(kBuf), 0xA5A5A5A5u);  // zero bytes copiados
}

TEST(IShellHle, LoadResDataExRejectsNullLengthPointer) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.RegisterResourceFile("resources.bar", BuildBarWithOneResource(1, 4000, {1}));
  shell_hle.Build(kVtableAddr, kObjectAddr);

  constexpr uint32_t kName = 0x90000, kSp = 0x90200, kBuf = 0x90300;
  WriteAeeCharString(cpu.GetMemory(), kName, "resources.bar");
  cpu.SetRegister(zeebulator::kSP, kSp);
  cpu.GetMemory().Write32(kSp, kBuf);
  cpu.GetMemory().Write32(kSp + 4, 0);  // pnBufSize e obrigatorio pelo SDK
  const uint32_t fn = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(fn, kObjectAddr, kName, 4000, 1), 0u);
}

TEST(IShellHle, LoadResDataExFailsForATypeIdPairNotInTheDirectory) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.RegisterResourceFile("resources.bar",
                                  BuildBarWithOneResource(1, 4000, {1, 2, 3}));
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t name_addr = 0x90000;
  WriteAeeCharString(cpu.GetMemory(), name_addr, "resources.bar");
  cpu.SetRegister(zeebulator::kSP, 0x90200);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/9999, /*type=*/1), 0u);
}

TEST(IDisplayHle, ResetToBlankPanelPresentsAWhiteScreen) {
  TestBackend backend;
  IDisplayHle display(backend, 8, 4);
  display.ResetToBlankPanel();

  // The console shows a cleared panel while a title starts, not a dead black
  // screen; the title paints its first frame over it.
  EXPECT_EQ(backend.push_count, 1);
  for (uint16_t pixel : display.LastPresentedFramebuffer()) {
    EXPECT_EQ(pixel, 0xFFFF);
  }
}

TEST(IDisplayHle, DrawTextThenUpdatePushesCorrectFrame) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  WriteAeeCharString(cpu.GetMemory(), 0x3000, "HI");

  // Stack args beyond R0-R3: x, y, prcBackground, dwFlags.
  cpu.SetRegister(zeebulator::kSP, 0x9000);
  cpu.GetMemory().Write32(0x9000, 10);  // x
  cpu.GetMemory().Write32(0x9004, 5);   // y
  cpu.GetMemory().Write32(0x9008, 0);   // prcBackground
  cpu.GetMemory().Write32(0x900C, 0);   // dwFlags

  uint32_t draw_text_sentinel = cpu.GetMemory().Read32(kVtableAddr + 4 * 4);
  hle.CallArmFunction(draw_text_sentinel, display_obj, /*nFont=*/0,
                       /*pcText=*/0x3000, /*nChars=*/static_cast<uint32_t>(-1));

  EXPECT_EQ(backend.push_count, 0) << "DrawText alone shouldn't push a frame";

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_width, 64);
  EXPECT_EQ(backend.last_height, 48);
  EXPECT_EQ(backend.last_format, PixelFormat::kRGB565);

  // "HI" is 2 real 5x7 glyphs on 6x8 cells starting at (10,5): H at
  // x=[10,14], I at x=[16,20], with a 1px blank spacing column at x=15.
  auto Px = [&](int x, int y) { return backend.last_frame[static_cast<size_t>(y) * 64 + x]; };
  EXPECT_EQ(Px(10, 5), 0xFFFFu) << "H top-left corner is set (H's top row is #...#)";
  EXPECT_EQ(Px(12, 5), 0u) << "H top-middle is clear (H's top row is #...#)";
  EXPECT_EQ(Px(10, 8), 0xFFFFu) << "H's crossbar row is set across the glyph";
  EXPECT_EQ(Px(12, 8), 0xFFFFu) << "H's crossbar row is set across the glyph";
  EXPECT_EQ(Px(15, 5), 0u) << "1px spacing column between glyphs stays clear";
  EXPECT_EQ(Px(16, 5), 0xFFFFu) << "I's top row is full (#####)";
  EXPECT_EQ(Px(18, 8), 0xFFFFu) << "I's centered stem is set on the crossbar row";
  EXPECT_EQ(Px(16, 8), 0u) << "I's stem is centered, not on the left edge";
  EXPECT_EQ(backend.last_frame[0], 0u) << "untouched pixel stays black";
}

TEST(IDisplayHle, DrawRectWithNullRectFillsWholeScreen) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 4, 3);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // void DrawRect(iname *po, const AEERect *pRect, RGBVAL clrFrame, RGBVAL clrFill, uint32 dwFlags)
  uint32_t draw_rect_sentinel = cpu.GetMemory().Read32(kVtableAddr + 5 * 4);
  hle.CallArmFunction(draw_rect_sentinel, display_obj, /*pRect=*/0, /*clrFrame=*/0,
                       /*clrFill=*/0x0000FF00);  // red: MAKE_RGB(255,0,0)

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  for (int i = 0; i < 4 * 3; ++i) {
    EXPECT_EQ(backend.last_frame[static_cast<size_t>(i)], 0xF800u) << "pixel " << i;  // RGB565 red
  }
}

TEST(IDisplayHle, DrawRectWithExplicitRectFillsOnlyThatArea) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 8, 8);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // Real AEERect: { int16 x, y, dx, dy; }
  constexpr uint32_t kRectAddr = 0x9000;
  cpu.GetMemory().Write16(kRectAddr + 0, 2);  // x
  cpu.GetMemory().Write16(kRectAddr + 2, 1);  // y
  cpu.GetMemory().Write16(kRectAddr + 4, 3);  // dx
  cpu.GetMemory().Write16(kRectAddr + 6, 2);  // dy

  uint32_t draw_rect_sentinel = cpu.GetMemory().Read32(kVtableAddr + 5 * 4);
  hle.CallArmFunction(draw_rect_sentinel, display_obj, kRectAddr, /*clrFrame=*/0,
                       /*clrFill=*/0x00FF0000);  // green: MAKE_RGB(0,255,0)

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[1 * 8 + 2], 0x07E0u) << "inside the rect";  // RGB565 green
  EXPECT_EQ(backend.last_frame[1 * 8 + 4], 0x07E0u) << "still inside (x=4 < 2+3)";
  EXPECT_EQ(backend.last_frame[1 * 8 + 5], 0u) << "outside the rect (x=5 >= 2+3)";
  EXPECT_EQ(backend.last_frame[0], 0u) << "untouched pixel stays black";
}

TEST(IDisplayHle, BlitRgbaCompositesOpaqueTexelsAndSkipsTransparentOnes) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 4, 4);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // 2x2 RGBA source: opaque red, opaque green, transparent (alpha=0,
  // should be skipped, leaving the destination pixel untouched), opaque
  // blue.
  const uint8_t rgba[2 * 2 * 4] = {
      255, 0,   0,   255,  // (0,0) red, opaque
      0,   255, 0,   255,  // (1,0) green, opaque
      12,  34,  56,  0,    // (0,1) transparent -- color should be ignored
      0,   0,   255, 255,  // (1,1) blue, opaque
  };
  display.BlitRgba(/*x=*/1, /*y=*/1, /*w=*/2, /*h=*/2, rgba);

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[1 * 4 + 1], 0xF800u) << "(1,1) red";
  EXPECT_EQ(backend.last_frame[1 * 4 + 2], 0x07E0u) << "(2,1) green";
  EXPECT_EQ(backend.last_frame[2 * 4 + 1], 0u) << "(1,2) transparent texel leaves destination untouched";
  EXPECT_EQ(backend.last_frame[2 * 4 + 2], 0x001Fu) << "(2,2) blue";
}

TEST(IDisplayHle, BlitRgbaClipsToDisplayBoundsWithoutCrashing) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 4, 4);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // 2x2 fully-opaque white source, positioned so it straddles the
  // bottom-right corner of a 4x4 display -- half the texels land
  // outside the real framebuffer bounds.
  const uint8_t rgba[2 * 2 * 4] = {
      255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255,
  };
  display.BlitRgba(/*x=*/3, /*y=*/3, /*w=*/2, /*h=*/2, rgba);

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[3 * 4 + 3], 0xFFFFu) << "the one real in-bounds texel still draws";
}

TEST(IDisplayHle, SetColorChangesDrawTextColorAndReturnsPrevious) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  uint32_t set_color_sentinel = cpu.GetMemory().Read32(kVtableAddr + 10 * 4);
  // RGBVAL SetColor(iname *po, AEEClrItem clr, RGBVAL rgb)
  uint32_t previous =
      hle.CallArmFunction(set_color_sentinel, display_obj, /*clr=*/0, /*rgb=*/0xFF000000);  // blue: MAKE_RGB(0,0,255)
  EXPECT_EQ(previous, 0xFFFFFF00u) << "default color is white before any SetColor call";

  WriteAeeCharString(cpu.GetMemory(), 0x3000, "H");
  cpu.SetRegister(zeebulator::kSP, 0x9000);
  cpu.GetMemory().Write32(0x9000, 0);  // x
  cpu.GetMemory().Write32(0x9004, 0);  // y
  cpu.GetMemory().Write32(0x9008, 0);  // prcBackground
  cpu.GetMemory().Write32(0x900C, 0);  // dwFlags
  uint32_t draw_text_sentinel = cpu.GetMemory().Read32(kVtableAddr + 4 * 4);
  hle.CallArmFunction(draw_text_sentinel, display_obj, /*nFont=*/0, /*pcText=*/0x3000,
                       /*nChars=*/static_cast<uint32_t>(-1));

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[0], 0x001Fu) << "drawn glyph uses the newly-set blue color";
}

TEST(IDisplayHle, GetDeviceBitmapWritesTheRegisteredInstanceAndReturnsSuccess) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);
  constexpr uint32_t kBitmapObj = 0x8000F000;
  display.SetDeviceBitmapInstance(kBitmapObj);

  // int GetDeviceBitmap(IDisplay *pIDisplay, IBitmap **ppBitmap)
  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 16 * 4);
  constexpr uint32_t kPpBitmapAddr = 0x9000;
  cpu.GetMemory().Write32(kPpBitmapAddr, 0xDEADBEEF);
  EXPECT_EQ(hle.CallArmFunction(sentinel, display_obj, kPpBitmapAddr), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(kPpBitmapAddr), kBitmapObj);
}

TEST(IDisplayHle, CreateDIBitmapAllocatesUsableOffscreenDib) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);
  // Give the display a dedicated DIB arena.
  constexpr uint32_t kArenaBase = 0x80040000;
  display.SetDibArena(kArenaBase, 0x10000);

  uint32_t create_fn = cpu.GetMemory().Read32(kVtableAddr + 13 * 4);
  constexpr uint32_t kPpOut = 0x80030000;
  cpu.GetMemory().Write32(kPpOut, 0);

  // CreateDIBitmap(display, &out, depth=16, width=8, height=4)
  // width in R3, height on stack arg 0 (at SP+0, per the HLE ABI).
  constexpr uint32_t kStackAddr = 0x80002000;
  cpu.SetRegister(zeebulator::kSP, kStackAddr);
  cpu.GetMemory().Write32(kStackAddr + 0, 4);  // height
  uint32_t rc = hle.CallArmFunction(create_fn, display_obj, kPpOut, 16, 8);
  EXPECT_EQ(rc, 0u);  // AEE_SUCCESS

  uint32_t dib_obj = cpu.GetMemory().Read32(kPpOut);
  EXPECT_NE(dib_obj, 0u);

  // The returned object must expose a working GetInfo (slot 12 of BitmapHle):
  // read the DIB's own vtable and query dimensions.
  uint32_t dib_vtable = cpu.GetMemory().Read32(dib_obj);
  EXPECT_GE(dib_obj, dib_vtable + 16u * 4u);  // vtable inteira fora do IDIB
  uint32_t get_info_fn = cpu.GetMemory().Read32(dib_vtable + 12 * 4);
  constexpr uint32_t kInfoStruct = 0x80031000;
  hle.CallArmFunction(get_info_fn, dib_obj, kInfoStruct, 12);
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 0), 8u);   // width
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 4), 4u);   // height
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 8), 16u);  // depth
}

TEST(IDisplayHle, CreateDIBitmapWithoutArenaFails) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);
  uint32_t create_fn = cpu.GetMemory().Read32(kVtableAddr + 13 * 4);
  constexpr uint32_t kPpOut = 0x80030000;
  cpu.GetMemory().Write32(kPpOut, 0xDEADBEEF);
  uint32_t rc = hle.CallArmFunction(create_fn, display_obj, kPpOut, 16, 8);
  EXPECT_NE(rc, 0u);                               // failure
  EXPECT_EQ(cpu.GetMemory().Read32(kPpOut), 0u);   // output nulled
}

TEST(IDisplayHle, CreateDIBitmapExAllocatesUsableOffscreenDib) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);
  display.SetDibArena(0x80040000, 0x10000);

  uint32_t create_fn = cpu.GetMemory().Read32(kVtableAddr + 24 * 4);
  constexpr uint32_t kPpOut = 0x80030000;
  cpu.GetMemory().Write32(kPpOut, 0);

  // CreateDIBitmapEx(display, &out, depth=16, height=6, width=10,
  //                  paletteEntries=0, extraBytes=0)
  // R2=depth, R3=height; stack[0]=width, stack[1]=palette, stack[2]=extra.
  constexpr uint32_t kStackAddr = 0x80002000;
  cpu.SetRegister(zeebulator::kSP, kStackAddr);
  cpu.GetMemory().Write32(kStackAddr + 0, 10);  // width
  cpu.GetMemory().Write32(kStackAddr + 4, 0);   // paletteEntries
  cpu.GetMemory().Write32(kStackAddr + 8, 0);   // extraBytes
  uint32_t rc = hle.CallArmFunction(create_fn, display_obj, kPpOut, 16, 6);
  EXPECT_EQ(rc, 0u);  // AEE_SUCCESS

  uint32_t dib_obj = cpu.GetMemory().Read32(kPpOut);
  EXPECT_NE(dib_obj, 0u);

  uint32_t dib_vtable = cpu.GetMemory().Read32(dib_obj);
  EXPECT_GE(dib_obj, dib_vtable + 16u * 4u);  // vtable inteira fora do IDIB
  uint32_t get_info_fn = cpu.GetMemory().Read32(dib_vtable + 12 * 4);
  constexpr uint32_t kInfoStruct = 0x80031000;
  hle.CallArmFunction(get_info_fn, dib_obj, kInfoStruct, 12);
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 0), 10u);  // width
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 4), 6u);   // height
  EXPECT_EQ(cpu.GetMemory().Read32(kInfoStruct + 8), 16u);  // depth
}

TEST(IDisplayHle, SetDestinationAndGetDestinationRouteCorrectly) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);
  constexpr uint32_t kDeviceBmp = 0x8000F000;
  display.SetDeviceBitmapInstance(kDeviceBmp);

  uint32_t set_dst = cpu.GetMemory().Read32(kVtableAddr + 14 * 4);
  uint32_t get_dst = cpu.GetMemory().Read32(kVtableAddr + 15 * 4);

  // Default: GetDestination returns device bitmap when none set.
  EXPECT_EQ(hle.CallArmFunction(get_dst, display_obj), kDeviceBmp);

  // SetDestination to an offscreen bitmap keeps that object active.
  constexpr uint32_t kOffscreen = 0x80020000;
  EXPECT_EQ(hle.CallArmFunction(set_dst, display_obj, kOffscreen), 0u);
  EXPECT_EQ(hle.CallArmFunction(get_dst, display_obj), kOffscreen);

  // SetDestination(NULL) resets to the device bitmap.
  EXPECT_EQ(hle.CallArmFunction(set_dst, display_obj, 0u), 0u);
  EXPECT_EQ(hle.CallArmFunction(get_dst, display_obj), kDeviceBmp);

  // IsEnabled (slot 22) always reports the display is enabled.
  uint32_t is_enabled = cpu.GetMemory().Read32(kVtableAddr + 22 * 4);
  EXPECT_EQ(hle.CallArmFunction(is_enabled, display_obj), 1u);
}

TEST(IDisplayHle, SetClipRectAndGetClipRectWorkCorrectly) {
  TestBackend backend;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IDisplayHle display(backend, 640, 480);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  constexpr uint32_t kRectAddr = 0x80002000;
  // Write test rect (x=10, y=20, dx=100, dy=200)
  cpu.GetMemory().Write16(kRectAddr + 0, 10);
  cpu.GetMemory().Write16(kRectAddr + 2, 20);
  cpu.GetMemory().Write16(kRectAddr + 4, 100);
  cpu.GetMemory().Write16(kRectAddr + 6, 200);

  // Call SetClipRect(display, &rect) (slot 18)
  uint32_t set_clip_fn = cpu.GetMemory().Read32(kVtableAddr + 18 * 4);
  EXPECT_EQ(hle.CallArmFunction(set_clip_fn, display_obj, kRectAddr), 0u);

  // Read back via GetClipRect (slot 19) into new addr
  constexpr uint32_t kOutRectAddr = 0x80002100;
  uint32_t get_clip_fn = cpu.GetMemory().Read32(kVtableAddr + 19 * 4);
  EXPECT_EQ(hle.CallArmFunction(get_clip_fn, display_obj, kOutRectAddr), 0u);

  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 0), 10);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 2), 20);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 4), 100);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 6), 200);

  // Set null rect restores full screen bounds
  EXPECT_EQ(hle.CallArmFunction(set_clip_fn, display_obj, 0u), 0u);
  EXPECT_EQ(hle.CallArmFunction(get_clip_fn, display_obj, kOutRectAddr), 0u);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 0), 0);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 2), 0);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 4), 640);
  EXPECT_EQ(cpu.GetMemory().Read16(kOutRectAddr + 6), 480);
}

TEST(IDisplayHle, BitBltDrawsSourceBitmapToFramebuffer) {
  TestBackend backend;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IDisplayHle display(backend, 8, 8);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // Set up source bitmap 4x4 RGB565 with a solid color
  constexpr uint32_t kSrcBmpObj = 0x80003000;
  constexpr uint32_t kSrcBmpBuf = 0x80004000;
  // Fill source buffer with red (0xF800)
  for (int i = 0; i < 4 * 4; ++i) {
    cpu.GetMemory().Write16(kSrcBmpBuf + i * 2, 0xF800);
  }
  // Setup DIB struct
  cpu.GetMemory().Write32(kSrcBmpObj + 0, 0);       // vtable
  cpu.GetMemory().Write32(kSrcBmpObj + 8, kSrcBmpBuf); // pBmp
  cpu.GetMemory().Write32(kSrcBmpObj + 16, 0);     // transparent color
  cpu.GetMemory().Write16(kSrcBmpObj + 20, 4);     // cx
  cpu.GetMemory().Write16(kSrcBmpObj + 22, 4);     // cy
  cpu.GetMemory().Write16(kSrcBmpObj + 24, 8);     // pitch (4 * 2)
  cpu.GetMemory().Write8(kSrcBmpObj + 28, 16);     // depth

  // BitBlt slot 6: args = display, xDest=2, yDest=2, cxDest=4; stack: cyDest=4, pSrc=kSrcBmpObj, xSrc=0, ySrc=0, rop=0
  uint32_t bitblt_fn = cpu.GetMemory().Read32(kVtableAddr + 6 * 4);
  constexpr uint32_t kStackAddr = 0x80002000;
  cpu.SetRegister(zeebulator::kSP, kStackAddr);
  cpu.GetMemory().Write32(kStackAddr + 0, 4);           // cyDest
  cpu.GetMemory().Write32(kStackAddr + 4, kSrcBmpObj);   // pSrc
  cpu.GetMemory().Write32(kStackAddr + 8, 0);           // xSrc
  cpu.GetMemory().Write32(kStackAddr + 12, 0);          // ySrc
  cpu.GetMemory().Write32(kStackAddr + 16, 0);          // rop

  uint32_t res = hle.CallArmFunction(bitblt_fn, display_obj, 2, 2, 4);
  EXPECT_EQ(res, 0u);

  // Check framebuffer pixel at (2,2) is red
  const auto& fb = display.LiveFramebuffer();
  EXPECT_EQ(fb[2 * 8 + 2], 0xF800);
  EXPECT_EQ(fb[0 * 8 + 0], 0x0000);  // Unwritten pixel
}

TEST(IDisplayHle, ObjectAddressPointsAtVtable) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  EXPECT_EQ(obj, kObjectAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kObjectAddr), kVtableAddr)
      << "object header's first word must point at the vtable";
}

// --- Posse do bitmap devolvido por GetDestination/GetDeviceBitmap ------------
//
// O codigo do proprio SDK da Qualcomm (utgifviewer.c, UTest_Enter) chama as
// duas funcoes e da IBITMAP_Release em cada resultado, sem AddRef nenhum no
// meio. Logo quem chama RECEBE uma referencia. Devolver sem AddRef faz cada
// chamada tirar uma referencia que ninguem pos -- medido na Z-Wheel com um
// contador real no bitmap do dispositivo: 82 movimentos, todos Release,
// contador terminando em -81.
//
// Estes testes seguram a correcao no lugar. Eles contam quantas vezes o slot 0
// (AddRef) do objeto devolvido e realmente chamado.
namespace {

// Monta um IBitmap falso cujo AddRef incrementa um contador visivel ao teste.
struct BitmapEspiao {
  static constexpr uint32_t kVtable = 0x80050000;
  static constexpr uint32_t kObject = 0x80051000;
  int addrefs = 0;

  void Instalar(zeebulator::Memory& memory, zeebulator::HleRuntime& hle) {
    std::vector<zeebulator::HleRuntime::HleFunction> slots(
        20, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
    slots[0] = [this](zeebulator::IArmCore& c) {
      ++addrefs;
      c.SetRegister(zeebulator::kR0, static_cast<uint32_t>(addrefs));
    };
    zeebulator::BuildInterfaceObject(memory, hle, kVtable, kObject, slots);
  }
};

}  // namespace

TEST(IDisplayHle, GetDestinationAddRefsTheBitmapItHandsBack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  TestBackend backend;
  IDisplayHle display(backend, 8, 4);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, 0x80002000, 0x80003000);

  BitmapEspiao espiao;
  espiao.Instalar(cpu.GetMemory(), hle);
  display.SetDeviceBitmapInstance(BitmapEspiao::kObject);

  const uint32_t slot_get_destination =
      cpu.GetMemory().Read32(0x80002000 + 15 * 4);
  hle.CallArmFunction(slot_get_destination, display_obj);

  EXPECT_EQ(espiao.addrefs, 1)
      << "GetDestination devolveu o bitmap sem AddRef: quem chamar vai dar "
         "Release numa referencia que ninguem pos";
}

TEST(IDisplayHle, GetDeviceBitmapAddRefsTheBitmapItHandsBack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  TestBackend backend;
  IDisplayHle display(backend, 8, 4);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, 0x80002000, 0x80003000);

  BitmapEspiao espiao;
  espiao.Instalar(cpu.GetMemory(), hle);
  display.SetDeviceBitmapInstance(BitmapEspiao::kObject);

  // Slot 16 = GetDeviceBitmap, logo depois do GetDestination (15).
  const uint32_t slot_get_device_bitmap =
      cpu.GetMemory().Read32(0x80002000 + 16 * 4);
  constexpr uint32_t kOut = 0x00090000;
  hle.CallArmFunction(slot_get_device_bitmap, display_obj, kOut);

  EXPECT_EQ(cpu.GetMemory().Read32(kOut), BitmapEspiao::kObject);
  EXPECT_EQ(espiao.addrefs, 1) << "GetDeviceBitmap devolveu o bitmap sem AddRef";
}
