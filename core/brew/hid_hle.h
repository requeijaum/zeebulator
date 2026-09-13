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
// --- Eixos analogicos do controle do Zeebo -----------------------------------
//
// A faixa de cada eixo e um BYTE SEM SINAL, 0..255, com o REPOUSO EM 128. Isso
// nao e escolha de projeto: esta escrito no proprio jogo. Verificado neste
// corpus, em funsoccer.mod (Zeebo F.C. Super League) em 0x001ed504:
//
//     mvn   r0, #0x7f        ; r0 = -128
//     sxtah r4, r0, r4       ; valor = (int16)eixo - 128
//     sxtah r3, r0, r3
//     strh  r4, [r2]         ; quatro eixos, convertidos e gravados em halfword
//
// O jogo subtrai 128 para achar o centro, logo o centro do aparelho e 128.
//
// POR QUE ISSO IMPORTA: reportar ZERO em repouso -- que era o que faziamos --
// entrega -128 nos quatro eixos, ou seja, o manche ENCOSTADO no batente. Todo
// jogo dessa camada anda sozinho para um canto com o controle parado.
inline constexpr int32_t kAxisMin = 0;
inline constexpr int32_t kAxisCenter = 128;
inline constexpr int32_t kAxisMax = 255;

// UID de cada eixo, na palavra correspondente do AEEHIDPositionInfo.
//
// O X VALE 0x0106C4D0, E AGORA ISSO E FONTE PRIMARIA, NAO INFERENCIA.
//
// O arquivo de configuracao do proprio console,
// research/sources/zeemu/rootfs/sys/hid_devices.cfg, traz QUATRO entradas de
// controle (Sony DualShock 4 CUH-ZCT2x e ZCT1x, Logitech Dual Action, Logitech
// RumblePad2), de dois fabricantes diferentes, e TODAS AS QUATRO declaram a
// mesma coisa:
//
//     AXIS:X:0x0106c4d0
//     AXIS:Y:0x0106c4d1
//     AXIS:Z:0x0106c4ce
//     AXIS:RZ:0x0106c4cf
//
// Antes desta base usava 0x0106C40C no X, que e UID de BOTAO -- o mesmo valor
// que este arquivo usa como kUidButtonNorth logo abaixo. Eixo e botao com o
// mesmo UID nao podem coexistir: o jogo varre a tabela do GetAxesInfo
// procurando UID de eixo, nao acha nenhum para o X e nunca guarda o campo dele.
// Esse argumento de colisao foi o que motivou a correcao; o arquivo do console
// depois a confirmou, quatro vezes.
inline constexpr int32_t kUidAxisX = 0x0106C4D0;
inline constexpr int32_t kUidAxisY = 0x0106C4D1;
inline constexpr int32_t kUidAxisZ = 0x0106C4CE;
inline constexpr int32_t kUidAxisRZ = 0x0106C4CF;

// AEEHIDPositionInfo: 25 palavras. A palavra 0 NAO e eixo -- e o bRelativeAxes.
inline constexpr uint32_t kPositionInfoWords = 25;
inline constexpr uint32_t kAxisWordX = 1;
inline constexpr uint32_t kAxisWordY = 2;
inline constexpr uint32_t kAxisWordZ = 3;
inline constexpr uint32_t kAxisWordRZ = 6;

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
  void GetDeviceInfoImpl(IArmCore& core);
  void GetNextConnectEventImpl(IArmCore& core);
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
