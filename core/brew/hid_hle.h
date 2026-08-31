#pragma once

#include <cstdint>
#include <deque>

#include "core/backend.h"
#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Real Zeebo `IHID`/`IHIDDevice` HLE (`AEEIHID.h`/`AEEIHIDDevice.h`,
// confirmed against the real bundled SDK headers under
// research/docs/sdk_installer_extract/ -- see TASKS.md Phase 8 for the
// full derivation, done against Double Dragon's real gamepad-init code
// and cross-referenced with the real bundled sample source
// research/samples/conftest_source/conftest/GamepadMgr.c).
//
// Reports exactly one simulated connected device -- honest about being
// simulated (no real controller-enumeration hardware exists here), but
// real games' own gamepad-init code (confirmed for Double Dragon) waits
// for at least one real device before proceeding, so reporting zero
// (this project's original, "more honest" answer) blocks real titles
// that reporting one does not.
//
// Only `RegisterForButtonEvent`(8)/`GetNextButtonEvent`(9) -- the two
// methods a real game call site was found directly exercising, driving
// a real, confirmed state-machine transition end to end -- have real
// behavior. Every other real vtable slot (including the analog-stick
// `GetPositionState` family) is a safe no-op stub, following this
// project's established convention for a real, present-but-unconfirmed
// slot (see IDisplayHle/IShellHle) rather than guessing an unverified
// calling convention.
class HidHle {
 public:
  HidHle(Memory& memory, HleRuntime& hle);

  // Builds the one simulated `IHIDDevice` instance and the top-level
  // `IHID` instance that hands it out (`CreateDevice`/
  // `GetConnectedDevices`). Returns the `IHID*` value the caller should
  // register against the real `AEECLSID_HID` ClsId (`0x0106c411`) with
  // `IShellHle::RegisterInstance`.
  uint32_t Build(uint32_t hid_vtable_address, uint32_t hid_object_address,
                 uint32_t device_vtable_address, uint32_t device_object_address);

  // Diffs `state` against whatever was last passed here (all-zero
  // buttons the first call) and enqueues one real `AEEHIDButtonInfo`-
  // shaped press/release event per button bit that changed, using the
  // real, header-confirmed UID for that physical button (see the
  // `.cpp` for the real UID table). Meant to be called once per real
  // frame/tick by whichever frontend owns the main loop, mirroring
  // `Backend::PollInput()`'s own per-frame contract.
  void UpdateState(const ZPadState& state);

 private:
  void RegisterForButtonEventImpl(IArmCore& core);
  void GetNextButtonEventImpl(IArmCore& core);
  void CreateDeviceImpl(IArmCore& core);
  void GetConnectedDevicesImpl(IArmCore& core);

  // {nButtonID, nState, nButtonUID} -- matches the subset of real
  // AEEHIDButtonInfo fields that vary per event; nButtonMin/nButtonMax
  // are always 0/1 for a simple digital button, written directly in
  // GetNextButtonEventImpl.
  struct ButtonEvent {
    int32_t button_id;
    int32_t state;
    int32_t button_uid;
  };

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t device_object_address_ = 0;
  uint16_t last_buttons_ = 0;
  std::deque<ButtonEvent> event_queue_;
};

}  // namespace zeebulator
