#include "core/brew/hid_hle.h"

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

// Opaque device handle real code only ever passes back into
// IHID_CreateDevice/GetDeviceInfo, never interprets directly -- any
// stable nonzero value works. Matches this project's original,
// experimental game_probe.cpp finding.
constexpr uint32_t kSimulatedDeviceHandle = 1;

// Real Zeebo Z-Pad button UIDs, confirmed against the bundled real
// AEEHIDButtons.h (research/docs/sdk_installer_extract/) -- see
// core/backend.h for the ZPadState bit each one corresponds to.
constexpr int32_t kUidDpadUp = 0x0106C3FE;
constexpr int32_t kUidDpadLeft = 0x0106C3FF;
constexpr int32_t kUidDpadDown = 0x0106C400;
constexpr int32_t kUidDpadRight = 0x0106C401;
constexpr int32_t kUidStartHome = 0x0106C402;
constexpr int32_t kUidShoulderL = 0x0106C406;
constexpr int32_t kUidShoulderR = 0x0106C408;
constexpr int32_t kUidButtonWest = 0x0106C40A;
constexpr int32_t kUidButtonSouth = 0x0106C40B;
constexpr int32_t kUidButtonNorth = 0x0106C40C;
constexpr int32_t kUidButtonEast = 0x0106C40D;

struct ButtonMapping {
  uint16_t mask;
  int32_t uid;
};

// Sequential index into this table doubles as the real nButtonID a real
// IHID_GetButtonInfo(index, ...) call would report for that button --
// not independently confirmed against real hardware enumeration order
// (no real call site exercising GetButtonInfo was found to check
// against), but matches the standard index-enumeration contract this
// project's other HLE modules already use elsewhere, and is at least
// self-consistent between this table and GetNextButtonEventImpl.
constexpr ButtonMapping kButtonMappings[] = {
    {ZPadState::kDpadUp, kUidDpadUp},
    {ZPadState::kDpadDown, kUidDpadDown},
    {ZPadState::kDpadLeft, kUidDpadLeft},
    {ZPadState::kDpadRight, kUidDpadRight},
    {ZPadState::kStartHome, kUidStartHome},
    {ZPadState::kShoulderL, kUidShoulderL},
    {ZPadState::kShoulderR, kUidShoulderR},
    {ZPadState::kButtonWest, kUidButtonWest},
    {ZPadState::kButtonSouth, kUidButtonSouth},
    {ZPadState::kButtonNorth, kUidButtonNorth},
    {ZPadState::kButtonEast, kUidButtonEast},
};

}  // namespace

HidHle::HidHle(Memory& memory, HleRuntime& hle) : memory_(memory), hle_(hle) {}

void HidHle::RegisterForButtonEventImpl(IArmCore& core) {
  // AEEResult RegisterForButtonEvent(IHIDDevice*, ISignal *piSignal)
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void HidHle::GetNextButtonEventImpl(IArmCore& core) {
  // AEEResult GetNextButtonEvent(IHIDDevice*, AEEHIDButtonInfo *pnButtonInfo,
  //   uint32 *pdwTimestamp, boolean *pbDroppedEvents)
  if (event_queue_.empty()) {
    core.SetRegister(kR0, 1);  // no more events (AEE_EFAILED-ish)
    return;
  }
  ButtonEvent event = event_queue_.front();
  event_queue_.pop_front();

  uint32_t info_addr = core.GetRegister(kR1);
  // struct AEEHIDButtonInfo { int nButtonID; int nState; int nButtonUID;
  //   int nButtonMin; int nButtonMax; } -- confirmed field order/size
  // directly against the real AEEIHIDDevice.h.
  memory_.Write32(info_addr + 0, static_cast<uint32_t>(event.button_id));
  memory_.Write32(info_addr + 4, static_cast<uint32_t>(event.state));
  memory_.Write32(info_addr + 8, static_cast<uint32_t>(event.button_uid));
  memory_.Write32(info_addr + 12, 0);
  memory_.Write32(info_addr + 16, 1);
  uint32_t timestamp_addr = core.GetRegister(kR2);
  if (timestamp_addr != 0) memory_.Write32(timestamp_addr, 0);
  uint32_t dropped_addr = core.GetRegister(kR3);
  if (dropped_addr != 0) memory_.Write32(dropped_addr, 0);
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void HidHle::CreateDeviceImpl(IArmCore& core) {
  // AEEResult CreateDevice(IHID*, int nHandle, IHIDDevice **ppDevice)
  uint32_t ppdevice = core.GetRegister(kR2);
  if (ppdevice != 0) memory_.Write32(ppdevice, device_object_address_);
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void HidHle::GetConnectedDevicesImpl(IArmCore& core) {
  // AEEResult GetConnectedDevices(IHID*, int nDeviceType, int *pnDevHandles,
  //   int pnDevHandlesLen, int *pnDevHandlesLenReq)
  uint32_t device_handles_addr = core.GetRegister(kR2);
  uint32_t device_handles_len = core.GetRegister(kR3);
  uint32_t num_handles_req_addr = HleRuntime::ReadStackArg(core, 0);
  if (device_handles_addr != 0 && device_handles_len >= 1) {
    memory_.Write32(device_handles_addr, kSimulatedDeviceHandle);
  }
  if (num_handles_req_addr != 0) memory_.Write32(num_handles_req_addr, 1);
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

uint32_t HidHle::Build(uint32_t hid_vtable_address, uint32_t hid_object_address,
                        uint32_t device_vtable_address, uint32_t device_object_address) {
  device_object_address_ = device_object_address;

  // Real vtable ordering confirmed directly against the bundled real
  // AEEIHIDDevice.h: INHERIT_IQI's 3 slots (AddRef/Release/
  // QueryInterface), then GetDeviceInfo/GetDeviceStatus/
  // RegisterForStatusChange/GetButtonInfo/GetNumberOfButtons/
  // RegisterForButtonEvent(8)/GetNextButtonEvent(9)/GetPositionState/
  // GetMinPositionInfo(11)/GetMaxPositionInfo(12)/GetAxesInfo(13)/... --
  // slots 11-13 matched a real Double Dragon call site exactly
  // (ddragonz.mod offset 0x100af4-0x100b48).
  std::vector<HleRuntime::HleFunction> device_methods(40, Stub);
  device_methods[8] = [this](IArmCore& core) { RegisterForButtonEventImpl(core); };
  device_methods[9] = [this](IArmCore& core) { GetNextButtonEventImpl(core); };
  BuildInterfaceObject(memory_, hle_, device_vtable_address, device_object_address,
                        device_methods);

  // Real vtable ordering: INHERIT_IQI's 3 slots, then CreateDevice(3)/
  // GetDeviceInfo/GetNextConnectEvent/RegisterForConnectEvents/
  // GetConnectedDevices(7) -- confirmed against the real bundled sample
  // source research/samples/conftest_source/conftest/GamepadMgr.c.
  std::vector<HleRuntime::HleFunction> hid_methods(10, Stub);
  hid_methods[3] = [this](IArmCore& core) { CreateDeviceImpl(core); };
  hid_methods[7] = [this](IArmCore& core) { GetConnectedDevicesImpl(core); };
  return BuildInterfaceObject(memory_, hle_, hid_vtable_address, hid_object_address,
                              hid_methods);
}

void HidHle::UpdateState(const ZPadState& state) {
  for (const ButtonMapping& mapping : kButtonMappings) {
    bool was_down = (last_buttons_ & mapping.mask) != 0;
    bool is_down = (state.buttons & mapping.mask) != 0;
    if (was_down == is_down) continue;
    int32_t button_id = static_cast<int32_t>(&mapping - kButtonMappings);
    event_queue_.push_back(ButtonEvent{button_id, is_down ? 1 : 0, mapping.uid});
  }
  last_buttons_ = state.buttons;
}

}  // namespace zeebulator
