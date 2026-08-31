#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/loader/bar.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds an IShell HLE object. Vtable slot order verified directly
// against Qualcomm's own AEEIShell.h (see TASKS.md Phase 3) -- AddRef
// and Release (from IBase) followed by CreateInstance through
// AppIsInGroup, append-only across BREW SDK versions.
//
// Every slot except CreateInstance/SetTimer/CancelTimer is currently a
// safe stub (sets a zero/failure-ish return value, no other side
// effects) -- extend individual slots with real behavior as games need
// them.
//
// CreateInstance is real, backed by a small registry of known singleton
// instances (see RegisterInstance): real compiled BREW app code goes
// through ISHELL_CreateInstance to obtain interfaces like IDisplay
// rather than receiving them directly in all cases -- confirmed via real
// disassembly of Double Dragon's AEEApplet_New call chain (TASKS.md
// Phase 8), which calls `ISHELL_CreateInstance(pIShell,
// AEECLSID_DISPLAY, &m_pIDisplay)` and treats a null result as fatal. An
// unregistered ClsId returns EFAILED (1), matching real BREW behavior
// for an unrecognized/unimplemented class rather than a lying "success"
// with an unwritten output pointer.
//
// LoadResDataEx (slot 41) is real too, backed by RegisterResourceFile:
// confirmed end to end against real Peggle (`peggle.mod`, TASKS.md
// Phase 8/PHASE8_LOG.md) -- a full instruction trace of a real per-tick
// call found the exact real `(pIShell, pszResFile, wResID, resType,
// pBuffer, pnLen)` calling convention, the real `-1` buffer sentinel
// for a size-only query (matching the real documented
// `ISHELL_GetResSize` macro), and, via `core/loader/bar.h`'s own
// resource-ID directory, exactly which real `.bar` entry a given
// `(resType, wResID)` pair resolves to.
//
// GetHandler (slot 32) is real too, found live via the sprite z-ordering
// investigation's LR-capture technique applied to the audio subsystem
// (TASKS.md/PHASE8_LOG.md Phase 8): real code (reached from the same
// per-resource "activate cached sound.ggz asset into a playback slot"
// path a bulk audio preloader feeds into) calls
// `ISHELL_GetHandler(pIShell, cls=0x01005500, pszMIME)` -- confirmed
// live, not guessed, twice, both through the real IShell object
// (matching real `AEECLSID ISHELL_GetHandler(IShell*, AEECLSID cls,
// const char *pszMIME)`, the exact shape the bundled real
// `AEEMediaUtil.c` sample uses for `AEECLSID_MEDIA`). The real MIME
// string argument didn't resolve to anything printable in that live
// capture, so it's ignored rather than matched on -- but the real
// calling convention (documented in that same sample) immediately
// feeds this call's return value into `ISHELL_CreateInstance`, so this
// doesn't need to know the *real* returned class ID: it returns
// `cls` itself when `cls == 0x01005500` (0 -- no handler -- otherwise),
// and `RegisterInstance(0x01005500, ...)` (see tools/game_probe.cpp)
// registers the real `MediaHle` object under that same value, closing
// the loop end to end.
//
// SetTimer/CancelTimer are real too: real BREW timers are one-shot, not
// repeating -- confirmed against a real bundled SDK sample
// (`research/samples/conftest_source/conftest/conftest.c`), whose own
// `PFNNOTIFY` timer callback re-arms itself by calling
// `ISHELL_SetTimer(pIShell, GAMELOOP_TIMER_MS, callback, pMe)` again as
// its own last action -- the standard BREW "game loop via self-
// rearming timer" pattern real Double Dragon uses too (confirmed via
// real disassembly of its HandleEvent(EVT_APP_START), see TASKS.md
// Phase 8). This class only tracks pending timers; it doesn't call ARM
// code itself -- see Tick().
class IShellHle {
 public:
  // `screen_width`/`screen_height` default to Zeebo's one real native
  // resolution (640x480, the same constant every frontend already
  // hardcodes) -- real GetDeviceInfo (slot 4) needs them to answer real
  // `AEEDeviceInfo::cxScreen`/`cyScreen` queries; real code that never
  // calls GetDeviceInfo doesn't need this, hence the default rather than
  // a required constructor argument.
  IShellHle(Memory& memory, HleRuntime& hle, int screen_width = 640, int screen_height = 480);

  // Registers the object pointer ISHELL_CreateInstance should hand back
  // for `cls_id`. Must be called before Build(), for any class the app
  // is expected to successfully create.
  void RegisterInstance(uint32_t cls_id, uint32_t object_ptr);

  // Registers a factory ISHELL_CreateInstance should call fresh on
  // every request for `cls_id`, instead of a single fixed object --
  // for classes real code creates more than one live instance of.
  // Real evidence this matters (TASKS.md/PHASE8_LOG.md Phase 8, the
  // sound investigation): the real GetHandler->CreateInstance call
  // pair for AEECLSID_MEDIA (`ddragonz.mod` 0x10a1e0) sits inside the
  // same function that runs once per cached sound.ggz resource being
  // activated into a playback slot -- i.e. once per sound, not once
  // overall -- so a single shared IMedia instance would have each new
  // sound silently stomp whatever the previous one was doing (same
  // underlying playback state, reused). Takes priority over a plain
  // RegisterInstance for the same `cls_id` if both are registered.
  void RegisterFactory(uint32_t cls_id, std::function<uint32_t()> factory);

  // Registers a real `.bar` resource file's raw bytes under the real
  // filename `ISHELL_LoadResDataEx` requests it by (e.g.
  // `"resources.bar"`), so real slot 41 calls can serve real resource
  // data instead of a blind stub. Parses `data` immediately (throws
  // std::runtime_error on a malformed archive, same contract as
  // `BarArchive::Parse` -- see core/loader/bar.h). Optional: games that
  // never call `LoadResDataEx` don't need this.
  void RegisterResourceFile(const std::string& name, std::vector<uint8_t> data);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

  // Schedules `callback`/`user_data` exactly the way a real
  // ISHELL_SetTimer(ms, callback, user_data) call would (same re-arm-
  // by-re-registering-the-same-identity semantics as SetTimerImpl),
  // without needing to fake a full ARM call through the real trap.
  // EXPERIMENTAL, added for one specific real, evidenced case (TASKS.md
  // Phase 8, Super BurgerTime): a real per-frame update function
  // reached via a still-unidentified class's method, not through
  // ISHELL_SetTimer directly -- see tools/game_probe.cpp for the real
  // disassembly evidence this is grounded in.
  //
  // `r0_override`, if given, is real evidence too, not a guess: the
  // Zeebo Sports Tênis/Zeeboids round of that same investigation found
  // real code registering through this exact path with a real, non-
  // zero 4th argument (`r1` at the real registration call, distinct
  // from `callback`/`user_data`) that this class used to silently
  // discard -- and the real registered callback itself reads its own
  // "self" from `r1`, not the real, already-validated-for-3-titles
  // `PFNNOTIFY(pUser in r0)` convention plain ISHELL_SetTimer uses.
  // Reads as this specific registration path really being a different,
  // real 2-argument callback shape (`callback(r0=<this override>,
  // r1=pUser)`), not the same PFNNOTIFY contract with a coincidentally-
  // unread argument. Only ever set via *this* experimental path --
  // plain ISHELL_SetTimer-registered timers are unaffected, keeping
  // their own already-validated single-argument firing convention.
  void ScheduleTimer(uint32_t ms, uint32_t callback, uint32_t user_data,
                      std::optional<uint32_t> r0_override = std::nullopt);

  // A pending SetTimer callback whose deadline Tick() determined has now
  // been reached. The caller is responsible for actually invoking it
  // (e.g. via HleRuntime::CallArmFunction(callback, user_data)) -- this
  // class has no CPU access of its own.
  struct ExpiredTimer {
    uint32_t callback;
    uint32_t user_data;
    // See ScheduleTimer's own doc comment on `r0_override`.
    std::optional<uint32_t> r0_override;
  };

  // Advances every pending timer by `elapsed_ms` and returns (removing)
  // any that have now reached their deadline, in the order they were
  // originally registered.
  std::vector<ExpiredTimer> Tick(uint32_t elapsed_ms);

 private:
  struct PendingTimer {
    uint32_t remaining_ms;
    uint32_t callback;
    uint32_t user_data;
    std::optional<uint32_t> r0_override;
  };

  void CreateInstanceImpl(IArmCore& core);
  void GetDeviceInfoImpl(IArmCore& core);
  void SetTimerImpl(IArmCore& core);
  void CancelTimerImpl(IArmCore& core);
  void LoadResDataExImpl(IArmCore& core);
  void GetHandlerImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  int screen_width_;
  int screen_height_;
  std::unordered_map<uint32_t, uint32_t> instances_;
  std::unordered_map<uint32_t, std::function<uint32_t()>> factories_;
  std::vector<PendingTimer> timers_;
  std::unordered_map<std::string, BarArchive> resource_files_;
  // Per-object vtable-slot-43 call counter -- see Build()'s own doc
  // comment on that slot for the real evidence this stateful behavior
  // is grounded in.
  std::unordered_map<uint32_t, uint32_t> slot43_call_counts_;
};

}  // namespace zeebulator
