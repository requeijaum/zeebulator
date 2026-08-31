#include <cstring>
#include <vector>

#include <gtest/gtest.h>

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
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, /*cls=*/0x1234, kPpObjAddr), 1u);
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

TEST(IShellHle, Slot43FirstCallReturnsTheConfirmedRealLiteral35) {
  // Real, confirmed return value -- see IShellHle::Build's own doc
  // comment on slot 43 for the full derivation. Real Alien Breaker
  // Deluxe disassembly requires exactly 35 (not just nonzero/success)
  // from this call's *first* real call site to proceed past a real
  // bail-out branch gating a real object-pointer field this project's
  // own live tracing confirmed otherwise stays null and crashes a
  // later real call.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 35u);
}

TEST(IShellHle, Slot43SecondCallOnTheSameObjectReturnsZero) {
  // Real, confirmed stateful/polling contract -- see IShellHle::Build's
  // own doc comment on slot 43. Real code calls this exact slot a
  // *second* time on the same object with the same arguments, deep
  // inside the real success path the first call's fix unblocked, and
  // needs 0 back (not 35 again) to take its own further real branch.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 35u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 0u);
}

TEST(IShellHle, Slot43CallCountIsTrackedPerObject) {
  // A second, distinct real object hitting this same slot starts its
  // own real lazy-init sequence fresh -- the toggle this fix adds is
  // keyed per-object (real `this`/R0), not a single global one, so two
  // independently-initializing real objects can't stomp each other's
  // state.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);
  constexpr uint32_t kOtherObjectAddr = 0x80002000;

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 35u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kOtherObjectAddr, 0), 35u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 0u);
}

TEST(IShellHle, Slot43AlternatesRatherThanLatchingAfterTheFirstCall) {
  // Real Alien Breaker Deluxe disassembly runs this exact real lazy-
  // init sequence back-to-back for many distinct real objects, on the
  // same shared shell object, all with the same arguments -- see
  // IShellHle::Build's own doc comment on slot 43 for the full
  // derivation. A "35 once, then 0 forever" latch (an earlier version
  // of this fix) answers the first real object's own sequence
  // correctly but starves every later one of ever seeing 35 again;
  // this call's own real contract is a strict odd/even toggle instead,
  // so every subsequent pair of calls gets its own fresh 35-then-0
  // answer, matching real, confirmed behavior for at least two
  // real, independent per-object sequences on the same object.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 35u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 0u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 35u);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, 0), 0u);
}

TEST(IShellHle, Slot43WritesTheSmallRealThirdArgOutParamValue) {
  // Real, confirmed dual-condition gate -- see IShellHle::Build's own
  // doc comment on slot 43. Confirmed live: leaving the real 3rd
  // argument (a real out-param) unwritten still hits the same real
  // bail-out branch one instruction later, even with the correct
  // return value, since real code checks `*pOut != 0` as a second,
  // separate condition. Writes the small constant 1, not a pointer --
  // confirmed live that *pOut later flows unmodified into a real raw
  // unsigned comparison against a real handle value, so a large
  // "safe, always-valid object address" choice (an earlier version of
  // this fix) permanently fails that later comparison instead.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 43 * 4);
  constexpr uint32_t kOutAddr = 0x90000;
  cpu.GetMemory().Write32(kOutAddr, 0);
  hle.CallArmFunction(sentinel, kObjectAddr, 0, kOutAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kOutAddr), 1u);
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
  // int LoadResDataEx(IShell*, const char *pszResFile, uint16 wResID,
  //   AEERESTYPE resType, void *pBuffer, uint32 *pnLen)
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1), 0u);
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

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 41 * 4);
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1), 0u);
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
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/4000, /*type=*/1), 1u);
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
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, name_addr, /*id=*/9999, /*type=*/1), 1u);
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
                       /*clrFill=*/0x00FF0000);  // red

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
                       /*clrFill=*/0x0000FF00);  // green

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
      hle.CallArmFunction(set_color_sentinel, display_obj, /*clr=*/0, /*rgb=*/0x000000FF);  // blue
  EXPECT_EQ(previous, 0x00FFFFFFu) << "default color is white before any SetColor call";

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
