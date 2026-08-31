#include "core/brew/hid_hle.h"

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HidHle;
using zeebulator::HleRuntime;
using zeebulator::ZPadState;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kHidVtable = 0x80000000;
constexpr uint32_t kHidObject = 0x80001000;
constexpr uint32_t kDeviceVtable = 0x80002000;
constexpr uint32_t kDeviceObject = 0x80003000;
constexpr uint32_t kScratch = 0x00090000;

// Real vtable slot indices, matching core/brew/hid_hle.cpp's own
// verified real ordering.
enum HidSlot { kHidCreateDevice = 3, kHidGetConnectedDevices = 7 };
enum DeviceSlot { kDeviceRegisterForButtonEvent = 8, kDeviceGetNextButtonEvent = 9 };

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  HidHle hid_hle{cpu.GetMemory(), hle};
  uint32_t hid;

  Fixture() { hid = hid_hle.Build(kHidVtable, kHidObject, kDeviceVtable, kDeviceObject); }

  uint32_t HidSlotAddr(HidSlot slot) { return cpu.GetMemory().Read32(kHidVtable + slot * 4); }
  uint32_t DeviceSlotAddr(DeviceSlot slot) {
    return cpu.GetMemory().Read32(kDeviceVtable + slot * 4);
  }
};

}  // namespace

TEST(HidHle, BuildReturnsTheHidObjectAddress) {
  Fixture f;
  EXPECT_EQ(f.hid, kHidObject);
}

TEST(HidHle, CreateDeviceWritesTheRealDeviceObjectAddress) {
  Fixture f;
  uint32_t ppdevice = kScratch;
  f.hle.CallArmFunction(f.HidSlotAddr(kHidCreateDevice), f.hid, /*nHandle=*/1, ppdevice);
  EXPECT_EQ(f.cpu.GetMemory().Read32(ppdevice), kDeviceObject);
}

TEST(HidHle, GetConnectedDevicesReportsExactlyOneSimulatedDevice) {
  Fixture f;
  uint32_t handles_addr = kScratch;
  uint32_t len_req_addr = kScratch + 0x100;
  f.cpu.SetRegister(zeebulator::kSP, kScratch + 0x200);
  f.cpu.GetMemory().Write32(kScratch + 0x200, len_req_addr);  // 5th arg, stack-passed

  uint32_t result = f.hle.CallArmFunction(f.HidSlotAddr(kHidGetConnectedDevices), f.hid,
                                           /*nDeviceType=*/0, handles_addr, /*len=*/4);
  EXPECT_EQ(result, 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(handles_addr), 1u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(len_req_addr), 1u);
}

TEST(HidHle, RegisterForButtonEventReturnsSuccess) {
  Fixture f;
  uint32_t result =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceRegisterForButtonEvent), kDeviceObject);
  EXPECT_EQ(result, 0u);
}

TEST(HidHle, GetNextButtonEventOnEmptyQueueFails) {
  Fixture f;
  uint32_t result =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, kScratch);
  EXPECT_EQ(result, 1u);
}

TEST(HidHle, PressingADpadButtonEnqueuesARealButtonInfoStruct) {
  Fixture f;
  ZPadState state;
  state.buttons = ZPadState::kDpadUp;
  f.hid_hle.UpdateState(state);

  uint32_t info = kScratch;
  uint32_t result =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, info);
  EXPECT_EQ(result, 0u);

  auto& mem = f.cpu.GetMemory();
  EXPECT_EQ(static_cast<int32_t>(mem.Read32(info + 0)), 0) << "nButtonID: index into the table";
  EXPECT_EQ(mem.Read32(info + 4), 1u) << "nState: pressed";
  EXPECT_EQ(static_cast<int32_t>(mem.Read32(info + 8)), 0x0106C3FE)
      << "nButtonUID: real confirmed DPAD_UP UID";
  EXPECT_EQ(mem.Read32(info + 12), 0u) << "nButtonMin";
  EXPECT_EQ(mem.Read32(info + 16), 1u) << "nButtonMax";
}

TEST(HidHle, ReleasingAButtonEnqueuesAReleaseEvent) {
  Fixture f;
  ZPadState pressed;
  pressed.buttons = ZPadState::kButtonSouth;
  f.hid_hle.UpdateState(pressed);
  f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, kScratch);

  ZPadState released;  // buttons back to 0
  f.hid_hle.UpdateState(released);
  uint32_t result =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, kScratch);
  EXPECT_EQ(result, 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(kScratch + 4), 0u) << "nState: released";
}

TEST(HidHle, MultipleSimultaneousButtonChangesQueueInTableOrder) {
  Fixture f;
  ZPadState state;
  state.buttons = ZPadState::kDpadRight | ZPadState::kShoulderL;
  f.hid_hle.UpdateState(state);

  uint32_t info = kScratch;
  uint32_t r1 =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, info);
  ASSERT_EQ(r1, 0u);
  int32_t first_uid = static_cast<int32_t>(f.cpu.GetMemory().Read32(info + 8));

  uint32_t r2 =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, info);
  ASSERT_EQ(r2, 0u);
  int32_t second_uid = static_cast<int32_t>(f.cpu.GetMemory().Read32(info + 8));

  EXPECT_EQ(first_uid, 0x0106C401) << "DPAD_RIGHT: earlier in the button table";
  EXPECT_EQ(second_uid, 0x0106C406) << "SHOULDER_L: later in the button table";

  uint32_t r3 =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, info);
  EXPECT_EQ(r3, 1u) << "no more events queued";
}

TEST(HidHle, HoldingAButtonAcrossUpdateStateCallsDoesNotReQueue) {
  Fixture f;
  ZPadState state;
  state.buttons = ZPadState::kButtonNorth;
  f.hid_hle.UpdateState(state);
  f.hid_hle.UpdateState(state);  // still held, no new edge
  f.hid_hle.UpdateState(state);

  f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, kScratch);
  uint32_t result =
      f.hle.CallArmFunction(f.DeviceSlotAddr(kDeviceGetNextButtonEvent), kDeviceObject, kScratch);
  EXPECT_EQ(result, 1u) << "only one edge, so only one event, regardless of how many "
                           "UpdateState calls saw it held";
}
