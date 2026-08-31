#include "core/brew/ishell.h"

#include <utility>

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

// Generic stub: sets a zero/failure-ish return value and does nothing
// else. Safe default for any BREW method call whose behavior isn't
// implemented yet -- real games that hit one of these will need it
// filled in with real behavior at that point.
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace

IShellHle::IShellHle(Memory& memory, HleRuntime& hle, int screen_width, int screen_height)
    : memory_(memory), hle_(hle), screen_width_(screen_width), screen_height_(screen_height) {}

void IShellHle::RegisterInstance(uint32_t cls_id, uint32_t object_ptr) {
  instances_[cls_id] = object_ptr;
}

void IShellHle::RegisterFactory(uint32_t cls_id, std::function<uint32_t()> factory) {
  factories_[cls_id] = std::move(factory);
}

void IShellHle::RegisterResourceFile(const std::string& name, std::vector<uint8_t> data) {
  resource_files_.emplace(name, BarArchive::Parse(std::move(data)));
}

void IShellHle::CreateInstanceImpl(IArmCore& core) {
  // int CreateInstance(IShell *po, AEECLSID cls, void **ppo)
  uint32_t cls_id = core.GetRegister(kR1);
  uint32_t ppobj = core.GetRegister(kR2);
  auto factory_it = factories_.find(cls_id);
  if (factory_it != factories_.end()) {
    memory_.Write32(ppobj, factory_it->second());
    core.SetRegister(kR0, 0);  // SUCCESS
    return;
  }
  auto it = instances_.find(cls_id);
  if (it == instances_.end()) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: unknown/unimplemented class
    return;
  }
  memory_.Write32(ppobj, it->second);
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::GetDeviceInfoImpl(IArmCore& core) {
  // void GetDeviceInfo(IShell *po, AEEDeviceInfo *pdi)
  // po is R0 (unused, matches "this"), pdi R1. Real AEEDeviceInfo (see
  // the real bundled AEEShell.h reference, research/docs/
  // sdk_installer_extract/brew_sdk_headers_reference/) starts with
  // `uint16 cxScreen; uint16 cyScreen;` at offsets 0/2 -- confirmed
  // against real Double Dragon disassembly (TASKS.md/PHASE8_LOG.md
  // Phase 8): its own real screen-size-query call site reads exactly
  // those two offsets out of this call's own output buffer, and (before
  // this fix) got 0/0 back from this slot's old blind-stub behavior,
  // producing a real, confirmed-via-live-GL-trace degenerate
  // `glViewport(0,0,1,0)` call downstream. Every other real
  // AEEDeviceInfo field is left zeroed (this project doesn't have
  // evidence any real game reads them yet) -- not a claim the rest of
  // the struct is correctly populated, just that these two confirmed-
  // read fields are.
  uint32_t pdi = core.GetRegister(kR1);
  if (pdi != 0) {
    memory_.Write16(pdi + 0, static_cast<uint16_t>(screen_width_));   // cxScreen
    memory_.Write16(pdi + 2, static_cast<uint16_t>(screen_height_));  // cyScreen
  }
  core.SetRegister(kR0, 0);
}

void IShellHle::ScheduleTimer(uint32_t ms, uint32_t callback, uint32_t user_data,
                               std::optional<uint32_t> r0_override) {
  // Re-registering the same (callback, user_data) pair reschedules it
  // rather than creating a duplicate -- matches the real self-rearming
  // timer pattern (see class doc comment) where the callback calls
  // SetTimer again with the same identity every time it fires.
  for (auto& timer : timers_) {
    if (timer.callback == callback && timer.user_data == user_data) {
      timer.remaining_ms = ms;
      timer.r0_override = r0_override;
      return;
    }
  }
  timers_.push_back(PendingTimer{ms, callback, user_data, r0_override});
}

void IShellHle::SetTimerImpl(IArmCore& core) {
  // int SetTimer(IShell *ps, uint32 dwCount, PFNNOTIFY pfnNotify, void *pUser)
  ScheduleTimer(core.GetRegister(kR1), core.GetRegister(kR2), core.GetRegister(kR3));
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::CancelTimerImpl(IArmCore& core) {
  // int CancelTimer(IShell *ps, PFNNOTIFY pfnNotify, void *pUser)
  uint32_t callback = core.GetRegister(kR1);
  uint32_t user_data = core.GetRegister(kR2);
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->callback == callback && it->user_data == user_data) {
      timers_.erase(it);
      core.SetRegister(kR0, 0);  // SUCCESS
      return;
    }
  }
  core.SetRegister(kR0, 1);  // EFAILED-ish: no matching timer
}

void IShellHle::LoadResDataExImpl(IArmCore& core) {
  // AEEResult LoadResDataEx(IShell *pIShell, const char *pszResFile,
  //   uint16 wResID, AEERESTYPE resType, void *pBuffer, uint32 *pnLen)
  // Real calling convention and the real `(void*)-1` "size only" buffer
  // sentinel confirmed against a real Peggle call site -- see this
  // class's own doc comment.
  std::string filename = ReadCString(memory_, core.GetRegister(kR1));
  uint32_t id = core.GetRegister(kR2);
  uint32_t type = core.GetRegister(kR3);
  uint32_t buffer = HleRuntime::ReadStackArg(core, 0);
  uint32_t len_addr = HleRuntime::ReadStackArg(core, 1);

  auto file_it = resource_files_.find(filename);
  if (file_it == resource_files_.end()) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: unregistered resource file
    return;
  }
  const BarEntry* entry =
      file_it->second.Find(static_cast<uint16_t>(type), static_cast<uint16_t>(id));
  if (entry == nullptr) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: no directory entry for this (type, id)
    return;
  }

  constexpr uint32_t kSizeOnlySentinel = 0xFFFFFFFF;
  if (buffer == kSizeOnlySentinel) {
    if (len_addr != 0) memory_.Write32(len_addr, entry->size);
    core.SetRegister(kR0, 0);  // SUCCESS
    return;
  }

  std::vector<uint8_t> data = file_it->second.Extract(*entry);
  for (uint32_t i = 0; i < data.size(); ++i) {
    memory_.Write8(buffer + i, data[i]);
  }
  if (len_addr != 0) memory_.Write32(len_addr, entry->size);
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::GetHandlerImpl(IArmCore& core) {
  // AEECLSID GetHandler(IShell *ps, AEECLSID cls, const char *pszMIME)
  // See this class's own doc comment for the real evidence this slot's
  // behavior is grounded in. Real code immediately feeds the return
  // value into ISHELL_CreateInstance, so returning `cls` itself (when
  // recognized) rather than some other synthetic value is enough --
  // RegisterInstance registers the real handler object under that same
  // ClsId.
  constexpr uint32_t kAudioMediaCls = 0x01005500;
  uint32_t cls = core.GetRegister(kR1);
  core.SetRegister(kR0, cls == kAudioMediaCls ? cls : 0);
}

std::vector<IShellHle::ExpiredTimer> IShellHle::Tick(uint32_t elapsed_ms) {
  std::vector<ExpiredTimer> expired;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (elapsed_ms >= it->remaining_ms) {
      expired.push_back(ExpiredTimer{it->callback, it->user_data, it->r0_override});
      it = timers_.erase(it);
    } else {
      it->remaining_ms -= elapsed_ms;
      ++it;
    }
  }
  return expired;
}

uint32_t IShellHle::Build(uint32_t vtable_address, uint32_t object_address) {
  // Order matches AEEIShell.h's INHERIT_IShell macro exactly (verified
  // directly against real Qualcomm source -- see TASKS.md Phase 3), up
  // through the pre-BREW-MP slot count (40 IShell-specific methods,
  // which is what a 2009-era Zeebo/BREW 4.x IShell should have --
  // BREW MP's later-appended slots, e.g. RegisterSystemCallback onward,
  // are deliberately not included since Zeebo predates that rebrand).
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                            // 0  AddRef
      Stub,                                            // 1  Release
      [this](IArmCore& c) { CreateInstanceImpl(c); },   // 2  CreateInstance
      Stub,  // 3  QueryClass
      [this](IArmCore& c) { GetDeviceInfoImpl(c); },  // 4  GetDeviceInfo
      Stub,  // 5  StartApplet
      Stub,  // 6  CloseApplet
      Stub,  // 7  CanStartApplet
      Stub,  // 8  ActiveApplet
      Stub,  // 9  EnumAppletInit
      Stub,  // 10 EnumNextApplet
      [this](IArmCore& c) { SetTimerImpl(c); },     // 11 SetTimer
      [this](IArmCore& c) { CancelTimerImpl(c); },  // 12 CancelTimer
      Stub,  // 13 GetTimerExpiration
      Stub,  // 14 CreateDialog
      Stub,  // 15 GetActiveDialog
      Stub,  // 16 EndDialog
      Stub,  // 17 LoadResString
      Stub,  // 18 LoadResData
      Stub,  // 19 LoadResObject
      Stub,  // 20 FreeResData
      Stub,  // 21 SendEvent
      Stub,  // 22 Beep
      Stub,  // 23 GetPrefs
      Stub,  // 24 SetPrefs
      Stub,  // 25 GetItemStyle
      Stub,  // 26 Prompt
      Stub,  // 27 MessageBox
      Stub,  // 28 MessageBoxText
      Stub,  // 29 SetAlarm
      Stub,  // 30 CancelAlarm
      Stub,  // 31 AlarmsActive
      [this](IArmCore& c) { GetHandlerImpl(c); },  // 32 GetHandler
      Stub,  // 33 RegisterHandler
      Stub,  // 34 RegisterNotify
      Stub,  // 35 Notify
      Stub,  // 36 Resume
      Stub,  // 37 ForceExit
      Stub,  // 38 GetPosition
      Stub,  // 39 CheckPrivLevel
      Stub,  // 40 IsValidResource
      [this](IArmCore& c) { LoadResDataExImpl(c); },  // 41 LoadResDataEx
      // Slots below this point are NOT verified against any real header --
      // unlike 0-41 above, they're only known to exist at all because real
      // Double Dragon disassembly (`ddragonz.mod` offset 0x10a234) showed a
      // genuine call through vtable offset 0xac (slot 43), one word past
      // what the "40 IShell-specific methods" pre-BREW-MP count above
      // accounted for -- either that count was of an incomplete real
      // header, or Zeebo's own BREW variant extends classic IShell by a
      // couple of slots the same way this project has already found it
      // doing for other interfaces. Extended with safe, generously-sized
      // headroom (matching this project's established precedent, e.g. the
      // HID device scaffold) rather than pinned exactly to slot 43, so the
      // next real call into this range doesn't reproduce the same
      // undersized-vtable crash.
      Stub,  // 42 unconfirmed
      // 43: real, confirmed 3-arg out-param shape, still-unidentified
      // real meaning. Real Alien Breaker Deluxe disassembly (`abd.mod`
      // 0x101360-0x101374, a real per-object lazy-init routine gating a
      // real object-pointer field this project's own live tracing
      // confirmed stays null and crashes a later real call otherwise)
      // calls this slot `(shell, dw=0, &pOut)` and requires *two* real
      // conditions to proceed past a real bail-out branch: the literal
      // return value 35 exactly (`cmp r0, #35`, not just "nonzero"), and
      // `*pOut != 0` one instruction later.
      //
      // *pOut isn't a pointer real code dereferences, though -- confirmed
      // live it flows on, unmodified, into a real raw unsigned comparison
      // much further down the same real function (`abd.mod` 0x101550,
      // `cmp [real handle from an earlier real lookup], pOut-value`) that
      // decides between a real success path and a real "not ready yet"
      // status. An earlier version of this fix wrote this same shell
      // object's own address here, which is *always* non-null (passing
      // the first check) but is such a large value that it's *never*
      // less-or-equal to any real handle in that later comparison,
      // permanently forcing the "not ready" branch -- confirmed live via
      // that exact comparison's own operands. Writes the small constant 1
      // instead: still real and non-null (passes the first check), and
      // small enough to be less-or-equal to any real handle this project
      // has observed there, letting the real success path trigger instead
      // of a real value being guessed at.
      //
      // With that fix in place, real code reaches a second real call
      // site to this exact slot, on the same object, with the same
      // arguments (`this=shell, r1=0`) -- confirmed live this second
      // call needs to return 0, not 35, to take its own further real
      // branch (`abd.mod` 0x1016a0 onward): the real code's own `bne`
      // otherwise skips real write logic a fixed-35 stub can't reach.
      // This is the same real "async, not ready yet" shape as the
      // `0x905` finding elsewhere in this same investigation (see
      // TASKS.md) -- a real stateful/polling contract, not a single
      // static value. Modeled here as a real per-object *toggle*, not a
      // one-shot "first call ever" latch: an earlier version of this
      // fix keyed the toggle on this shell object's own address (the
      // only real identity available to this call, since the real
      // per-target object being initialized -- e.g. `abd.mod`'s own
      // `r4`-relative struct -- is never passed as an argument here at
      // all), which correctly answered the *first* real per-object
      // lazy-init sequence (35 then 0) but then starved every
      // subsequent real object's own sequence of ever seeing 35 again
      // (confirmed live: a live per-call trace showed a *second* real
      // target object's own init call landing on this shared counter's
      // 3rd call, getting 0 first instead of 35, and its own real
      // out-field staying null as a direct result -- exactly the same
      // failure shape the first fix solved for object one). Real
      // Alien Breaker Deluxe disassembly runs this exact real lazy-
      // init sequence back-to-back for what live tracing confirmed are
      // 35 distinct real objects (`abd.mod`, real per-object pointers
      // populated at addresses 0x80200000, 0x80200004, ... one word
      // apart -- an evidently real, sequential real allocation, not a
      // coincidence), each needing its own fresh 35-then-0 pair.
      // Switched to alternating strictly by call parity (odd calls
      // return 35, even calls return 0) rather than "only the very
      // first call ever": confirmed live this correctly completes all
      // 35 real per-object sequences in a row, each getting its own
      // real, valid, non-null pointer -- both real call sites this
      // slot's own doc comment above already documents (35 then 0, on
      // the same object, same arguments) are still satisfied exactly,
      // since they're just this same toggle's first two calls.
      [this](IArmCore& c) {
        uint32_t pout = c.GetRegister(kR2);
        if (pout != 0) c.GetMemory().Write32(pout, 1);
        uint32_t object_address = c.GetRegister(kR0);
        uint32_t& count = slot43_call_counts_[object_address];
        c.SetRegister(kR0, (count % 2 == 0) ? 35 : 0);
        ++count;
      },
      Stub,  // 44 unconfirmed
      Stub,  // 45 unconfirmed
      Stub,  // 46 unconfirmed
      Stub,  // 47 unconfirmed
      Stub,  // 48 unconfirmed
      Stub,  // 49 unconfirmed
  };
  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
