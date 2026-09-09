#include "core/brew/nid_table.h"

#include <gtest/gtest.h>

#include <string>

#include "core/brew/hle_runtime.h"
#include "core/brew/interface_object.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::BuildInterfaceObjectLabeled;
using zeebulator::DescribeClsid;
using zeebulator::HleRuntime;
using zeebulator::KnownClassName;

namespace {

// --- NID name table (Vita3K-style declarative class-id -> name) ---

TEST(NidTable, KnownClassIdsResolveToStableNames) {
  EXPECT_STREQ(KnownClassName(0x01001001u), "AEECLSID_DISPLAY");
  EXPECT_STREQ(KnownClassName(0x01005500u), "AEECLSID_MEDIA");
  EXPECT_STREQ(KnownClassName(0x01014bc3u), "AEECLSID_GL");
  // The ABD wall object and its real applet id are named -- the whole
  // point of the table for Phase 9d.
  EXPECT_STREQ(KnownClassName(0x0103d8ecu), "AEECLSID_QEGL");
  EXPECT_STREQ(KnownClassName(0x0108e356u), "APP_ALIEN_BREAKER_DELUXE");
}

TEST(NidTable, UnknownClassIdReturnsNullptr) {
  EXPECT_EQ(KnownClassName(0xdeadbeefu), nullptr);
  EXPECT_EQ(KnownClassName(0x00000000u), nullptr);
}

TEST(NidTable, DescribeClsidNamesKnownAndFlagsUnknown) {
  EXPECT_EQ(DescribeClsid(0x01001001u), "AEECLSID_DISPLAY (0x01001001)");
  EXPECT_EQ(DescribeClsid(0xdeadbeefu), "UNKNOWN (0xdeadbeef)");
}

// --- Per-slot labeling in HleRuntime (drives the unimplemented-slot logger) ---

TEST(NidLabeledSlots, LabeledBuildAttachesPerSlotLabels) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);

  std::vector<HleRuntime::HleFunction> methods(
      4, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
  const std::vector<const char*> slot_names = {"AddRef", "Release", nullptr,
                                               "DrawGeometry"};

  BuildInterfaceObjectLabeled(cpu.GetMemory(), hle, /*vtable=*/0x80000000,
                              /*object=*/0x80001000, methods, "TEST_IFACE",
                              slot_names);

  // vtable slot i holds the sentinel address for method i; the label the
  // logger would print for that address must name the interface+slot.
  uint32_t s0 = cpu.GetMemory().Read32(0x80000000 + 0 * 4);
  uint32_t s2 = cpu.GetMemory().Read32(0x80000000 + 2 * 4);
  uint32_t s3 = cpu.GetMemory().Read32(0x80000000 + 3 * 4);
  EXPECT_EQ(hle.LabelForAddress(s0), "TEST_IFACE::slot0 AddRef");
  EXPECT_EQ(hle.LabelForAddress(s2), "TEST_IFACE::slot2");  // no method name
  EXPECT_EQ(hle.LabelForAddress(s3), "TEST_IFACE::slot3 DrawGeometry");
}

TEST(NidLabeledSlots, UnlabeledRegisterHasEmptyLabel) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  uint32_t addr = hle.Register([](zeebulator::IArmCore&) {});
  EXPECT_TRUE(hle.LabelForAddress(addr).empty());
  // Out-of-range / below trap base yields empty, never a crash.
  EXPECT_TRUE(hle.LabelForAddress(0x00000000u).empty());
  EXPECT_TRUE(hle.LabelForAddress(0xFFFF0000u).empty());
}

TEST(NidLabeledSlots, LabeledDispatchStillRunsTheHandler) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);

  bool called = false;
  std::vector<HleRuntime::HleFunction> methods = {
      [&](zeebulator::IArmCore& c) {
        called = true;
        c.SetRegister(zeebulator::kR0, 42);
      }};
  BuildInterfaceObjectLabeled(cpu.GetMemory(), hle, 0x80000000, 0x80001000,
                              methods, "TEST_IFACE", {"Only"});
  uint32_t sentinel = cpu.GetMemory().Read32(0x80000000);

  // BX LR at 0x1000 lets CallArmFunction return after the trap fires.
  uint32_t r = hle.CallArmFunction(sentinel);
  EXPECT_TRUE(called);
  EXPECT_EQ(r, 42u);
}

}  // namespace
