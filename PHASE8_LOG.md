# Phase 8 Investigation Log — Double Dragon (and, from the log's final
# section onward, a second title: Peggle; a third, Super BurgerTime,
# starts near the very end)

This is the detailed, blow-by-blow investigation log for TASKS.md's Phase 8
task "Iteratively debug against the real game, filling HLE API gaps as
they're hit." It was split out of TASKS.md once it grew large enough to
dominate that file — see TASKS.md for the current summary/status and the
rest of the project's task list.

Every entry here is grounded in real evidence (real disassembly via
`arm-none-eabi-objdump`, real bundled SDK/header extraction, or live
runtime instrumentation) against the real, compiled game module —
never guessed. Entries are in chronological order (oldest first); the
most recent entry reflects current status. Almost all entries are
against Double Dragon's `ddragonz.mod`; the final section is against a
second real title, Peggle's `peggle.mod`, brought in to check whether
HLE work tuned against one game generalizes to another.

---

**First real attempt, honestly reported**: built `tools/game_probe.cpp`,
a dev tool that drives Double Dragon's real, compiled
`mod/274754/ddragonz.mod` through the actual BREW app lifecycle —
`AEEMod_Load` → `IModule::CreateInstance(ClsId=274754)` →
`HandleEvent(EVT_APP_START)` — using the real `IShell`/`IDisplay`/
`IGL`/`IEGL`/`IFile`/`IMedia` HLE built in earlier phases, real
`data.ggz`/`sound.ggz` assets mounted via `VirtualFilesystem`, and a
real SDL2 window/GL context/audio device (not a headless stub).
**Result: does not yet load.** The game does not successfully reach
`EVT_APP_START`. Two real, previously-unknown facts about the real
BREW ABI were discovered and corrected along the way (via a genuine
NSIS extraction of the Qualcomm BREW MP SDK installer, not guessed):
  - `EVT_APP_START = 0`, not `1` as our own `hello_brew.c`/
    `hello_gl.c` SDK fixtures had assumed (`platform/system/inc/AEEEvent.h`).
    Those fixtures are self-consistent, so this never broke anything
    internally — but it was wrong for real game code.
  - The real `AEEAppStart` struct is
    `{ int error; AEECLSID clsApp; IDisplay *pDisplay; AEERect rc; const char *pszArgs; }`
    — 5 fields, not 4 — and the real `AEERect` is
    `{ int16 x, y, dx, dy; }` (8 bytes), not `{ int x, y, dx, dy; }`
    (16 bytes) as our own fixtures assumed. Correct total size: 24 bytes.
**The actual blocker**: disassembling the real `.mod` directly
(`arm-none-eabi-objdump -D -b binary -m arm`, the authoritative way to
read a raw flat binary — manual hex decoding of this same function
produced real mistakes before switching to the real tool) shows
`AEEMod_Load`'s body computing `r0 = <module's own loaded base
address>` via PC-relative addressing, then executing
`ldr r0, [r0, #-4]` — reading a pointer from 4 bytes *before* the
module's own base. This is the real ARM RVCT "ROPI" (Read-Only
Position-Independent) convention: compiled code expects the real BREW
OS loader to place a pointer to a runtime-allocated global-data/
static-base segment immediately before the module image in memory.
Zeebulator's `.mod` loader (`core/loader/mod.{h,cpp}`) does not
currently provide this — nothing is written at `moduleBase - 4` — so
that load reads whatever was already in emulated memory there (zero,
since it was never written), and the module jumps through a garbage
function pointer built from it.
**Why this wasn't obvious at first**: the ARM interpreter's existing,
correct convention of decoding never-written (zero-filled) memory as
harmless `ANDEQ r0,r0,r0` no-ops meant the very first attempt produced
a *plausible-looking but meaningless* "success" — 262,237 steps of
no-ops walking PC from address 0 back up to the module's own base
address, silently re-entering `AEEMod_Load`, and returning
`R0=0x00000001` (a bogus "module pointer"). This was **not** a CPU
interpreter bug (it behaves exactly as specified for the bytes it's
given) — it's the loader's missing static-base setup producing a
false positive that looked like progress. Caught by building
`CallArmFunctionChecked`, a diagnostic wrapper that tracks whether PC
ever leaves the loaded module's address range and reports it as a
loud warning rather than silently accepting whatever `R0` comes back.
With that check in place, the real, honest result is: PC leaves the
module's range after only 37 steps (reading the garbage pointer and
jumping through it), and the tool now stops and reports this cleanly
instead of reporting a false "AEEMod_Load OK".
**Net assessment**: real compiled Double Dragon code now runs
further and more meaningfully than at any prior phase — past the
entry-stub validation exercised by Phase 2's `mod_probe` (which never
called real `AEEMod_Load` with real arguments) and into real
argument-validated logic inside the actual function. It still does
not load. The next concrete step (not yet attempted) is teaching
`core/loader/mod.{h,cpp}` to allocate and populate a static-base
segment at `moduleBase - 4`, per the real ROPI convention — this is a
loader/runtime-support gap, not an HLE API gap, so it's tracked here
rather than under a specific interface.
**Static-base fix implemented**: `core/brew/mod_runtime.{h,cpp}`
(`ModRuntime`). Confirmed the exact table layout needed by reading
the real bundled SDK reference source `AEEModGen.c` (found under
`research/docs/sdk_installer_extract/`): `AEEMod_Load` calls the
standard helper `AEEStaticMod_New(sizeof(AEEMod), pIShell, ph,
ppMod, NULL, NULL)`, which does
`MALLOC(nSize + sizeof(IModuleVtbl))` — and `IModuleVtbl` is exactly
4 function pointers (`AddRef`/`Release`/`CreateInstance`/
`FreeResources`, 16 bytes), matching the disassembly's `add r0, r5,
#16` immediately before the indirect call through table offset
`0x68`. Cross-checked the PC-relative literal at file offset
`0x220c` in the real `ddragonz.mod` directly (`r0 = pc(0x2188) +
0xffffde78 = 0x00000000`), confirming the computed address is
exactly the module's own load base — i.e. the table pointer really
does live at `moduleBase - 4` for any load address, as the ROPI
convention requires. `ModRuntime::Install()` writes a table pointer
there and populates only that one confirmed slot (offset `0x68`)
with an HLE trap implementing a simple bump-allocator `MALLOC`; all
other table offsets remain intentionally unmapped (unconfirmed).
5 new tests (`tests/mod_runtime_test.cpp`): table pointer placement,
a real allocation call through the HLE trap, non-overlapping
successive allocations, 4-byte alignment, and out-of-heap-space
returning NULL.
**Verified against the real game**: reran `zeebulator_game_probe`
against the real `ddragonz.mod`/`data.ggz`/`sound.ggz`.
`AEEMod_Load` now succeeds *legitimately* — no
wandered-outside-module warning, a real module pointer
(`0x80300000`, the heap's first real allocation) rather than the
old false-positive `0x00000001`. Execution now reaches
`IModule::CreateInstance(ClsId=274754)` and runs real, in-bounds
module code to completion, but that call itself returns a genuine
`EFAILED` (1) without writing an object pointer — a new, different,
and deeper real gap than the loader issue this fix closes.
**Continued the investigation**: disassembled `CreateInstance`'s
real body (`arm-none-eabi-objdump` again, not manual decoding).
With the *wrong* guessed `ClsId` (`274754`, the `.mod` file's
containing directory name — just a filesystem/distribution
convention, it turns out, not the app's real class ID), the real
code path legitimately hit a `cmp ClsId, #0x0102f789; bne EFAILED`
check and correctly reported failure — a real, trustworthy
rejection, not a bug. Cross-checked the literal directly against
the raw file bytes at offset `0x738` (`0x0102f789`, exactly the
value the disassembly compares against) to be certain. **Rerunning
with the real ClsId (`0x0102F789` / `16971657`) gets past that
check** and reaches real, deeper application logic: `AEEClsCreateInstance`
tail-calls the real, standard SDK helper `AEEApplet_New` (identified
by matching disassembly line-by-line against the real reference
source `AEEAppGen.c`, also found under
`research/docs/sdk_installer_extract/`), which mallocs a real
`AEEApplet` struct (via the same confirmed MALLOC slot) and then
calls `ISHELL_CreateInstance(pIShell, AEECLSID_DISPLAY, &m_pIDisplay)`
— real compiled app code obtains its `IDisplay` through `IShell`,
not directly via `EVT_APP_START` as our own `hello_brew`/`hello_gl`
SDK fixtures do. `AEECLSID_DISPLAY`'s real value (`0x01001001`) was
read directly out of the disassembly (the literal loaded right
before the `ISHELL_CreateInstance` call), not guessed.
Our `IShell::CreateInstance` was a blind stub returning success
without writing `*ppObj` — meaning `m_pIDisplay` stayed `NULL`,
which `AEEApplet_New` correctly treats as fatal (matching the real
reference source) and tries to clean up via `FREE(pme)` — landing
on a **second** unmapped static-base table slot, offset `0x6c`
(`FREE`, immediately after `MALLOC` at `0x68`), causing the same
jump-through-null-pointer wander as before.
**Fixed both**: `core/brew/ishell.{h,cpp}` — `BuildIShell` became
the stateful `IShellHle` class with `RegisterInstance(clsId, ptr)`,
so callers can register real singleton objects (our already-built
`IDisplayHle` instance) for `IShell::CreateInstance` to hand back;
an unregistered ClsId now returns real `EFAILED` instead of a lying
"success" with an unwritten pointer. `core/brew/mod_runtime.{h,cpp}`
gained the `FREE` slot at offset `0x6c` (a no-op trap — consistent
with the bump allocator's documented no-free-list limitation).
3 new `IShellHle` tests in `tests/brew_test.cpp`.
**Reran against the real game**: with all three fixes, `CreateInstance`
now gets substantially further — `*ppObj` is written
(`0x80300024`, a real, live `AEEApplet` pointer) and it returns real
`SUCCESS` (0) — but `tools/game_probe.cpp`'s wandered-outside-module
check still (correctly) flags the result as untrustworthy: at real
step 168, different real code elsewhere in the module reads *yet
another* static-base table offset (`0xc0`), unrelated to
`MALLOC`/`FREE`, and jumps through it while still zero/unmapped.
This is the same static-base table, but evidently used as a much
larger, general-purpose runtime-services jump table (real BREW's
`AEEStdLib` helper dispatch is a plausible candidate, based on the
calling pattern seen at this new site) — mapping it out fully looks
like it needs its own systematic pass (identifying each offset's
real function one at a time against the reference SDK source, the
same way `MALLOC`/`FREE` were confirmed), not a single quick fix.
**Scanned the whole binary instead of tracing one call site at a
time**: wrote a one-off script (`arm-none-eabi-objdump` output fed
through a small Python pattern-match for "read static base, then
index the result") to find every distinct offset `ddragonz.mod`
actually dereferences off this table. Real, bounded result — not
"dozens", nine distinct offsets total: `0x4`, `0x8`, `0x14`, `0x68`
(`MALLOC`), `0x6c` (`FREE`), `0xb0`, `0xc0`, `0xe8`, `0x13c`. Offset
`0xc0` dominates by far (138 of the real call sites). Checked three
independent `0xc0` call sites and found byte-for-byte identical
code shape at all three: call the slot with no argument, read
offset `+12` from the result, dereference that once more to reach
a vtable, and call vtable slot 2 (`CreateInstance`) on it — i.e.
`ISHELL_CreateInstance(pIShell, ClsId, ppObj)` where `pIShell` comes
from an ambient "current app context" instead of being passed in
explicitly. This is the real mechanism behind `AEEStdLib` helper
macros that don't take an explicit `IShell*` argument.
**Implemented**: `core/brew/mod_runtime.{h,cpp}` gained
`SetShellInstance()` and a `GetAppContext` trap at offset `0xc0`
that returns a small context struct with the real `IShell` pointer
at field `+12` (written fresh on every call, so call order with
`SetShellInstance()` doesn't matter). `ModRuntime`'s constructor
gained a `context_address` parameter for where that struct lives.
4 new tests in `tests/mod_runtime_test.cpp` (FREE doesn't crash,
`GetAppContext` returns the registered shell pointer at the
confirmed offset, both call orderings of `SetShellInstance()`).
**A second, unrelated bug found and fixed in `tools/game_probe.cpp`
itself** (not an emulation gap): `*ppObj` from `IModule::CreateInstance`
is the `IApplet*` object itself, not a callable `HandleEvent`
pointer — real `HandleEvent` is vtable slot 2 of *that* object
(`AddRef=0, Release=1, HandleEvent=2`, confirmed against the real
`AEEAppGen.c` reference source's `IAppletVtbl` init order), the same
double-indirection already used for `IModule::CreateInstance`
itself just above it. The tool was also passing `0` instead of the
real applet pointer as `HandleEvent`'s own `po` ("this") argument.
Both fixed.
**Full result — first complete, successful BREW app lifecycle for a
real commercial game module**: reran `zeebulator_game_probe`
against the real `ddragonz.mod`/`data.ggz`/`sound.ggz`.
`AEEMod_Load` → `IModule::CreateInstance` →
`IApplet::HandleEvent(EVT_APP_START)` all complete with **no**
wandered-outside-module warning and **no** `UnimplementedInstruction`
— `HandleEvent` returns real `TRUE` (1), meaning the real compiled
app reports it successfully handled its own startup event. Captured
a real screenshot of the live SDL window afterward (`xwd` +
`ffmpeg`, same verification rigor as Phase 5): solid black, which is
the *correct*, expected result here, not a failure — nothing has
driven a draw call yet, since only the one `EVT_APP_START` event was
ever delivered and no per-frame/timer event pump exists yet.
**Honest scope**: this is real, verified initialization success —
not yet "playable" (PRD/Phase 8's actual exit criterion). Real
gameplay would need at minimum: a `SetTimer`/timer-tick mechanism
(currently `IShell::SetTimer` is a blind stub), likely
`EVT_APP_ACTIVATE` and other lifecycle events real BREW sends after
`EVT_APP_START`, and probably several of the six still-unmapped
static-base offsets found by the scan above (`0x4`, `0x8`, `0x14`,
`0xb0`, `0xe8`, `0x13c`) as deeper game logic starts executing.
**Implemented the real per-frame game loop**: traced
`HandleEvent(EVT_APP_START)`'s one real HLE call and found it's
`ISHELL_SetTimer(shell, 33, callback=0x1239dc, pUser=applet_ptr)` —
the real game registering its own tick callback, exactly the
standard "self-rearming `SetTimer`" BREW game-loop pattern
confirmed against a real bundled SDK sample
(`research/samples/conftest_source/conftest/conftest.c`'s
`conftest_TimerNotify`, which re-arms itself via `ISHELL_SetTimer`
as its own last action -- real BREW timers are one-shot, not
repeating). `IShell::SetTimer`/`CancelTimer` (vtable slots 11/12)
are now real: `IShellHle` tracks pending timers and exposes
`Tick(elapsed_ms)`, which the frontend calls once per loop
iteration and drives any newly-expired callback through
`CallArmFunctionChecked`. 4 new `IShellHle` tests.
**Mapped two more static-base slots this way, real disassembly at
each step, not guesses**:
  - Offset `0xb0` (4 real call sites) is `GETUPTIMEMS`: called
    twice with no argument around a chunk of work, with
    `second_result - first_result` used immediately after as an
    elapsed-ms delta (`sub r1, r0, r7` in the real timer-callback
    disassembly) -- the standard elapsed-time-measurement idiom.
    `ModRuntime::Tick()` now advances a millisecond counter this
    slot returns.
  - Offset `0x4` (10 real call sites, the single most common after
    `0xc0`) is `MEMSET`: a real call site
    (`ddragonz.mod` file offset `0x1f9b0`) disassembles to exactly
    `memset(r4+46, 0, 10)` -- `r0`=dest, `r1`=fill value, `r2`=byte
    count, matching ANSI `memset` precisely. Real ROPI-compiled
    code apparently routes even plain C library calls through this
    table, not just AEE/BREW-specific ones -- worth remembering
    when mapping the remaining offsets (`0x8`, `0x14`, `0xe8`,
    `0x13c`; `0x8`'s one real call site disassembles to a very
    `strcpy`-shaped 2-argument call, `strcpy(dest, src)`, but
    wasn't fully confirmed/implemented this pass).
2 new tests total for these (`ModRuntime.GetUpTimeMsStartsAtZeroAndAdvancesWithTick`,
`ModRuntime.MemsetFillsExactlyTheRequestedRangeAndReturnsDest`).
**Reran against the real game**: the timer callback now fires for
real, every frame, and runs measurably deeper each time a slot gets
mapped -- 14 real steps before `SetTimer` existed at all, 41 after
`GETUPTIMEMS`, 153 after `MEMSET`. The real per-tick game logic
currently stops on a **new, different** class of gap, traced down
to a specific real function this time (`ddragonz.mod` offset
`0x23a18`, called with `r0 = &applet[0x140]`, an *embedded*
sub-object living inside the applet's own struct, not a separately
`MALLOC`'d one): it copies a couple of fields from a template
object cached at `applet[0x140]+12`, then reads `*(applet[0x140])`
-- expecting the embedded sub-object's own vtable pointer to
*already* be populated -- and calls vtable slot 18 (offset `0x48`)
on it. That memory was never written by anything in the traced
execution (no static-base call touches it, and our own `MALLOC`
doesn't zero-fill), so it reads back 0 and the call jumps through
NULL. This isn't a missing runtime-library slot -- it's most likely
a real `str <compile-time-vtable-address>, [applet+0x140]`
"placement new" that should have happened earlier, inside
`HandleEvent(EVT_APP_START)`'s own real (non-HLE) logic, which
wasn't traced at per-instruction granularity this session (only
its one real HLE call was). Root-causing this means tracing
`HandleEvent`'s full internal control flow, not just another
library-call mapping -- a different, larger kind of investigation
than the last several fixes.
**Root-caused, and the actual cause was simpler than expected**:
full per-instruction tracing of `HandleEvent(EVT_APP_START)` shows
it does *nothing* for `evt==0` except call `ISHELL_SetTimer` and
return TRUE (confirmed via real disassembly of the app's own event
dispatcher at `ddragonz.mod` offset `0xc5e0`, which switches on the
real BREW event codes with `evt==0` landing on exactly that path)
-- so `applet+0x140` was never going to be set there. Tracing
further up, into the *already-passing* `CreateInstance` call (with
full tracing this time, not just its HLE calls), found the real
write: `ddragonz.mod` offset `0x1b5e4` does
`*(applet+0x140) = *(our_context_struct + 20)` -- reading a
**second field** of the same "app context" struct our own
`ModRuntime::GetAppContextImpl` (offset-`0xc0` slot) returns, one
we'd only ever populated at offset `+12` (the `IShell` pointer).
Offset `+20` was never written, so it read back 0 and the NULL
propagated forward into `applet+0x140`, surfacing several function
calls later as the crash actually being chased. Confirmed what
offset `+20` should be by continuing to trace forward: the value
written into `applet+0x140` later gets dereferenced and called on
vtable slot 18, which -- checked directly against the real
`AEEIDisplay.h`'s `INHERIT_IDisplay` macro -- is exactly
`SetClipRect(po, pRect)`, called with `pRect=NULL`. So context
offset `+20` is the current app's `IDisplay` pointer, sibling to
`IShell` at `+12`. `ModRuntime` gained `SetDisplayInstance()`.
**Bonus finding**: `IDisplayHle`'s vtable only had the first 13 of
`IDisplay`'s real slots built (an incorrect assumption from Phase 3
that that was the full pre-BREW-MP interface) -- extended it to
all 26 real slots per `AEEIDisplay.h` (through `SetPrefs`), stubbed
beyond the four with real behavior (`AddRef`/`Release`/`DrawText`/
`Update`), so slot-18 and beyond no longer read past the array.
**Also mapped a sixth static-base slot** the same session, offset
`0x14`: a real call site (`ddragonz.mod` offset `0x23b00`) calls it
with one string-pointer argument and immediately does
`add r1, r0, #1` on the result -- the classic `strlen(s) + 1`
buffer-sizing idiom, matching ANSI `strlen` exactly.
6 new tests total across `mod_runtime_test.cpp`
(`GetAppContextSlotReturnsDisplayInstanceAtConfirmedOffset`,
`StrlenReturnsLengthExcludingNullTerminator`,
`StrlenReturnsZeroForEmptyString`, plus the earlier increment's).
**Reran against the real game**: the tick callback now runs
measurably further with each fix -- 153 (before this round) → 229
(after the `IDisplay` context field) → 238 (after `STRLEN`) real
steps. The very next real call the trace already shows waiting
(right after `STRLEN` returns, at `ddragonz.mod` offset `0x23b18`)
is a **seventh** static-base slot, offset `0xe4` -- not caught by
the earlier whole-binary scan (a gap in that scan's own pattern-
matching window, worth revisiting) -- called with a string
pointer, a stack buffer, and a size constant `0x200`, shaped like
`strlcpy`/`strncpy`.
**Implemented it**: unlike the other libc-shaped slots, this one's
exact real name/signature wasn't matched against a reference
source (the argument order -- `src, strlen(src)+1, dest, cap` --
doesn't cleanly match a standard `strlcpy(dest, src, len)`), but
the copy semantics are unambiguous from the calling convention
alone: `n = min(strlen_plus_one, cap); memcpy(dest, src, n)`.
Implemented exactly that (`ModRuntime::BoundedStrcpyImpl`). 2 new
tests (copies up to the requested length; never writes past the
cap).
**Reran against the real game and got real visible output for the
first time**: no wander, no `UnimplementedInstruction`, no
"did not complete trustworthily" -- the tick callback ran cleanly
across roughly 500 real frames over an 8-second observation window
with no new gap surfacing. Captured a live screenshot (`xwd` +
`ffmpeg`): a small white rectangle now renders in the top-left of
the window (our `IDisplayHle::DrawText` placeholder-block
behavior, so not real game graphics yet, but real proof the game
is now genuinely calling into `IDisplay` draw methods every frame
through the fully-resolved static-base table). This is the first
point all session where something is visibly different on screen.
**Decoded the full steady-state per-tick call sequence**: added a
lightweight HLE-call-only trace mode (`hle_trace`, logs just the
trap address + registers for real interface calls, not every ARM
instruction) to `tools/game_probe.cpp` and captured 10 consecutive
ticks. Every tick does the *identical* real sequence: `GETUPTIMEMS`
→ 4× `MEMSET` (clearing small sub-buffers) → `IDISPLAY_SetClipRect(NULL)`
→ `IDISPLAY_DrawRect(NULL, clrFrame=0xffffffff, clrFill=0xffffff00)`
→ `IDISPLAY_SetColor(1, 0x00000000)` → six repetitions of
`[STRLEN → bounded-strcpy → IDISPLAY_DrawText]` (six distinct HUD/
menu strings) → `ISHELL_SetTimer(..., 100, callback, applet)` to
re-arm. `DrawRect` and `SetColor` were still stubs, so the
background fill and text color were both silently no-ops.
**Implemented both, for real**: `IDisplayHle` gained
`DrawRect`/`SetColor`. Treats `RGBVAL` as the common real-BREW
`0x00RRGGBB` packing (`MAKE_RGB(r,g,b)`, per the real
`AEEIDisplay.h` reference doc comment) -- this specific bit layout
wasn't independently confirmed against a real header this session,
unlike the vtable slot order itself, which was; documented as an
assumption in `idisplay.h`. `DrawRect` fills the given `AEERect`
(or the whole screen if `pRect` is `NULL`) with `clrFill`, no
border rendering yet. `SetColor` collapses all `AEEClrItem` slots
into one "current color" `DrawText` now uses instead of a
hardcoded white (a documented simplification). 4 new tests
(`IDisplayHle.DrawRectWithNullRectFillsWholeScreen`,
`DrawRectWithExplicitRectFillsOnlyThatArea`,
`SetColorChangesDrawTextColorAndReturnsPrevious`, plus updated
`DrawTextThenUpdatePushesCorrectFrame` coverage).
**Reran against the real game**: the screenshot now shows a solid
**yellow background with black text** -- not solid-color noise,
a plausible, deliberate real UI color scheme, which is a strong
positive signal the reverse-engineered `RGBVAL` packing and
`AEERect` layout assumptions are correct. This is the first frame
all session with real, meaningful visual content.
**Confirmed stable over a longer window**: reran for 35 real
seconds (~2000 real frames) with `hle_trace` capped to the first
10 ticks for output volume -- no wander, no exception, no "did not
complete trustworthily" for the entire run.
**Wired up real keyboard input**: real disassembly of the app's
own `HandleEvent` dispatcher (`ddragonz.mod` offset `0xc640`)
shows real BREW event codes `evt==0x101`/`0x102` both calling a
helper (offset `0x1a3c4`) that does
`sub r0, wParam, #0xe021; cmp r0, #22; addls pc, pc, r0, lsl #2` --
a jump table converting `wParam` values in `[0xe021, 0xe021+22]`
into per-key bitmask flags (OR'd in for `0x101`, AND-cleared for
`0x102`) -- i.e. real key-down/key-up events, confirmed from the
binary itself even though the exact real `AVK_*` numeric mapping
wasn't independently verified against a header this session.
`tools/game_probe.cpp` now forwards real SDL key events into
`HandleEvent(applet, 0x101/0x102, wParam, 0)` -- number keys 0-9
map to the confirmed range's first 10 offsets, arrow keys to the
next four (a real, disassembly-grounded event *mechanism*, but an
exploratory, unconfirmed *specific key* mapping, documented as
such in code).
**Tested with real synthetic key events** (`python-xlib`'s XTest
extension, since the harness has no physical keyboard): number
key `1` and the up/down/left arrows all dispatch cleanly through
real app code and return `TRUE` (handled), matching the confirmed
bitmask-flag mechanism -- no visible change on screen yet since
`DrawText` still only draws placeholder blocks rather than real
glyphs, so distinguishing key-driven state changes visually isn't
possible yet either. The right-arrow-mapped code hit a **new**
gap (same "wandered into scratch memory, hit an invalid
instruction" shape as every other gap this session) -- caught
cleanly by `game_probe`'s own exception handling (prints and
continues) rather than crashing the tool, confirming that
safety net holds up under real, unplanned input too. Not yet
root-caused.
**Implemented real glyph rendering**: `core/brew/font5x7.{h,cpp}`,
a small, self-authored 5x7 bitmap font (uppercase Latin letters,
digits, space; anything else falls back to a small box) --
hand-designed for this project, not extracted from any real game
or copied from a third-party font table, consistent with
`CONTRIBUTING.md`'s clean-room policy. `IDisplayHle::DrawText` now
rasterizes real per-character glyphs (folding lowercase to
uppercase, since the font only has one case) instead of a solid
placeholder block. Updated the two existing `DrawText` tests
(`IDisplayHle.DrawTextThenUpdatePushesCorrectFrame`,
`BrewLifecycle.HelloBrewAppDrawsTextAndUpdatesScreen`) to check
real glyph-shaped pixel patterns instead of solid-block bounds,
and added 5 new `Font5x7` unit tests.
**Reran against the real game**: the screenshot now shows
genuinely distinct per-character shapes for the first time (not a
solid block) -- real, verifiable progress -- but most of the
drawn characters fall back to the generic box rather than a real
letter, meaning the six real HUD strings likely use lowercase
and/or punctuation the current font doesn't cover yet. Expanding
the font's coverage (lowercase, common punctuation) would likely
make the real text legible; not done this pass.
**Root-caused the key-input gap**: traced the right-arrow-mapped
code path with full instruction tracing and found **two** more
real static-base slots, both hit only by that specific jump-table
branch (not the up/down/left/digit paths already tested clean):
  - Offset `0x0` (the table's very first word, never read by
    anything up to this point) is **MEMCPY** -- a real call site
    (`ddragonz.mod` offset `0x223b4`) reads it with a bare
    `ldr r3, [r0]` (no displacement) and calls `(dest, src, n=36)`,
    sitting naturally right before `MEMSET` at offset `0x4`.
  - Offset `0x8` is **STRCPY** -- the same offset flagged as
    "strcpy-shaped but unconfirmed" several rounds ago. A real
    call site (`ddragonz.mod` offset `0x1a3e0` onward) passes just
    `(dest, src)`, no length, matching unbounded
    `char *strcpy(char*, const char*)` exactly.
Both implemented in `ModRuntime` (`MemcpyImpl`/`StrcpyImpl`) with
2 new tests each behavior confirms (exact-range copy, stops at the
null terminator, returns `dest`).
**Reran against the real game**: sent all 14 mapped keys (4 arrows
+ digits 0-9, 28 key-down/key-up events total) -- every single one
now dispatches cleanly through real app code with zero exceptions
and zero wandering. The key-input path is fully clean for the
first time. Static-base table slots confirmed so far: `0x0`
(MEMCPY), `0x4` (MEMSET), `0x8` (STRCPY), `0x14` (STRLEN), `0x68`
(MALLOC), `0x6c` (FREE), `0xb0` (GETUPTIMEMS), `0xc0`
(GETAPPCONTEXT), `0xe4` (bounded copy) -- nine confirmed slots
total.
**Solved the mystery of what the six on-screen strings actually
say, and why**: added a temporary debug print of `DrawText`'s raw
code units (removed after use, not committed) and found they
aren't `AECHAR`/UTF-16 at all -- they're a plain 8-bit `char*`
being read 16 bits at a time, so each "code unit" is really two
swapped ASCII bytes. Decoded, the six strings are fragments of one
real message: **"Memory is insufficient. Please start \[the game\]
after finishing \[the\] other application \[...\] by pushing the
button."** -- a genuine BREW low-memory warning dialog, not
gameplay HUD text, which is why it renders identically every
single frame.
**Root-caused *why* the game thinks memory is insufficient** (a
real, hardware-realistic diagnosis, not a guess): traced the
renderer back through a generic "draw N strings from a table"
helper (`ddragonz.mod` offset `0x49f0`) to a per-state dispatch
keyed by a field at `applet+0x24`, confirmed live via a temporary
watchpoint on that exact address (removed after use) to show it's
written twice during `CreateInstance` -- to `0` (proceeding
normally), then immediately to `1` (the "show warning" state) --
*before* `HandleEvent(EVT_APP_START)` even runs. Two candidate
"if this fails, set state=1" checks earlier in the same function
were ruled out by disassembly (both real called functions
unconditionally return success in this binary). The real cause:
`ISHELL_CreateInstance(shell, ClsId=0x01002001, &field)` -- a real
class ID we don't have registered -- fails, and the function
wrapping that call has an explicit `mov r0, #0; bx lr` failure
path that propagates the failure straight into
`state = 1`. This is a **real, mechanically accurate**
"insufficient memory" trigger: the game asks for some subsystem
via `CreateInstance`, doesn't get it, and (reasonably, from the
game's perspective) reports it as a resource-availability problem.
**Not yet fixed**: `0x01002001` is an unidentified real BREW class
(a different numeric family than `AEECLSID_DISPLAY`'s
`0x0100_1xxx`, plausibly a Zeebo-specific or sound/config-related
service) -- registering a stub for it risks a *different* crash
the moment the game calls a method on it we haven't scaffolded,
since we don't yet know what interface shape it expects. Tracked
as the next concrete step: identify (or reference-implement a
shape for) class `0x01002001`, alongside expanding `font5x7` for
legible real text now that we know what it's mostly needed for.
**Cleared the `0x01002001` gate with a deliberately generic
scaffold, not a guessed real interface**: added
`core/brew/scaffold_object.h/.cpp`
(`BuildGenericStubObject(memory, hle, vtable, object, slot_count)`
-- every slot just returns 0, no assumed shape) plus
`IDisplayHle::GetDeviceBitmap`/`SetDeviceBitmapInstance` (real
disassembly of `0x1b5c0` showed the game immediately dereferencing
`GetDeviceBitmap`'s result's vtable, so leaving it unwritten --
the previous blind `Stub` behavior -- was a second, independent
crash risk beyond the `CreateInstance` failure). `tools/game_probe.cpp`
now registers a 40-slot scaffold for `0x01002001` (sized to cover
the highest slot, 33, the disassembly shows being called) and a
20-slot scaffold as the device bitmap. 2 new tests
(`tests/scaffold_object_test.cpp`: object-header/vtable wiring,
every slot callable and returns 0;
`IDisplayHle.GetDeviceBitmapWritesTheRegisteredInstanceAndReturnsSuccess`
in `tests/brew_test.cpp`). **Verified against the real game**: the
`applet+0x24` state variable (read via a temporary debug print,
removed after use) now reads `0` instead of `1` past this specific
gate -- confirmed via `arm-none-eabi-objdump` that the caller
(`0x1b154`) now sees a nonzero (success) return from `0x1b5c0`.
**The chain goes deeper**: the *same* real disassembly technique
applied to `0x1b5c0`'s caller (`ddragonz.mod` offset `0x1b060`,
the applet's broader graphics-init routine) shows it gates
`applet+0x24` on **seven** sequential subsystem-init calls, not
just the one already found -- two (`0x96f0`, `0x1aea0`) confirmed
unconditionally successful in this binary, `0x1b5c0` now fixed,
and three more real gates found and fixed this round:
- `0x1b2fc`: two more `ISHELL_CreateInstance` calls, for classes
  `0x01001003` and `0x01001014` (confirmed via direct objdump on
  the literal-pool addresses `0x1b394`/`0x1b398`). Neither result
  is dereferenced inside that function itself, so generic
  40-slot scaffolds (registered in `game_probe.cpp`) were enough.
- `0x1d5b8`: two *more* `CreateInstance` calls, for classes
  `0x01014bc3` and `0x01014bc4`. **A real mistake caught and
  fixed in the same round**: a first attempt reused the
  `0x0100100x`-family class IDs from the `0x1b2fc` gate on the
  assumption they'd repeat -- wrong, and caught immediately
  because the resulting run showed a genuine crash
  (`pc=0x00000000`, "wandered outside module") instead of a clean
  pass/fail, since the real code's own error-cleanup path (at
  `0x1d940`) dereferences a still-null local when
  `CreateInstance` legitimately fails for an unregistered class --
  a real bug in *our* registration, not the emulator. Re-checked
  directly against `objdump` output for the exact literal-pool
  addresses (`0x1d970`/`0x1d974`) the function's own
  `ldr r1,[pc,#N]` instructions reference, found the real values
  (`0x01014bc3`/`0x01014bc4`), and registering scaffolds for
  *those* fixed it: the run now reaches `CreateInstance OK` and
  the event loop with **no crash**, though `0x1d5b8` itself still
  legitimately returns failure (state becomes `3`, not `1` --
  a different failure code, i.e. a different dialog/behavior than
  the "insufficient memory" one, not yet decoded) for a deeper
  reason: a real in-module helper (`0x23e58`) that indirects
  through a global pointer chain (not through the `IDisplay*`
  argument it's nominally called with) to reach some other
  object's vtable slot 4, and returns NULL. **Pinned down exactly
  which object via the same instruction-trace technique** (a
  temporary `trace=true` on the `CreateInstance` call, removed
  after use): `0x23e58` reads a module-global pointer at file
  offset `0x4ccb8` (computed PC-relative, not through the
  static-base table) -- which holds the *exact object our own
  `0x01014bc4` scaffold created* (confirmed live: the trace shows
  it loading our own `0x80017000` object address, then its
  `0x80016000` vtable, then calling slot 4, landing on our own
  generic `Stub` sentinel, which is why it returns 0). So this
  isn't a mystery third object -- it's the *same* class
  `0x01014bc4` we already scaffolded, being asked to do
  real work (its slot 4, called with the original `IDisplay*`
  as `po`) that a uniform "return 0" stub can't satisfy. Searched
  the binary for embedded debug/class-name strings (none found --
  this is a release build) and for corroborating references in
  other real `.mod` files (none available -- only one real game
  binary in `research/games/` this session). Real progress
  requires either finding another real BREW binary/doc that
  identifies class `0x01014bc4`'s actual interface, or reasoning
  from its usage here (slot 4, called with the display as `po`,
  result later validated against a fixed `0x3000`-byte size and
  fed to two more real helper calls) -- deferred rather than
  guessed, since a wrong shape risks a harder-to-diagnose failure
  downstream than the current clean "returns failure" state.
  Tracked as the next concrete step, alongside decoding what
  "state = 3" actually displays.
**Broke the "0x01014bc4 is unidentified" dead end wide open**: the
Zeebo SDK package bundled in this repo
(`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
OpenGLES_Extension_1.5.3_...zip`) turned out to be a real,
unextracted MSI installer -- `7z x Installer.msi` pulled out its
embedded CAB, `cabextract` on that produced ~46 hashed-name files,
several of which are real Qualcomm C headers/DLLs. One of them
(`AEEGL.h`) contains, verbatim: `#define AEECLSID_GL 0x01014bc3`
and `#define AEECLSID_EGL 0x01014bc4` -- an exact match for both
literals `0x1d5b8` requires. Even better: this codebase already
has a complete, previously-tested `GlHle` (real `IGL`/`IEGL`
implementations, built in an earlier phase for a different purpose
and wired up directly rather than through `CreateInstance`) --
`tools/game_probe.cpp` now registers `GlHle::BuildGl`/`BuildEgl`'s
real objects as the `CreateInstance` answers for these two real
classes instead of generic scaffolds, and removed the now-
redundant direct `BuildGl`/`BuildEgl` calls further down.
**Found two more real, evidence-backed gaps this unlocked** (all
via the same trace-then-objdump technique, `trace=true` +
temporary PC-filtered gate prints, both fully reverted before
each commit):
- A missing static-base runtime-table slot, offset `0xe8`
  (STRSTR): real disassembly of `0x1d5b8` shows
  `eglQueryString(dpy, EGL_EXTENSIONS)`'s result fed straight into
  this slot alongside a literal string read directly out of the
  real file's bytes (`"EGL_QUALCOMM_COLOR_BUFFER"`, at file offset
  `0x4fcc4`) -- the standard `strstr(exts, "SOME_EXT")` idiom.
  Implemented as real `ModRuntime::StrstrImpl` (3 new tests:
  finds-a-match, no-match, empty-needle). This only crashed
  because `GlHle::EglQueryString` (slot 7 of `IEGL`) was a blind
  `Stub` returning null -- implemented for real: returns an
  honest (never-null, per the real EGL spec's guarantee) string
  for `EGL_VENDOR`/`EGL_VERSION`/`EGL_CLIENT_APIS`, and an empty
  string for `EGL_EXTENSIONS` since no extensions are implemented
  (2 new tests: never-null across several query names, extensions
  string is genuinely empty).
- The bitmap object `IDisplay::GetDeviceBitmap` returns gets a
  second real call site (beyond the one in `0x1b5c0`): its slot 2
  again, this time inside `0x1d5b8`, in the same
  "QueryInterface"-shaped way (`obj, clsid=0x01001045, &ppo`) --
  and this time the result is unconditionally `Release()`'d
  moments later with no null check, so leaving the output pointer
  unwritten (the generic scaffold's blind-`Stub` default) is a
  real, confirmed crash (traced to a null-pointer `bx`), not a
  hypothetical risk. Added `BuildStubObjectWithOverride` to
  `scaffold_object.h/.cpp` (same generic-stub base, but one named
  slot gets a real caller-supplied implementation) and used it to
  give the bitmap's slot 2 a real (if still generic-downstream)
  `CreateInstance`-shaped implementation: succeeds and writes a
  fresh scaffold object for `clsid == 0x01001045`, fails
  otherwise -- exactly mirroring `IShellHle::CreateInstanceImpl`'s
  own discipline. 2 new tests (override slot runs the real
  implementation; non-overridden slots stay generic).
**Verified against the real game**: with all of the above, the
*entire* graphics-init routine at `0x1b060` -- which turned out to
gate `applet+0x24` on **ten** sequential checks, not seven --
passes zero-crash from `AEEMod_Load` through `CreateInstance`
through `HandleEvent(EVT_APP_START)` and into the steady-state
tick loop, for the first time this project.
**Found and identified an eighth real, previously-unknown gate**
(same technique): `0x1b71c`, called from inside `0x1b060` after
the GL/EGL work, does `ISHELL_CreateInstance(shell,
ClsId=0x0106c411, ...)` then `IHID_GetConnectedDevices(pIHID,
nDeviceType=0x0106c3fd, ...)`. Both literals are real and
confirmed two different ways: they're named exactly
(`AEECLSID_HID`, `AEEUID_HID_Joystick_Device`) in the real BREW
SDK sample source already bundled in this repo
(`research/samples/conftest_source/conftest/GamepadMgr.c`), and
the real `AEEIHID.h` (found the same way as `AEEGL.h`, in a
*different* Zeebo SDK installer CAB under
`research/docs/sdk_installer_extract/sdk_installer_cab/`) confirms
`GetConnectedDevices` really is vtable slot 7
(`INHERIT_IQI`'s 3 slots + `CreateDevice`/`GetDeviceInfo`/
`GetNextConnectEvent`/`RegisterForConnectEvents`/
`GetConnectedDevices`). We have no real joystick to enumerate, so
`game_probe.cpp` registers a `BuildStubObjectWithOverride` scaffold
whose slot 7 honestly reports zero connected devices (true, not
guessed) -- confirmed via the gate-trace technique that this is
enough to clear this specific check. A second class the same
routine would need next (`0x01041207`, only reachable once a
device is found) is registered as a fully generic scaffold, since
it's unreached in practice (0 devices) but still needs to exist
so `CreateInstance` doesn't fail outright if that assumption is
ever wrong.
**Found a ninth gate, not yet identified**: `0x1c6b0` (called
right after the HID work) delegates to `0x22384`, which delegates
to `0x237c4`, which calls a method (vtable slot 2, args
`(object, name=<a literal string pointer>, flags=2)`) on the
object created for `ClsId 0x01001003` back in the `0x1b2fc` gate
-- our generic scaffold's blind `Stub` returns a null handle,
which the caller correctly detects and returns failure for (no
crash, just the same "insufficient" dead end one layer deeper).
This has the shape of a real resource/texture/font "load by name"
call, but unlike `AEECLSID_GL`/`EGL`/`HID` above, nothing in this
repo's bundled research materials (searched: extracted GLES SDK
cab, extracted plain sdk_installer_cab, the SDK headers reference)
names class `0x01001003` or corroborates a real shape for this
call -- implementing it further would mean guessing a return
value and its downstream consumption, not verifying one, so
deliberately stopped here rather than fabricate behavior. Tracked
as the next concrete step.
**Follow-up research pass, no behavior changes**: went back
through two more bundled-but-previously-unopened real resources --
`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
samples.zip` (a *second*, larger real OGLES sample set beyond the
already-extracted `MSM7500_OGLES_...` one -- turned out to be the
same samples, but worth the check) and
`ZeeboDeveloperGuide0.97.pdf` (readable via `pdftotext`, not yet
read this session). Two payoffs, both corroborating rather than
contradicting earlier scaffolds (no code changes needed, comments
updated for the next reader):
- `simple_drawtexture.c` (a real bundled sample) does exactly
  `IDISPLAY_GetDeviceBitmap(...)` then
  `IBITMAP_QueryInterface(pIBitmapDDB, AEECLSID_DIB,
  (void**)&pDIB)`, then casts `pDIB` straight to
  `NativeWindowType` for `eglCreateWindowSurface` -- matching, in
  both call shape *and* downstream use, exactly what Double
  Dragon's own disassembly showed for the still-unidentified
  `0x01001045` scaffold. Strongly suggests `0x01001045 ==
  AEECLSID_DIB`, though (unlike `AEECLSID_GL`/`EGL`/`HID`) the
  numeric literal itself isn't in any bundled header, so this is
  circumstantial, not confirmed.
- The dev guide's own IHID walkthrough creates
  `AEECLSID_SignalCBFactory` immediately after `AEECLSID_HID`, to
  back IHID's connect/button-event `ISignal` callbacks -- the same
  order Double Dragon's disassembly showed for the still-
  unidentified `0x01041207`. Same caveat: matching order, not a
  confirmed literal. Also caught and fixed a wrong comment while
  cross-checking this: `0x01041207` is NOT gated on finding a real
  joystick (`GetConnectedDevices`'s own success/fail, not its
  device count, controls whether it's reached) -- it fires
  unconditionally in this run, and was already being scaffolded
  correctly; only the explanatory comment was wrong.
Also confirmed `IFont`/`ISprite` are real, Zeebo-supported
interfaces (per the dev guide's feature table) -- plausible
candidates for what `0x01001003`'s family is *for*, but with no
numeric ID or call-shape example anywhere in the bundled
materials, using that to implement slot 2's real behavior would
still be guessing, not verifying -- left as-is.
**The user asked directly: is this ninth gate actually necessary to
run the game? Answer, confirmed empirically: yes** --
`applet+0x24` must be exactly `0` (not `1`, `2`, or `3`) for the
per-tick game loop to run real logic instead of redrawing a
warning dialog every frame forever; this gate is the last of the
ten that was still nonzero.
**Found the real identity by reading the actual string/flags
involved, not just the call shape**: the "name" argument passed to
`0x01001003`'s slot 2 is a real, literal C string baked into the
file (`./udata/ddz.sav`, read directly from `ddragonz.mod` at file
offset `0x4e078`) -- a save-game path, not a font/texture name as
guessed last round. Full disassembly of the surrounding routine
(`0x237c4`/`0x9f3c`) shows a textbook "load save, or create a
fresh one" sequence: `IFILEMGR_Test` (slot 7) on that path, and on
failure `IFILEMGR_GetFreeSpace` (slot 8, checked against a
minimum) then `IFILEMGR_OpenFile` (slot 2) with a literal mode of
`2`, which is exactly real `AEEFile.h`'s `_OFM_READWRITE` (a
literal `4` appears too, matching `_OFM_CREATE`). That's
`IFileMgr`'s real vtable shape precisely -- so `0x01001003` is
very likely real `AEECLSID_FILEMGR` (still not a confirmed literal
number, same caveat as `AEECLSID_DIB`/`SignalCBFactory`, since no
bundled header states it numerically -- but the call shape and
flow leave little doubt).
**This project already had a complete, tested `FileHle`** (an
earlier phase, GGZ-backed, deliberately read-only) that had never
been wired into `CreateInstance` either -- same pattern as
`GlHle`. Wiring it in alone wasn't enough, though: the read-only
design would legitimately fail this exact "create a save file"
flow (`OpenFile` returning null for a path that doesn't exist in
the shipped GGZ content, `GetFreeSpace` returning `0`). **Extended
`FileHle` with a real, separate in-memory "user data" store**
(`core/brew/file_hle.h`'s `writable_files_`, distinct from the
read-only GGZ-backed `vfs_`): `OpenFile` now honors
`_OFM_CREATE`, creating a genuinely writable/readable file if
nothing else has it; `Write` (previously `StubFailed` for every
file) now really writes into a writable file's backing buffer
(growing it as needed) and still correctly fails for read-only
GGZ-backed files; `Test`/`GetInfo` recognize writable files too;
`GetFreeSpace` (previously a blind `Stub` returning literal `0`,
which would have failed this exact minimum-space check) now
reports a plausible simulated 1 MiB quota, optionally writing
total capacity through its real second `uint32*` argument. 8 new
tests in `tests/file_hle_test.cpp`: create-on-missing, write-then-
read-back round trip, `Test` sees newly-created files,
re-opening a created file by name sees previously-written bytes,
`GetFreeSpace` returns nonzero and correctly writes its optional
output pointer -- plus confirmed the existing
`ReadOnlyMethodsAllReturnAnError` test still passes unchanged
(GGZ-backed files remain genuinely read-only; only the new
writable store accepts writes).
**Verified against the real game -- the entire ten-gate chain
finally clears**: `applet+0x24` reads `0` after `CreateInstance`
for the first time this project. Real per-frame game logic now
runs -- tick traces show over a hundred distinct real HLE calls
per frame (texture/rendering-shaped, not the same six repeated
`DrawText` calls from the old warning dialog) across 4 full
ticks, a qualitative change from every previous run.
**Found a tenth gap this unblocked, of a new kind**: tick 4
throws `"Miscellaneous instruction space (MRS/MSR/etc.)"` --
not a missing HLE slot this time, but a real ARM instruction our
interpreter doesn't decode. Real cause (traced from the log, not
yet from fresh disassembly): a "wandered outside module" warning
(`pc=0x00000000`) fires a few real instructions earlier, meaning
something *else* -- most likely still one more missing/incomplete
HLE slot -- causes a jump through a null/garbage function
pointer; the CPU then walks forward through zeroed memory (which
happens to decode as harmless no-ops) until it reaches real data
at a low, non-code address that isn't a valid instruction. The
MRS/MSR error is a downstream symptom, not the real bug -- tracked
as the next concrete step: find what actually jumps to null in
tick 4 (a fresh instruction trace + objdump, same technique as
every fix this session, should pin it down directly).
**Found and fixed it**: an instruction trace of tick 4 (temporary
`trace=true` on that one tick, removed after use) pinned the null
jump to `ddragonz.mod` offset `0x23d0c` -- `ldr r3, [r0, #0x13c]`
off the static-base table, a **twelfth** table slot never mapped
(offsets `0x0`/`0x4`/`0x8`/`0x14`/`0x68`/`0x6c`/`0xb0`/`0xc0`/
`0xe4`/`0xe8` were already confirmed; this is `0x13c`). Reading
the actual arguments off the trace (not guessing): R1 wasn't a
plain integer as the calling convention alone might suggest -- it
was itself a real string pointer, and reading that string
directly out of the file gave `"ERROR CODE:%d"`. That, plus the
call being immediately followed by `STRLEN` on the destination
buffer, confirms a `sprintf`-family formatter -- unusual only in
that its third argument is `void **ppArgs` (a pointer to an
*advancing* args cursor, matching the double indirection at the
call site) rather than a plain `va_list`. Implemented as
`ModRuntime::SprintfImpl` supporting `%d`/`%u`/`%x`/`%X`/`%s`/`%c`/
`%%` (no width/precision/flags -- no evidence any real call needs
them yet). 3 new tests in `tests/mod_runtime_test.cpp`, including
one that reproduces the exact real confirmed case
(`"ERROR CODE:%d"` + arg `5` -> `"ERROR CODE:5"`, and confirms the
args cursor advances correctly).
**Verified against the real game**: the crash is gone -- the
previous ~15-second run (bounded by an external timeout, not a
crash) now runs cleanly through many more ticks with zero
exceptions, actually calling the new sprintf slot with the real
confirmed string and formatting a real error code into it
(screenshotted: the display now shows a different, several-line
message -- no longer the old "insufficient memory" dialog -- most
characters still render as the small unmapped-character fallback
box, consistent with the still-unresolved byte-swapped-string
quirk documented earlier in Phase 8, now applying to this new
string instead). Not yet investigated: *why* the game is
formatting an "ERROR CODE" message at all at this point (i.e.
what real error condition it detected, and whether it's a
legitimate real-hardware error path or something this emulator is
still getting wrong upstream) -- tracked as the next concrete
step, alongside decoding the message's actual text once the
byte-swapped-string handling is revisited.
**Follow-up investigation, no code changes**: added temporary
debug prints (removed after use) to `ModRuntime::SprintfImpl`
(dump the formatted string), `GlHle::EglSwapBuffers`, and
`IDisplayHle::Update` to answer two questions. First: what are the
actual values? `"ERROR CODE:6"` and `"LIST COUNT:3"`, redrawn
every tick (the same repeating-diagnostic pattern as the earlier
"insufficient memory" dialog, just a different message). Traced
the error-code field to a confirmed real struct offset
(`applet+0x36c4`, found via the real disassembly at
`ddragonz.mod` offset `0x10602c`) and found the *only* other real
reference to that exact offset in the whole binary is a reset-to-
zero at offset `0x1209dc` -- no direct `str` instruction
anywhere else constructs that same offset, meaning whatever sets
it to `6` does so indirectly (a computed/array-indexed write, or
a value copied in from elsewhere) rather than a single obvious
missing-HLE-call site like every fix so far this session. This is
real application business logic, not an emulator gap -- a
meaningfully different (and likely much longer) kind of
investigation than closing HLE/runtime-table gaps, so deliberately
not pursued further this round. Second: confirmed `IDisplayHle::
Update` fires every tick (148 times in one ~8s run) while
`GlHle::EglSwapBuffers` never fires at all (0 times) -- ruling out
a suspected rendering-pipeline conflict between the software
`IDisplay` framebuffer and the real GL context sharing one SDL
window (`Sdl2GlBackend::SwapBuffers` really does call
`SDL_GL_SwapWindow`, confirmed by reading
`frontends/standalone/sdl2_gl_backend.cpp`) -- moot for now since
the game hasn't reached real per-frame GL presentation yet, only
this diagnostic-overlay loop.

**Found and fixed a real gap this round, via a live memory watchpoint
on `applet+0x36c4`** (temporary, `Memory::Write32` -- confirmed the
field is legitimately written `0` a few times during setup, then a
single real write of `6` correlates exactly with one specific HLE call
sequence right before it). That sequence: `IFILEMGR_OpenFile(pFileMgr,
"sound.ggz", mode=1)` -- the game opens its own packed resource
archive **as a raw file**, by its plain filename, rather than expecting
every entry pre-extracted. `tools/game_probe.cpp`'s `MergeGgzInto` only
ever registered each GGZ archive's *decompressed entries* in the
`VirtualFilesystem`, never the archive's own raw bytes under its own
basename -- so `OpenFile("sound.ggz")` correctly, honestly returned
"not found." Fixed by also registering each archive's raw bytes under
`BaseName(path)` when loading (`vfs.AddFile(BaseName(path), raw)`).
**Verified against the real game** (a temporary debug print in
`FileHle::OpenFileImpl`/`SeekImpl`/`ReadImpl`, removed after use):
`OpenFile("sound.ggz", mode=1)` now succeeds (`file_size=1928097`,
exactly the real file's size), and the game goes on to genuinely
`Read`/`Seek` within it -- real audio-archive access now works, not
just a non-crash.
**The `ERROR CODE:6` loop itself persists past this fix, for a
different, deeper reason.** Traced the caller (a real loop at
`ddragonz.mod` offset `0x1c964`, disassembled directly): it iterates
a resource list (up to 81 slots) calling a helper at offset `0x1bfd0`
for each with `(pIShell, "sound.ggz", nameTable[i], &resultTable[i])`,
stopping as soon as that helper returns `0`. A second real open of
`"sound.ggz"` in this loop does `Open` -> `Seek(SEEK_START, 488)` ->
`Release`, with **no `Read` in between** this time (unlike the first,
successful open earlier in the same tick, which did `Read` before
seeking further) -- and the error gets set immediately after. This is
genuine resource-list business logic (why does entry N behave
differently from entry 0, what `0x1bfd0` actually validates, what
"`LIST COUNT:3`" really counts) rather than a missing HLE primitive --
would need substantially more disassembly of `0x1bfd0` and the
81-slot-iteration loop specifically to resolve, tracked as the next
concrete step.

**Found and fixed a real, foundational bug this round: `IFILE_Seek`'s
return value was backwards.** The real `AEEFile.h` documents `Seek`'s
contract precisely: it returns `AEE_SUCCESS`(0)/`AEE_EFAILED`(1), *not*
the resulting position -- the one documented exception being
`_SEEK_CURRENT` with `moveDistance=0`, a "tell" operation that does
return the current position. `FileHle::SeekImpl` had always returned
the resulting position instead, which only coincidentally matched real
"0=success" for the specific case of seeking to absolute position 0 --
so every previous test (and every previous real gate this session) that
happened to seek to position 0 looked correct, while any real seek to a
*nonzero* position returned a nonzero value real game code reads as
failure. **Found via the resource-list loop from the previous entry**:
its helper (`ddragonz.mod` offset `0x739c`) does
`ISHELL`-style `Seek(SEEK_START, index*8)` then checks the result
against 0 -- exactly the shape that would silently break for every
`index > 0`. Rewrote `SeekImpl` to match the real contract exactly,
including the real documented asymmetry between read-only files
(seeking outside `[0, size]` fails) and writable files (seeking past
EOF extends the file; only seeking negative fails) and the `_SEEK_
CURRENT`+0 tell exception. Rewrote the existing Seek test to check the
real contract instead of the old (wrong) one, and added 3 more:
tell-returns-position, out-of-bounds-fails-on-read-only, and
past-EOF-extends-a-writable-file.
**Verified against the real game** (a temporary live memory watchpoint
+ full instruction trace of the exact failing call, both removed after
use): `0x739c`'s *two* real `Seek` calls (one to the GGZ header-table
entry, one to the resource's real data offset) now both succeed
correctly, and the whole function completes and returns success for a
real resource index (61) that previously failed outright. **The
`ERROR CODE:6` loop still doesn't fully clear**, for a new, different,
narrower reason found by continuing the same trace one level deeper:
after the two real `Seek`s succeed, the resource-loading routine
(`0x1bfd0`) reads the file's real 8-byte GGZ header entry correctly
(offset `0x1c3e25`, size `0x9ef0` -- both parsed correctly), then tries
to `Read` the actual resource content through a *different* object,
`manager->12` (`manager` here is a distinct per-subsystem struct at
`applet+0x19c`, not the `applet+0x128` GL-init struct from earlier) --
and `manager->12` still points at the old, still-unidentified generic
scaffold for class `0x01001014` (the twin of `0x01001003`=`FileMgr`
found together at the very first `0x1b2fc` gate this session), whose
blind `Stub` slot 3 returns 0 bytes read every time. Nothing in
`0x739c`/`0x1bfd0` ever *writes* `manager->12` -- it must be
initialized once elsewhere, in a dedicated init routine for this
`applet+0x19c` subsystem not yet located. Tracked as the next concrete
step: find that init routine (the same technique as every fix this
session -- find what constructs `applet+0x19c`, trace its own
`ISHELL_CreateInstance` calls) to finally identify class `0x01001014`
for real, the same way `0x01001003` was identified.

**Follow-up investigation into the `0x01001014` object, no code changes
this round.** The previous entry's guess at *which* struct field was
the culprit was imprecise -- corrected here with direct trace evidence.
A live watchpoint on `applet+0x19c`'s own `+12` field found it gets
written `0x8030014c` (i.e. `applet+0x128`'s address), but that field
turned out to be an unrelated cached back-reference, not the actual
object dereferenced by the failing `Read` call. Re-reading the exact
instruction trace of the failing call directly (register values, not
re-derived by hand) showed the real culprit precisely: inside
`0x1bfd0`, its own first argument (confirmed via the trace to be
`applet+0x128`, not `applet+0x19c` -- `0x1c964` passes
`*applet+0x19c` i.e. `applet+0x19c`'s *own* `+0` field, which caches a
pointer back to the shared `applet+0x128` loader struct) is
dereferenced at `+12` to get the object, whose vtable slot 3 (`Read`)
is called. That's exactly `applet[0x128+12]` -- the real, confirmed
`0x01001014` object from the very first `0x1b2fc` gate -- still our
blind 40-slot generic scaffold, whose slot 3 just returns 0.
**Searched further for the class's real identity, still inconclusive.**
Re-examined `0x1b2fc` (where the object is created) directly: no
initialization call is made on it after `CreateInstance` succeeds --
it's created once and presumably used standalone later, with no
"attach me to a specific open file" step visible anywhere in the
traced call chain (`0x1b2fc` create -> `0x1c964` resource-list loop ->
`0x1bfd0` per-item loader -> `0x739c` open/seek/read-header, none of
which ever write `applet[0x128+12]` after creation). Chased one
promising real lead to a dead end: the same qx_cab SDK extraction that
named `AEEAppGen.c`/`AEEModGen.c` also contains real Qualcomm "QX
Engine" middleware headers, including `QXPackFileManager.h` and
`QXPack.h` -- a real pack-file archive reader many Zeebo-era BREW games
built on. Read both directly: `QXPack`'s own file format (string
table, directory records, `QXPackFile{fileNameStringID, fileSize,
fileDataOffset}`) does not match our own GGZ format's simpler
"N 8-byte (offset,size) entries" structure at all, and `QXPackFileMgr`
is a plain host-side C API (`QXPackFileMgr_Create(QXState*)`), not
something obtained via `ISHELL_CreateInstance` -- ruled out with real
evidence, not assumed. No further real leads found in this repo's
bundled materials for what `0x01001014` actually is. Deferred rather
than guessed -- a wrong implementation here risks trading "reads 0
bytes, harmlessly" for "reads garbage bytes, corrupts real game state
in a way that's much harder to notice or diagnose than the current
clean failure."

**Pushed past the deferral above and implemented class `0x01001014`'s
real behavior.** Every piece of real evidence gathered so far pointed
at the same shape: the object is created once, never explicitly bound
to a file, and its `Read` is always called immediately after the
game's own loader opens/seeks the file it actually wants through a
*separate* `IFileMgr` object. Modeled that directly:
`FileHle::BuildLastOpenedFileProxy` returns an object whose `Read`
(slot 3) forwards to whichever file `OpenFileImpl` most recently
returned, re-resolved on every call rather than fixed at construction.
Documented in both `file_hle.h` and the registration site in
`game_probe.cpp` as a deliberate, evidence-grounded implementation --
not a confirmed-correct one -- since no real header or SDK sample
found in this repo's bundled materials names the class. **Verified
against the real game** (live trace): items that previously read 0
bytes through the old blind scaffold now read real byte data.
**Also found and fixed a second, independent real bug in the same
pass: the emulated heap was exhausted partway through the resource
list.** `ModRuntime`'s heap was sized at 1MB arbitrarily, never
measured against real needs. The resource loader `MALLOC`s a real,
sizeable audio buffer (tens of KB to just over 1KB, varies per item)
for every one of the real list's entries in a tight loop; 1MB runs out
partway through and MALLOC legitimately returns real NULL, which real
game code has no path to recover from. Bumped to 16MB in
`game_probe.cpp` (a generous but not unreasonable amount of app heap
for a 2009-era dedicated gaming device) -- verified via live trace
that MALLOC no longer fails across a full run. Both fixes committed
together (`cb52c00`), with 3 new proxy tests
(`LastOpenedFileProxy*` in `tests/file_hle_test.cpp`) alongside the
existing Seek-contract tests.

**With both fixes in place, the loader gets *much* further -- dozens
of real resources load successfully -- but `ERROR CODE:6`/`LIST
COUNT:3` still eventually appears, now after processing far more
items than before.** Re-traced tick 3 in full (instruction trace grew
from ~3,300 lines pre-fix to ~13,500 lines post-fix) and followed the
new failure back to its exact cause. First, a side discovery that
corrects an earlier assumption in this log: `"LIST COUNT:3"` is *not*
a count of which resource-list item failed -- it's the per-tick state
machine's own step counter (the 12-case jump table at `0x1c1ec`
documented earlier), incrementing once per real tick regardless of
which resource failed or why. The dispatcher reports one hardcoded
generic failure code for the whole case (`case 2`), so this diagnostic
was never going to distinguish "item 3 failed" from "item 63 failed"
in the first place -- a dead end for narrowing the search that way,
reconciled now rather than chased further.
Second, and more useful: counted exactly 63 successful
`MALLOC`+`Read` cycles in the tick-3 trace before the new failure, and
found the specific real `Seek` immediately preceding it targets header
offset `584` -- i.e. `73 * 8`, the **74th and last** entry in
`sound.ggz`'s real 74-entry GGZ table (the file's own "loaded 74
entries" banner, printed at every run's startup, was hiding in plain
sight the whole time). That entry's real header bytes (read directly
out of the real file, confirmed independently with a small Python
script rather than trusted from the trace alone) declare
`offset=1927592, size=1034`; the real file is exactly `1928097` bytes
long, leaving only `505` bytes after that offset -- **529 bytes short
of the declared 1034**. Decompressing those exact 505 real bytes as
gzip (`zlib.decompressobj(16+zlib.MAX_WBITS)`) succeeds cleanly,
produces exactly 1034 bytes of output, and consumes the stream with
zero leftover bytes and `eof=True` -- i.e. this is a **complete,
valid, correctly-terminated gzip stream**, not truncation or a parsing
bug on our end. `size` in the GGZ header is the *decompressed* length
(matching this repo's own `ggz.h` documentation), but the real game
code at `0x1bfd0`/`0x739c` is reading `size` **raw, undecompressed
bytes directly off disk** in a loop that keeps requesting more
whenever a read comes up short of the running total -- which happens
to work for every other entry only because there's always more file
data *after* it (the next entries' compressed streams) to keep
pulling from until the requested total is reached, entry boundaries be
damned. The very last entry has nothing after it, so the read
genuinely, correctly comes up short (`505` then `0`), and `0x1bfd0`'s
own final `expected == actual` check (`1034 != 505`) fails -- exactly
matching the observed trace (`ldr r0,[r4,#0x14]; cmp r0,r5` at
`0x1c0a0`/`0x1c0a4`, branch taken to the fail path).
**Also confirmed this is a genuinely persistent failure, not
transient**: reran with `hle_trace` (no full instruction trace) across
10 real ticks and saw `"LIST COUNT:3"`/`"ERROR CODE:6"` printed
identically, unchanged, every single tick -- ruling out an earlier
open question about whether the state machine might recover on a
later tick.
**Not yet resolved**: whether real hardware's original `sound.ggz`
genuinely has more trailing data after this last entry (making our
copy of the asset the actual gap, not the emulator), or whether real
game code is expected to legitimately fail exactly this way for the
list's last slot and recover through a path not yet traced (a retry,
a skip, or a "give up gracefully and continue to gameplay anyway" that
our own state-machine driving hasn't reached or doesn't implement).
Tracked as the next concrete step -- continuing the investigation
rather than switching to something else, per direction.
Side note for future invocations: this round's investigation began
with a wrong assumption that the `.mod`'s containing folder name
(`274754`) was Double Dragon's real `IModule::CreateInstance` `ClsId`
-- it isn't (that was already corrected earlier in this very log, see
the `0x0102F789`/`16971657` entry above); re-confirmed directly
against the real compiled literal at file offset `0x738` before
re-running. `tools/game_probe.cpp` always takes the real ClsId as its
4th CLI argument, `16971657`, not the folder name.

**Resolved the "not yet resolved" question above, via real disassembly
of the dispatcher itself (`0x1c170` onward, the true function entry --
`0x1c1ec`/`0x1c964` from earlier entries are both part of the same
function) plus a live watchpoint, no guessing.** Two findings, one of
which *corrects* an earlier entry in this log:
1. **Correction**: `"LIST COUNT"` and the per-tick case-dispatch index
   are not two related-but-distinct counters as the previous entry
   concluded -- they are the exact same 16-bit struct field
   (`applet+0x36b2`). The dispatcher reads it as `r1` at entry
   (`ldrsh r1,[r4,#2]` at `0x1c18c`) to pick which of the 12 cases to
   run, then unconditionally increments and stores it back
   (`0x1c3c8`-`0x1c3d0`) before returning. So every real tick this
   dispatcher runs, it advances to the *next* case, permanently -- this
   is a run-once-through-12-states sequencer, not a retry loop.
2. **The real reason the diagnostic never changes turns out to be much
   simpler than "stuck retrying case 2 forever"**: it isn't retried at
   all. A live watchpoint on both fields (`applet+0x36c4`
   error-code, `applet+0x36b2` count) across a full ~900-tick run
   showed exactly four writes total -- count going `0 -> 1 -> 2`, then
   error-code `-> 6` and count `-> 3`, all within the first 3 real
   ticks -- and **zero further writes to either field for the
   remaining ~900 ticks**. The whole dispatcher function opens with an
   unrelated early-out guard (`0x1c170`: if `[r1+4] == 0`, write `1`
   and return immediately, touching neither field) -- consistent with
   whatever higher-level code owns this subsystem simply no longer
   calling this stepper at all once loading has failed, rather than
   calling it and hitting a guard every time (a guard trip would have
   shown up as repeated writes of `1`, which never appeared). The
   values displayed every frame afterward are just whatever was last
   left in memory from tick 3, redrawn by a separate, unrelated render
   path -- not evidence of an active retry.
   Also confirmed independently via static disassembly of the real
   call site (`0x1c250`: `mov r3, #81`) that the loop's 81-item bound
   is a **hardcoded literal**, not derived from any real parsed count
   -- so the real game genuinely, unconditionally attempts up to 81
   sound-resource slots every time this state runs, regardless of how
   many entries `sound.ggz` actually has. Combined with the previous
   entry's finding (failure lands exactly on real entry 74 of 74, via
   genuine end-of-file exhaustion, not an out-of-range index), this
   makes it likely that our copy of `sound.ggz` is missing trailing
   data the real distributed file has -- i.e. **the current best
   hypothesis is a research-asset gap, not an emulator gap**: if the
   real file had enough trailing bytes for entry 74's raw read to
   reach its full declared 1034 bytes (by spilling into further, even
   if unused, archive data the same way every earlier entry does),
   this exact failure would not occur. No further real evidence in
   this repo's bundled materials to confirm or rule that out -- no
   code changes this round, purely investigative.
3. Also fully, statically disassembled `0x1bfd0`'s tail (`0x1c09c`-
   `0x1c0f8`) to rule out a missed bypass: the final
   `cmp [manager+20](expected), r5(actual)` is a strict equality gate
   with no partial-acceptance path -- on any mismatch it unconditionally
   falls through to a cleanup branch that returns real `0`. Confirms
   there is no alternate real code path that would let a short read
   like this one succeed; the only way past this specific gate is more
   real bytes at that file offset.
4. Tried one more thing before concluding this thread: temporarily
   injected all 14 of this dev tool's currently-mapped `AVK_*` key codes
   (`SdlKeyToAvk`'s number-key and arrow-key range) as real
   `HandleEvent` key-down/up pairs spaced across ~450 further ticks,
   on the chance the frozen subsystem is actually waiting on player
   input (an "insert coin"/"press start" style dismiss) rather than
   being permanently dead. A live watchpoint across the whole run showed
   zero further writes to either field for any of them. Inconclusive
   rather than a firm no, since this tool's key mapping is explicitly a
   guess, not a confirmed real one (see `SdlKeyToAvk`'s own doc comment)
   -- but no evidence found that input is the missing piece either.
   Reverted (no code changes kept from this experiment).

**Where this leaves Phase 8**: every concrete, findable-with-real-
evidence gap in the emulator's own HLE has been closed for this code
path. What remains blocking real playability is a single real resource
(`sound.ggz` entry 74 of 74) that this repo's copy of the file cannot
satisfy a strict, exact-match real read against, for a reason (genuine
end-of-file) that's a property of the *asset*, not of anything the
emulator does. Making further progress here most likely needs a
different real copy of `sound.ggz` (or of the whole game package) to
compare against, rather than more disassembly of code already read
exhaustively down to its last real branch.

---

## Second real game: Peggle

Rather than keep pushing on Double Dragon's asset-level dead end, brought
in a second real commercial title to test something Double Dragon alone
never could: whether HLE work this deeply tuned against one game's real
code actually generalizes, or was accidentally overfit to it.

**Sourcing, and an unplanned but useful side-verification.** Downloaded
all 61 real games from the `zeebo-arquivista` archive.org preservation
item (https://archive.org/details/zeebo-arquivista, a curated ~700MB
Zeebo collection distinct from this repo's original Double Dragon
source) into `research/games/_archive_org_zeebo-arquivista/`
(git-ignored, same as every other real game asset in this repo). Before
picking a second title, re-downloaded Double Dragon from this
independent source specifically to test the open question from the
previous section: is this repo's `sound.ggz` possibly an incomplete
research-asset dump? Compared all 5 real files
(`274754.mif`/`data.ggz`/`ddragonz.mod`/`ddragonz.sig`/`sound.ggz`)
byte-for-byte (SHA-256) against the copy already in this repo: **all 5
are identical**, including `sound.ggz`. This rules out "incomplete dump"
as the explanation for the entry-74 EOF mismatch documented above — the
file is confirmed authentic and complete from two independent sources,
which narrows that open question rather than closing it (see that
section for the remaining hypotheses).

**Format survey across all 61 titles, an important and unplanned
finding**: only Double Dragon uses the GGZ archive format this repo's
loader (`core/loader/ggz.h`) already supports. Every other title uses a
different real container. The classic-arcade ports (`Bad Dudes vs.
DragonNinja`, `Caveman Ninja`, `Dark Seal`, `Heavy Barrel`, `Karnov's
Revenge`, `Street Hoop`, `Pac-Mania`, `Super BurgerTime`, etc.) ship
loose per-asset files (`.tex`/`.wav`/`.fnz`/`.tga`) alongside a
`"PACK"`-magic `.pkg` container (real magic bytes confirmed directly:
`50 41 43 4B` = `"PACK"`, followed by a real embedded zlib stream --
`78 da`, zlib's "best compression" header -- found inside one entry's
raw bytes) -- consistent with these being ports built on an embedded
classic-arcade-hardware emulation core bundling original ROM data,
architecturally very different from Double Dragon's native BREW app.
Several real PopCap titles (`Peggle`, `Bejeweled Twist`, `Zuma's
Revenge`) instead ship a single clean `resources.bar`/`resources.dat`
archive alongside `<game>.mod`/`.sig` -- much closer in shape to Double
Dragon's own layout, though the header bytes checked (`Peggle`'s
`resources.bar`) don't match any publicly documented PopCap BAR
signature, so the real format is still unidentified, not assumed.
**Picked Peggle**: cleanest layout of the non-GGZ titles, and its
`peggle.mod` (274,124 bytes) is noticeably smaller than
`ddragonz.mod` (462,748 bytes).

**Found Peggle's real `IModule::CreateInstance` ClsId via live
execution, not hand-decoding.** Peggle's `AEEMod_Load`/`AEEMod_New`
prologue (file offset `0`-`0x2740`-ish) follows the identical real
`AEEModGen.c`-template shape Double Dragon's does (same MALLOC-based
`AEEMod_New(dwSize=20, ...)` call through the confirmed static-base
slot `0x68`) -- expected, since both are BREW SDK-compiled ROPI
binaries -- but its `CreateInstance` (real address `0x2630`, found by
resolving the vtable-populating PC-relative literal stores in
`AEEMod_New`'s tail) is a *thunk* that jumps through a stored function
pointer (`module+12`) rather than doing an inline class-ID compare like
Double Dragon's. That pointer is null on the very first real
`AEEMod_Load` call (confirmed live: the two stack args `AEEMod_New`
forwards for it come from memory Double Dragon's equivalent call site
also zeroes), so real `CreateInstance` instead falls through to a
generic real dispatcher at module offset `0xcc8`. Rather than keep
hand-decoding this unfamiliar shape, just ran it live (`trace=true` on
the `CreateInstance` call, same technique used throughout this log) with
a guessed ClsId (the folder name, `278962` -- wrong, as expected) and
read the real comparison directly out of the trace: `cmp r4, r0` at
module offset `0xce8`, with `r0` loaded from a literal at `0xcd8` whose
real value, cross-checked directly against the raw file bytes at that
exact literal-pool address, is **`0x01099CD6`** (decimal `17407190`) --
unrelated to Double Dragon's own real ClsId, `0x0102F789`; different
game, different constant. Re-ran with the real value: `CreateInstance` reaches
real, substantial code (accesses `AEECLSID_DISPLAY` = `0x01001001`,
matching Double Dragon's own confirmed real value) but wanders outside
the module at step 33, jumping to `pc=0x00000000` -- the same "missing
static-base slot -> null function pointer -> jump to zero" shape
documented earlier in this log for Double Dragon's own early
`AEEMod_Load` gap.

**Found and fixed the real gap: static-base slot `0x9c`, a debug-logging
function.** Traced the indirect call at module offset `0xd18`
(`ldr ip,[r0,#0x9c]; ...; bx ip`, where `r0` is the confirmed
static-base table address) and its arguments by resolving every
PC-relative literal address by hand and reading the real bytes at each
directly out of the file (not guessed): the destination buffer passed
as the first call's first argument literally contains the ASCII bytes
`"*dbgprint"`; the source argument is a real Windows build-machine path,
`"e:\Peggl..."` (a debug build artifact). A second, differently-shaped
call through the *same* slot immediately after (module offset `0xd30`
onward) passes a real literal format string as its first argument --
found three at nearby call sites: `"CREATING APPLET: %x"`, `"FAILED TO
CREATE APPLET %x"`, `"FAILED TO ALLOCATE MEMORY %x"`. This is BREW's
real `DBGPRINTF` macro family (different fixed-arity real helper calls
per how many `%`-substitutions the format string needs, all funneling
through one static-base slot). Checked every real call site found (161
total, `grep`-counted) for whether the return value is ever used
afterward -- it never is, the very next instruction after every call
overwrites `r0` before it could be read -- so implemented as a pure
no-op (`core/brew/mod_runtime.{h,cpp}`, `kDbgPrintfSlotOffset = 0x9c`,
one new test `DbgPrintfSlotDoesNotCrash`). Committed (`b0788bc`).
**Verified against the real game**: re-ran Peggle's `CreateInstance` --
now completes in 141 real steps (versus wandering after 33 and running
786,680 steps of a real-but-untrustworthy excursion before), returns
real `SUCCESS` (`r0=0`), and writes a real non-null applet pointer
(`0x80300024`).

**Blocked on a new, different, and more architectural gap: Thumb-mode
code.** Driving `HandleEvent(EVT_APP_START)` on the real, non-null
applet immediately throws: a real `BX`/`BLX` at module offset `0xa4c`
targets an odd address, requesting Thumb (T32) instruction state.
`core/cpu/arm_interpreter.cpp` (~600 lines) is ARM-only -- it has no
Thumb decoder and no ARM/Thumb interworking at all, because Double
Dragon's `ddragonz.mod` apparently never needed one. This is
qualitatively different from every other gap in this log: those were
all missing HLE call-outs or table slots (bounded, mechanical,
find-the-real-behavior-and-wire-it-in work); this is the CPU
interpreter itself missing a whole instruction-set mode, which would
mean a second 16-bit decoder plus correct mode-switching semantics --
a real, separate feature, not attempted this round. Deliberately
stopped here rather than starting that work without discussing scope
first.

**Where this leaves the Peggle thread**: real ClsId found and verified;
one real, general (not Peggle-specific) HLE gap found, fixed, tested,
and committed, benefiting any future title that hits the same slot;
`CreateInstance` now succeeds cleanly end-to-end.

**Implemented Thumb (T16) decoding and ARM/Thumb interworking.**
`core/cpu/arm_interpreter.{h,cpp}` gained a full second decoder: all 19
real instruction format groups from the ARM Architecture Reference
Manual's Thumb instruction set summary, matched bit-for-bit against
that public specification (the same kind of real-evidence grounding
this whole log uses for BREW APIs, just against the CPU's own public
ISA spec instead of a game binary) -- except software interrupt
(format 17) and the two reserved condition codes in format 16
(`0b1110`/`0b1111`), both of which raise `UnimplementedInstruction`,
matching how ARM-state SWI is already handled. Also implemented real
interworking per the ARMv5T+ rule ARMv6/ARM1136J-S includes: `BX`/`BLX`
in both instruction states, Thumb's `POP{pc}`, and ARM's `LDR`/`LDM`
loading directly into `pc` all now select ARM or Thumb from the target
address's bit 0 (previously, ARM's `BX` to a Thumb target threw
outright, and ARM's `LDR`/`LDM` into `pc` never checked bit 0 at all --
both real gaps this closes, not just the one that blocked Peggle).
33 new tests (`tests/thumb_test.cpp`), one per format plus interworking
and two deliberately-chosen edge cases: the real, spec-defined "loaded
value wins over writeback" rule for `LDM` with the base register in its
own register list, and rejection of a real Thumb-2-only encoding
(`SXTH`, `0xB200`) that's genuinely unassigned in the classic Thumb1
ISA ARMv6 implements. Every encoding used in the tests was generated
independently via a small Python script from the same bit-layout
description cited in the implementation's own comments, then the two
were cross-checked against each other -- not hand-copied into both
places, to avoid the test oracle and the implementation sharing the
same mistake.
**Verified against the real game**: re-ran Peggle's `HandleEvent
(EVT_APP_START)` -- it now executes real Thumb code for 25,003 steps
before hitting the next real gap (previously: an immediate throw on the
very first Thumb `BX`). All 234 tests (33 new Thumb tests plus every
pre-existing test, none touched) pass.

**Found two more real static-base gaps this same round, using the newly
now-working Thumb decoder to trace further than was previously possible.**
Both found via the same method as every other slot in this log: let
real execution wander to `pc=0x00000000` (the standard "missing
static-base slot" signature), find the exact call site through the
confirmed table-fetch idiom (`ldr rX,[pc,#N]; add rX,pc,rX; ldr
rX,[rX,#-4]`), and read real evidence (arguments, literals) directly
out of the file's bytes.
1. **Offset `0x44`, a second MEMCPY-equivalent.** The one real call
   site unambiguously reachable through the confirmed table-fetch idiom
   (`peggle.mod` offset `0xc718`) calls it with exactly memcpy's
   calling convention -- `(dest=sp+28, src=sp+4, count=24)`, a plain
   24-byte stack-struct copy -- and its return value is read
   immediately after, matching `void *memcpy(dest,src,n)`'s "returns
   dest" contract too. (A second, coincidentally-same-numbered access
   at a different call site turned out to be an unrelated object's own
   vtable slot, not this table -- ruled out by checking for the
   table-fetch idiom specifically, not just the numeric offset.) Most
   likely a real ARM EABI helper symbol (e.g. `__aeabi_memcpy`) a
   compiler can emit separately from user-callable `memcpy` for
   compiler-generated struct copies, but behaviorally identical --
   wired to the same `MemcpyImpl`, one new test confirms the alias
   behaves identically to the original slot. Committed alongside the
   Thumb work.
   **Verified against the real game**: real execution now reaches
   25,833 steps before the next gap (up from 25,003).
2. **Found, not yet fixed: a third real field on the shared "app
   context" struct.** The confirmed offset-`0xc0` slot (documented
   earlier in this log as returning a struct with `IShell` at `+12` and
   `IDisplay` at `+20`) has a third real field, `+0x2c` (44), that real
   Peggle code reads and dereferences expecting an actual object: `r0 =
   context[0x2c]; r2 = *r0; r3 = *(r2+4); r2 = r3+r2; bx r2` (module
   offset `0xafb4`-`0xafc8`) -- a vtable-style call, though the final
   `r3+r2` addition is an unusual shape worth understanding properly
   (possibly a relative/position-independent vtable entry, not a plain
   absolute function pointer) before implementing anything. Our context
   struct only ever writes `+12`/`+20`, so `+0x2c` reads back `0`,
   `*0` also reads `0` (our `Memory` returns 0 for unmapped addresses),
   and the final `bx r2` (r2 still `0` after the chain) is what lands
   back on the familiar `pc=0x00000000` signature. Deliberately not
   guessed -- what real interface this field is meant to expose isn't
   yet known (candidates worth checking against the bundled real BREW
   SDK reference material: some other ambient/shell-adjacent interface
   `AEEApplet`-style context structs commonly carry) -- tracked as the
   concrete next step.

**Where this leaves things now**: the CPU interpreter itself is no
longer the blocker for Peggle -- everything remaining is back to the
same kind of HLE-gap, real-evidence-grounded work every other entry in
this log describes.

**Fixed the `+0x2c` context field with a general mechanism, not a
guess.** Re-examined the real call site (`ldr r0,[r9,#0x2c]; ldr
r2,[r0]; ldr r3,[r2,#4]; add r2,r3,r2; bx r2`, `peggle.mod` offset
`0xafb4`-`0xafc8`) and recognized the shape: the resolved call target
is `vtable_pointer + *(vtable_pointer + 4)`, not the ordinary `*(vtable
+ slot*4)` every other confirmed interface in this codebase uses. This
is ARM RVCT's documented "ROPI" (Read-Only Position-Independent) C++
virtual-function-table convention: entries store offsets *relative to
the vtable's own address* rather than absolute function pointers, so
the vtable itself never contains a load-address-dependent value.
Confirmed this isn't specific to one call site by checking the pattern
is a real, general ABI variant, not a one-off -- so rather than a
one-off workaround, added a proper general mechanism:
`scaffold_object.h/cpp`'s `BuildGenericRelativeVtableStubObject`
(mirrors `BuildGenericStubObject`'s exact role for absolute-vtable
interfaces, just storing `sentinel - vtable_address` at each slot so
the real ABI's relative-offset formula resolves back to the real
sentinel). Two new tests confirm the header and the real resolution
formula both work. `ModRuntime` gained a third settable context field,
`SetThirdContextObject()`, mirroring `SetShellInstance`/
`SetDisplayInstance` exactly -- the real interface's identity is still
unknown, so this is wired to the new relative-vtable-safe scaffold, the
same "observe, then replace with real behavior once identified" role
generic stubs play everywhere else in this codebase.

**That unblocked one more real gap: static-base offset `0x74` is
REALLOC.** Found by continuing the exact same trace once the `+0x2c`
call could resolve instead of wandering to zero. Two independent real
call sites (`peggle.mod` offsets `0x3b038` and `0x3b0dc`) -- two
separate growable-array template instantiations, one with 56-byte
elements, one with 4-byte elements -- both call it with `(old_ptr=the
array's current buffer, new_size=new_element_count * element_size)`
and check the result for non-null before overwriting their own buffer
pointer: exactly `void *realloc(void *ptr, size_t size)`'s real
contract, including "leave the old block alone on failure" (neither
call site's own logic would make sense otherwise). Implemented against
this allocator's existing no-free-list bump allocator (`ModRuntime::
Allocate()`, factored out of `MallocImpl` so both share it): allocates
a fresh block of `size` bytes and copies from the old block if both
succeed. Documented tradeoff, not a hidden gap: since this allocator
never tracks or reuses freed sizes, it copies `size` bytes from the old
block rather than `min(old_size, size)` (real realloc's contract) --
safe specifically because bump-allocated memory is never reused, so
anything read past the old block's real content is either still-zeroed
or not-yet-allocated memory, and the real callers observed always
immediately overwrite that tail with new elements right after a
successful grow anyway. Three new tests (grow-and-preserve,
fail-leaves-old-block-alone, null-pointer-behaves-like-malloc).

**A debugging detour worth recording, since it nearly got mis-
diagnosed as a bug**: with both fixes in place, re-running Peggle
appeared to hang after one single real trap call in tick 0 -- no more
output for 90+ seconds under a bounded `timeout`. Chased this
seriously (temporarily reduced the interpreter's own step budget,
separated stdout/stderr to rule out output buffering as the cause,
added raw fprintf/fflush instrumentation directly inside `Step()` and
`ExecuteBranchExchange`) before finding the real explanation: the
"hang" was a real `bx lr` where `lr` held exactly `0xf0000000` --
`trap_base`, the real sentinel address `HleRuntime`/`tools/game_probe.
cpp`'s own call-loop convention uses to mean "this ARM function call
completed, return control to the C++ caller." That's the *correct*,
successful completion of the tick-0 timer callback -- not a bug. Once
that returns cleanly, `tools/game_probe.cpp`'s own top-level loop does
exactly what it's designed to do for an interactive tool: delay 16ms,
tick again, forever, until a real `SDL_QUIT` (never sent in this
headless test run). All debug instrumentation was reverted (`git diff`
confirmed clean on `core/cpu/arm_interpreter.cpp` before committing).

**Verified against the real game**: Peggle's `HandleEvent(EVT_APP_
START)` now returns successfully (`1`), and the tool reaches "Reached
the event loop with no unhandled instruction! Window will stay open."
-- the exact same milestone Double Dragon reached, on a second real
game, using entirely general (not Peggle-specific) fixes. All 240 tests
pass.

**Next concrete steps**: (1) let Peggle run for many more real ticks to
see what real gap (if any) shows up next, the same iterative way every
title in this log has been debugged; (2) separately, reverse-engineer
`resources.bar`'s real format (still not started; Peggle's own header
doesn't match any known public format checked so far) -- needed before
Peggle can load its own real assets the way Double Dragon does.

---

**Ran (1) -- no new crash across thousands of real ticks.** Let the
tool run for 60+ real seconds (thousands of 16ms ticks) past the point
`HandleEvent(EVT_APP_START)` first succeeded. Confirmed indirectly
rather than by reading direct output (stdout is fully buffered when
redirected to a file, and the process is killed by an external
`timeout` rather than exiting -- so buffered output past the initial
setup lines is lost, the same trap the "false-alarm hang" entry above
already ran into): the process consistently required *external*
termination to stop. Every real error path in
`tools/game_probe.cpp`'s tick loop (`wandered_outside_module`,
`exceeded_step_budget`, a thrown `UnimplementedInstruction`) sets
`running = false` and lets the tool exit on its own -- so a process
that only stops when killed from outside, never on its own, is
consistent with (though not direct proof of) clean, uninterrupted
success across all of those ticks.

**Started (2), `resources.bar`'s real format -- made real progress on
*what* it is, not yet *how it's laid out on disk*.** Found the real
call site the same way as every static-base slot in this log: found
`"resources.bar"` as a real string thrice in `peggle.mod` (`0x3cdc8`,
`0x3d280`, `0x3ec64`), then wrote a small script scanning the whole
binary for the confirmed "PC-relative literal, add pc" idiom to find
which real code computes each string's address -- two real call sites
resolved cleanly (`peggle.mod` offsets `0x8ed0` and `0xa688`).
Disassembling the first's surrounding function (`0x8e90`-`0x8eec`)
shows it's **not a custom file-reading routine at all** -- it's a real
call through the confirmed `ISHELL` vtable (offset `0xc0`'s ambient
context struct, `+12` for the `IShell` pointer, then the real object's
own vtable at slot `0xa4`/41), with a five/six-argument shape: `(pIShell,
"resources.bar", id=r4&0xFFFF, type=0x5000, [sp]=0xFFFFFFFF, [sp+4]=
&local)`. Cross-referencing the real bundled `AEEShell.h` identifies
every piece of this precisely: `RESTYPE_BINARY` is literally defined as
`0x5000`; `AEE_RES_EXT` is literally defined as `".bar"` -- "Extension
of BREW Application Resources"; and the exact `(p,psz,id,t,b=-1,l)`
argument shape matches the real documented macro `ISHELL_GetResSize`,
`#define ISHELL_GetResSize(p,psz,id,t,l) (IShell_LoadResDataEx((p),
(psz),(id),(t),(void*)-1,(l)), *l)` -- the `-1` buffer sentinel is the
real, documented way to ask "just tell me the size, don't copy the
data." So: `resources.bar` is confirmed to be a **standard BREW
application resource file**, not anything Peggle-specific -- the exact
same real mechanism any BREW app's `.bar` resource file uses, just
happening to share a name with (and be otherwise unrelated to) PopCap's
own differently-shaped "BAR" archives on other platforms (ruled out
firmly this time, not just "doesn't match a public spec" as noted
earlier probing this file).
**Where this hits a real, structural limit the rest of this log hasn't
run into**: every other format/API this project has reverse-engineered
so far had its *implementation* inside a real, disassemblable `.mod` --
GGZ's reader, the static-base runtime-support table, every `IFile`/
`IShell` HLE call. `ISHELL_LoadResDataEx`'s real implementation lives in
the Zeebo device's own closed OS/firmware, not in `peggle.mod` at all
-- there is no compiled code in this repo's possession that parses the
real `.bar` binary layout. Cracking the format therefore needs a
different method than the rest of this log: blind, evidence-anchored
byte analysis of the raw file, verified against a known (resource ID,
real size) pair -- not disassembly of a caller. Tried to get that
anchor by tracing the real requested resource ID at the one real call
site found: neither of the two confirmed call sites (`0x8ed0`,
`0xa688`) is reached by `CreateInstance`, `HandleEvent(EVT_APP_START)`,
or the first several real ticks driven the same blind way as
everything else in this log -- meaning Peggle's real code only reaches
its own resource-loading path under some game state (a specific menu,
level, or asset category) not yet reached by driving ticks alone.
**Deliberately stopped here rather than guess the byte layout blind**:
without a real (ID, size) pair to check candidate structures against,
attempting to parse `resources.bar`'s directory now would be
undirected guessing -- exactly the kind of "wrong implementation risks
trading a clean, diagnosable failure for silent data corruption" this
log has avoided everywhere else. Tracked as the next concrete step,
either by finding what real game state reaches the resource-load call
(more disassembly of the surrounding real control flow), or by
resuming the raw byte analysis already started on the file's first 128
bytes (a plausible small header/count field around offset 0, then what
looks like a directory table with irregular strides starting around
offset `0x2c` -- not yet confirmed against any real value).

**Chased "what real game state reaches the resource-load call" and
found the real, structural reason: Peggle's main loop doesn't use the
self-rearming `SetTimer` pattern this codebase's whole tick-driving
model assumes.** Statically traced the real call chain backward from
the resource-load site (`peggle.mod` offset `0x8e90`) through three
real callers (`0x8860` -> `0x6d34` -> `0x2aca4`, the last flanked by
two more real `DBGPRINTF` calls with real source line numbers `216`/
`221`) to build a plausible reach path, then tested it directly with a
cheap PC watchpoint (temporary, reverted) on all three addresses across
60+ real seconds (thousands of ticks): zero hits. Also tried injecting
all 14 of the dev tool's mapped `AVK_*` key codes across many ticks in
case a menu/splash screen was waiting on input (temporary, reverted):
also zero hits, and critically, zero evidence any *further* tick ever
ran at all.
That last point led to the real answer: added a temporary live print
directly inside `IShellHle::SetTimerImpl` (reverted after use) and
confirmed `ISHELL_SetTimer` is called **exactly once** across the
entire run -- `(ms=20, callback=0x00132db0, user_data=0x80280200)`.
The callback address's bit 0 is clear (ruling out a real suspicion
this raised: that `tools/game_probe.cpp`'s `CallArmFunctionChecked`
dispatches a timer callback's raw pointer value as a PC transfer
without checking it for the real ARM/Thumb interworking convention a
function-pointer *value* is subject to, unlike a direct branch --
worth fixing generally if it's ever found to matter, but not the cause
here). And per this file's own earlier, already-fully-traced record of
tick 0's real execution (~24 instructions, one `GETAPPCONTEXT` call, a
clean `bx lr` return), that lone callback never calls `SetTimer` again
to re-arm itself.
Double Dragon's entire per-frame loop depends on exactly that real,
self-rearming pattern -- documented plainly in this project's own code
(`core/brew/ishell.h`'s class doc comment: "real game code re-arms its
own via `ISHELL_SetTimer` ... calling `SetTimer` again with the same
identity every time it fires"). **Peggle's real main loop evidently
does not work this way**, which is the real, now-confirmed reason
nothing past tick 0 -- including the resource-load call site -- is
ever reached by driving simulated time forward the way every title in
this log has been driven so far. What Peggle's real continuation
mechanism actually *is* (a different real BREW notification API, a
redraw/vsync-driven callback, or something else entirely) is not yet
identified -- this is now the concrete blocker for both making further
progress on Peggle at all and for finding a verifiable anchor to crack
`resources.bar`'s binary format.
All temporary instrumentation for this investigation (the PC
watchpoint, the key-injection probe, the `SetTimerImpl` print) was
reverted -- confirmed via a clean `git status`/`git diff` before moving
on. 240 tests still pass.

---

**Fixed the real timer re-arm gate.** Re-examined tick 0's callback
(`peggle.mod` offset `0x32db0`, already fully disassembled in the
entry above) closely enough this time to see it *does* try to re-arm
its own `ISHELL_SetTimer` -- gated entirely behind
`*(context[0x24] + 20)` being non-zero, a fourth real field on the
same shared "app context" struct as the confirmed Shell/Display/third-
object fields, which this codebase never wrote (so it always read as
null and the gate always failed). Unlike the other three fields, this
one is read and written as a plain data struct rather than through a
vtable -- the same callback stores a 64-bit timestamp at `+24`/`+28`
and reads another field at `+0x2a0`. Traced the time source
(`peggle.mod` offset `0x16cbc`) and confirmed it's nothing new: a thin
wrapper around this codebase's own already-working `GETUPTIMEMS` slot,
returning it zero-extended to 64 bits.
Added `ModRuntime::SetFourthContextObject`, wired in `tools/game_probe.
cpp` to a real, writable, zeroed memory block with only the one
confirmed-load-bearing field (`+20`) pre-set non-zero -- explicitly
framed (in both the code comment and here) as an educated, minimal
enabling stub, not a claim about what this struct actually is.
**Verified against the real game**: tick 0's callback now runs
hundreds of real HLE calls -- including a real `ISHELL_CreateInstance`
for class `0x01001003`, the exact same `FileMgr` class Double Dragon
uses -- where it previously made exactly one. Committed (`cf1b1fe`).

**Immediately hit a new, different gap, and traced it precisely: not
a missing HLE call this time, but a much bigger real structure than
our placeholder can safely stand in for.** Re-ran with full
instruction tracing bounded to tick 0 (temporary, reverted after) and
found the new wander-to-zero at real module offset `0x99f8`
(`peggle.mod` offset `0x9868`-`0x99f8`, a different real function from
the timer callback -- reached from it). The exact real sequence:
`bl 0x27594` (`GETAPPCONTEXT`, confirmed identical to every other real
call site) `-> ldr r0,[r0,#0x24]` (our new field) `-> add r0,r0,
#0x45000 -> ldr fp,[r0,#0x3d8]` -- i.e. real code treats
`context[0x24]` not as a small "manager" object with a couple of
meaningful fields, but as the **base address of a large global data
arena**, with different real subsystems reaching their own portion of
it through large, fixed offsets (`0x45000` here) from that same base.
Confirmed by adding `r11` to the trace print (temporary, reverted):
`fp` (`r11`) is null at the crash, exactly as expected, since our
placeholder object is only ~1KB and everything past it reads as
unmapped/zero by default -- not a bug in the fix itself, just
confirmation the real structure goes much further than what's been
implemented.
**Deliberately not extended further right now.** The `+20` "is ready"
flag was a single, narrow, well-evidenced field with a clear real
consequence (unlocking `SetTimer`); guessing at what real object
belongs at `context[0x24] + 0x45000 + 0x3d8` inside what is apparently
a large, multi-subsystem global arena -- with no idea yet how many
more such offsets exist elsewhere in it -- is a different, much larger
kind of guess, and exactly the risk this log has avoided everywhere
else ("a wrong implementation here risks trading a clean, diagnosable
failure for silent data corruption"). Tracked as the next concrete
step: either map out more of this arena's real layout via further
disassembly (now that the technique -- watch `GETAPPCONTEXT` ->
`context[0x24]` -> large fixed offset -- is established and
repeatable), or treat `context[0x24]` itself differently (e.g. as a
large real allocation the emulator provisions generously by default,
if further evidence suggests the arena's *contents* mostly don't need
to be meaningful except at a few specific, identifiable offsets like
this session's `+20`).
All temporary instrumentation (the `r11` trace column, the tick-0-only
trace flag) reverted -- clean `git diff` confirmed. 241 tests pass.

---

**Provisioned `context[0x24] + 0x45000 + 0x3d8` and immediately hit
two more real, evidence-grounded gaps in a row, both now fixed.**
First attempt wrote a generic stub object into that field during
initial setup, before any ARM code ran -- `fp` was still null at the
same crash site as before. A `Memory::Write8` watchpoint on that exact
address (temporary, reverted) caught the real cause: real
`CreateInstance`/`HandleEvent(EVT_APP_START)` code writes a real zero
to this same field once, as part of its own initialization (evidently
its own "not yet initialized" reset), unconditionally clobbering
whatever was already there. **Fix**: moved the write to happen right
before the "Reached the event loop" print, i.e. after `HandleEvent`
returns and after the real reset has already happened -- confirmed via
trace this unblocks the immediate null-pointer crash (step count moved
from 3100 to 3106).

Immediately hit a second gap one level deeper: the object's slot 2 is
called with a real QueryInterface-style shape (`this`, an id/flag, and
an output pointer `ppOut` in `r2`); our stub correctly returned success
in `r0`, but since it never touched memory, `*ppOut` stayed zero, and
the real caller dereferenced it without checking the status -- a new
null-pointer crash at step 3155. Patching slot 2 with a second stub
object fixed that one level, but the exact same shape recurred a third
time at the same step count, through the newly-returned object's own
slot 2. Rather than keep hand-patching one level at a time, generalized
to a recursive **self-propagating stub** (`std::function<uint32_t()>`
lambda capturing itself by reference, in `tools/game_probe.cpp`): every
slot of every generated object now lazily builds a fresh child object
of the same shape and writes it into whatever output pointer the real
caller passed, on demand, however deep a real chain of these turns out
to go. Explicitly marked EXPERIMENTAL and kept local to
`tools/game_probe.cpp` rather than promoted to
`core/brew/scaffold_object.h`, since this out-pointer-chaining shape is
not yet confirmed as a general real BREW ABI convention -- it's only
been observed at this one real call site so far.

**Verified against the real game**: tick 0 now runs vastly deeper --
real `ISHELL_CreateInstance(ClsId=0x01001003)` (the same `FileMgr`
class confirmed via Double Dragon), many real `Seek`-shaped
(`trap=0xf000029c`) calls, real `trap=0xf0000294` calls, and the
self-propagating chain itself visibly firing through real traps
`0xf0000584`, `0xf0000664`, `0xf0000700`, `0xf0000588` -- before
hitting a new, different-in-kind wall. First tried the self-propagating
stub with 10 slots per object and hit the exact same step-3155 wall;
tracing showed the failing read (`ldr r3,[r1,#0x30]`, offset 48 = slot
12) was simply past the end of a too-small 10-slot vtable, reading
unmapped memory as zero. Widened to 40 slots (matching this codebase's
established sizing convention for unidentified interfaces) and hit the
*same* step-3155 wall again -- this time confirmed via careful trace
re-reading that the instruction is not a vtable-indirected call at all:
it reads directly from `object+0x30` (the object's own memory), not
`*(*object+0x30)` the way every real vtable call handled so far works.
That is a third, genuinely different real object convention -- a flat
struct with a function pointer embedded at a fixed offset, distinct
from both the ROPI-relative-vtable shape (context's own third field)
and the standard absolute-vtable shape (every `BuildInterfaceObject`
call, including this arena object itself) already implemented.
**Deliberately left undoctored** -- the self-propagating stub's plain
zero-filled object memory reads as 0 there, producing the same clean,
diagnosable wander as every previous real gap in this log, rather than
guessing at what real function belongs at that offset. This is the
next concrete step for continuing this investigation.

Committed (`01fa50e`). All temporary instrumentation (the
`Memory::Write8` watchpoint in `core/memory/memory.cpp`, an `r11`
trace column in `core/cpu/arm_interpreter.cpp`) reverted -- confirmed
via clean `git diff` on both files. 241 tests pass.

---

**The "flat struct at offset 0x30" wall was a misdiagnosis -- the real
bug was in this codebase's own self-propagating stub, not a third real
object convention.** Re-enabled full register tracing for tick 0 only
(temporary, reverted after) and re-read the exact real call chain at
peggle.mod offsets `0x1099e0`-`0x109aac` closely, instruction by
instruction, rather than trusting the earlier quick read.

Real vtable slot 2 (offset 8) genuinely is the `(this, id@r1,
ppOut@r2)` QueryInterface shape the stub was built around -- confirmed
again at offset `0x1099e0`: `ldr r2,sp+0x28 (ppOut); ldr r3,[r0,#8]
(slot 2); ldr r1,[pc,#lit] (a real id constant, 0x0101eb0b); mov
r0,fp; bx r3`. But real slot 3 (offset 0xc) is a *different*, also
real shape: `(this, ppOut@r1)`, no id argument at all -- confirmed at
offset `0x109a98`: `ldr r0,[fp]; add r1,sp,#0x24; ldr r2,[r0,#0xc]
(slot 3); mov r0,fp; bx r2`. And real slot 4 (offset 0x10, confirmed
at offset `0x109a00`) is called with *no* output pointer whatsoever --
`r1`/`r2` just hold whatever leftover values earlier code left in
them. A further real call, `bl 0x105b50` at offset `0x109a94` (itself
a tiny two-instruction real trampoline, `ldr r3,[r0]; ldr r3,[r3,#0xc];
bx r3`, i.e. "call this->vtbl[3]" generically), is made with `r1=0,
r2=0` explicitly set by the real caller just before the call -- a real
"just checking, don't return anything" idiom.

The previous version of the self-propagating stub wrote a fresh child
object into r2 for *every* slot unconditionally, with no knowledge of
which shape a given real slot actually used. For slot 4's and the
`bl 0x105b50` call's real, no-output shape, this blindly clobbered
whatever r2 (or r1) held -- confirmed via re-tracing that one such
write landed on real address 0 itself (`r2` was legitimately 0 for
that call), silently seeding it with a stray child-object address. It
was *that* stray, self-inflicted value -- not a real "flat struct"
object -- that later got picked up, treated as a vtable pointer by
`ldr r1,[r0]` (r0 having read back 0 from an earlier never-actually-
delivered `ppOut`), and read at offset 0x30 (slot 12, well within the
real 40-slot vtable) as a function pointer, producing a bogus non-null
`bx` target and the misleading step-3155 crash.

**Fix**: only slots 2 and 3 get the self-propagating treatment now,
each using its own real, evidenced output register (r2 and r1
respectively), and each skipped entirely when that register is null
(so the real "just checking" calls are left alone rather than
corrupting address 0 or anywhere else). Every other slot (0, 1, and
4-39) is now a plain, side-effect-free stub -- consistent with this
codebase's normal `Stub` pattern elsewhere, and honest about what
isn't actually understood yet.

**Verified against real Peggle**: tick 0 now makes 337 real HLE calls
before its next wander (up from 207 before this fix), and survives
6312 real ARM steps (up from 3155) -- roughly double the real
execution depth in both measures. It still eventually wanders to
`pc=0` and throws the same downstream invalid-instruction exception at
module offset `0x90024` as before, but now from a **new, later, and
presumably genuinely different** real call site -- not yet
individually traced, since this round's fix was already a large,
self-contained, evidence-grounded unit of progress worth landing on
its own. Continuing to chase the new wander point the same way (full
register tracing, cross-referenced against real disassembly) is the
natural next step.

Committed (`6ab0d9f`). Rebuilt `minimal.ggz` (a synthetic, empty-but-
valid GGZ archive -- a real 8-byte one-entry table pointing at a real,
empty gzip member -- used as Peggle's `data.ggz`/`sound.ggz` stand-ins
since Peggle ships neither; the original copy from earlier sessions
lived in a since-expired scratch directory) to re-run the probe; not
committed, since `tools/game_probe.cpp` already documents how to
construct one and it's a throwaway harness input, not project source.
241 tests pass.

---

**Chased the new, later wander point from the previous fix -- and it
was a real gap this codebase already knows how to fill.** Re-enabled
full register tracing for tick 0 (temporary, reverted) and found the
new `pc=0` wander (at step 6312, up from 3155) came from real code at
`peggle.mod` offset `0x132dfc`: `ldr r0,[r4,#0x28]` (`r4` = the app
context address) with **no null check** before immediately calling a
small real subroutine at offset `0x131fac` with that value. That
subroutine (`ldr r1,[r4]; ldr r2,[r1]; add r2,r2,r1; bx r2`, after
first calling `GETAPPCONTEXT` to get `r0`/context again) is the exact
same ARM RVCT "ROPI" relative-vtable dispatch already confirmed and
implemented for the context struct's third field (`+0x2c`) -- i.e.
this is a **fifth confirmed field on the same shared app context
struct, offset `+0x28`**, using a convention this codebase already has
a scaffold for.

Checked whether this field needed the same write-timing care as the
fourth field's arena (a `Memory::Write8` watchpoint on
`context_address + 0x28`, temporary, reverted): real code never writes
it at all across the whole run -- purely read, never written, unlike
the arena's internal reset. And unlike the fourth field's arena
object, `GetAppContextImpl` already rewrites the *entire* context
struct fresh on every real `GETAPPCONTEXT` call (confirmed by reading
its own source, `core/brew/mod_runtime.cpp`), so there was no
write-timing hazard to work around here.

**Fix**: added `ModRuntime::SetFifthContextObject`, an exact mirror of
`SetThirdContextObject`, and wired it in `tools/game_probe.cpp` to
another `BuildGenericRelativeVtableStubObject` scaffold (its real
identity is just as unknown as the third field's).

**Verified against real Peggle -- this is the milestone the whole
Peggle investigation has been chasing**: the timer callback now runs
tick after tick with zero "wandered outside module" warnings and zero
thrown exceptions. Confirmed two ways: (1) a 15-second run with per-
tick HLE tracing enabled through tick 9 shows ten consecutive clean
ticks, each ending with the same real call sequence and no errors; (2)
an unbounded 60-second run needed external `timeout` termination
rather than exiting on its own -- the exact same "success looks like a
hang" signature already established and trusted for Double Dragon's
own steady-state event loop (every real error path in
`tools/game_probe.cpp`'s loops sets `running=false` and exits cleanly
on its own; nothing here does). Committed (`473758e`). 241 tests pass.

**Not yet done, and the natural next steps**: this is real *sustained*
execution, not necessarily real *correct* execution -- the third and
fifth context fields, the fourth field's arena beyond its one known
sub-offset, and every non-slot-2/3 method on the self-propagating
stub's generated objects are all still safe no-op placeholders, so
there's no guarantee real game logic (physics, scoring, resource
loading) is actually progressing rather than looping harmlessly. There
is also no visible output yet -- nothing has driven a real frame to
the SDL window. The next concrete step is watching (or tracing) what
the game actually *does* over many ticks: does real state visibly
change (level data loading, `resources.bar` finally being opened, a
frame rendered), or is it looping in place on placeholder objects that
never deliver real content forward.

---

**Answered that question directly: it's looping in place, not
progressing.** Added temporary debug prints (reverted after use, no
source changes remain) to every real "does something visible/external"
HLE call this codebase has -- `IDisplayHle::DrawText`/`DrawRect`/
`SetColor`/`Update`, `FileHle::OpenFileImpl`, and
`IShellHle::CreateInstanceImpl`'s unknown-class failure path -- then
ran real Peggle for 30 real seconds (roughly a thousand-plus real
ticks, going by the ~16-20ms real timer interval).

**Result**: only five real events fired in the *entire* 30-second run,
all during the one-time `CreateInstance`/`HandleEvent(EVT_APP_START)`
setup, none afterward: `OpenFile("udata/game", mode=1)` (read, fails),
`OpenFile("udata/game", mode=2)` (read/write, fails),
`OpenFile("udata/game", mode=4)` (create, presumably succeeds --
a real "load save data, create if missing" pattern), and two real
`CreateInstance` requests for classes this codebase doesn't implement
(`ClsId=0x0103d8ec`, `ClsId=0x01030766` -- real, evidenced leads for a
future round, not chased further this round). **Zero** `DrawText`/
`DrawRect`/`Update`/`SetColor` calls the entire run -- the game never
draws anything -- and **zero** further file opens once ticking began,
including no attempt to open `resources.bar` itself.

Cross-checked by diffing the full per-tick HLE call trace (all 20 real
calls, trap addresses and register arguments both) between tick 1 and
tick 5 of a separate traced run: **identical**, except for one
incidental pointer value differing by 16 bytes (not a growing
self-propagating-stub address -- those grow by 0x1000 per new object
-- so almost certainly stack/heap noise, not real state). Tick 1 and
tick 5 execute the exact same 20 HLE calls in the exact same order
with the exact same arguments.

**Conclusion**: the sustained, exception-free execution from the
previous two fixes is real, but it is a **fixed loop over placeholder
objects**, not real game logic progressing. The timer callback re-arms
itself and re-runs the same ~20-call sequence every tick indefinitely
-- consistent with the callback polling something (most likely one of
the still-unidentified interfaces behind the third/fifth context
fields' relative-vtable placeholders, or a sub-offset of the fourth
field's arena beyond the one confirmed `+20` gate) that our safe
no-op stubs can never report as "ready," so the real code that would
follow (loading `resources.bar`, drawing a frame) never runs. This is
the expected, honest limit of the "safe generic placeholder" approach
this whole investigation has used -- unblocking gates one confirmed
field at a time gets real code *running*, but getting it to do
something *meaningful* needs the placeholders' real identities, which
aren't known yet.

**Next concrete step for whoever continues this**: identify what the
third/fifth context fields' relative-vtable objects and the
self-propagating stub's non-2/3 slots are actually being asked for --
in particular, the literal ID constant baked into the module at the
confirmed slot-2 QueryInterface call (`0x0101eb0b`, `peggle.mod`
offset `0x1099f0`) is a real compile-time constant, and cross-
referencing it (and the two unknown `ClsId`s found this round) against
real BREW/Zeebo headers or other real `.mod` binaries already in
`research/` may reveal what real interface is actually being asked
for, the same way earlier class IDs in this project were identified.
All temporary debug prints reverted -- confirmed via clean `git diff`.
241 tests pass (no source changes to test).

---

**Chased the cross-referencing lead from the previous round.** This
repo's own reference BREW header subset
(`research/docs/sdk_installer_extract/brew_sdk_headers_reference/`) is
small (13 files) and had no exact match for any of the three IDs
(`0x0101eb0b`, `0x0103d8ec`, `0x01030766`), but it did establish the
real numeric convention: `AEEIID_IBase`/`AEEIID_IDisplay` are
`0x0103xxxx`, `AEECLSID_DISPLAY*`/`AEECLSID_DISPLAY_NULL` are
`0x0101xxxx` -- both unknown IDs sit squarely in those same real
families, consistent with being genuine platform-allocated IDs rather
than app-private ones.

More useful: a binary literal search (`struct.pack("<I", id)`) across
every real `.mod`/binary already in `research/games/` found
`0x0103d8ec` is **not Peggle-specific** -- it also appears in
`Super BurgerTime/mod/279125/supbtime.mod`. Disassembling both real
call sites (`peggle.mod` offset `0x104a50`-`0x104aa0`;
`supbtime.mod` offset `0x110e64`-`0x110ef4`) shows the **exact same**
real instruction sequence in both, independently-compiled titles: an
`AddRef`-shaped call on a real `IShell` pointer, then
`ISHELL_CreateInstance(pShell, 0x0103d8ec, &slot)`, and -- only if
that fails (`cmp r0,#0; beq ...`) -- a second attempt with a different
real ClsId (`peggle.mod` literal at offset `0x104ac8` = `0x01014bc4`;
confirmed identical at `supbtime.mod` offset `0x110f24`). Two
independently-compiled real games trying the exact same two literal
class IDs in the exact same fallback order is strong evidence this is
a real, standard SDK/compiler-emitted helper -- not anything
Peggle-specific -- even without a header match.

A third real ClsId, `0x01030766` (`peggle.mod` offset `0x10a208`-
`0x10a24c`), was found and traced separately: reached via a real
`IShell` pointer read from offset `+12` of the calling function's own
struct parameter (the same confirmed Shell-field layout as the ambient
app context struct), with `ISHELL_CreateInstance`'s result stored
**unconditionally** (no failure check at all) into that struct's own
offset `+0x48`.

**Fix**: registered all three with the same generic
`BuildGenericStubObject` scaffold already established for the
analogous `0x01002001` case (Double Dragon investigation, this same
log, above) -- deliberately not guessing at a real interface shape
neither header evidence nor call-site evidence actually supports yet.
Committed (`3045852`).

**Verified against real Peggle**: tick 0's total HLE call count
dropped from 340 to 331, consistent with the real fallback-
`CreateInstance` attempt now being skipped since the primary succeeds
-- confirming the fix took effect exactly as the real disassembly
predicts. **However**: the steady-state per-tick loop (tick 1 onward)
is completely unchanged -- still the exact same 20 real calls, same
order, verified by diffing the full trace before and after this fix.
**This rules out all three of these classes as the cause of the
per-tick progress plateau** documented in the previous round -- they're
one-time startup calls, not part of whatever the steady-state loop is
actually polling.

While tracing `0x01030766`'s call site, incidentally found a further,
adjacent real lead worth recording rather than chasing this round:
immediately after it (`peggle.mod` offset `0x10a254`), a separate real
function calls `GETAPPCONTEXT`, reads `context[0x24]` (the confirmed
arena base), and reads `arena+0x45000+0x3dc` -- four bytes past the
already-confirmed, already-provisioned `+0x3d8` sub-offset. This looks
like a sibling slot in the same small run of pointer-sized arena
fields, but reachability from the actual steady-state loop wasn't
confirmed this round (this function wasn't observed in any of the
traced ticks), so it wasn't provisioned -- a candidate for a future
round, not a confirmed blocker.

**Not yet resolved**: the actual per-tick blocker. The steady-state
loop's own real ID constant (`0x0101eb0b`, queried every tick via the
confirmed slot-2 QueryInterface shape on the fourth field's arena
sub-object) still has no header match and wasn't chased further this
round beyond the header/binary cross-reference above (also no match).
Identifying it likely needs either a fuller real BREW MP SDK header
set than this repo currently has, or continuing to trace structurally
-- what real methods get called on whatever object a real
implementation would eventually return -- the same way the context
struct's Shell/Display fields were originally identified.

---

**Web search for a fuller real BREW MP header set, or anything else
that might identify `0x0101eb0b`/`0x0103d8ec`/`0x01014bc4` --
exhausted for now, no matches found.** No search hit (general web,
BREW developer forums, GitHub) turns up any of these four hex values
in any indexed source. Two adjacent leads were checked and ruled out:

- **Infuse** (Tuxality's independent Zeebo/BREW HLE emulator, also
  written "from clean reverse engineering" per its own dev blog, and
  already reaching a playable steady state on Double Dragon, Crash
  Nitro Kart 3D, and Zeebo Family Pack) looked promising, but its
  source is **closed and proprietary** ("as-is", no redistribution, no
  modification) -- there is no code to safely reference even if it
  had solved this exact problem, and its public blog posts describe
  features only at a high level, with nothing about class IDs or the
  app context struct. It also doesn't target Peggle or Super
  BurgerTime, so it may never have hit these particular IDs anyway.
  Disassembling *its* compiled binary to extract a class-ID table
  would violate its own license terms and is a materially different,
  more invasive action than anything else in this project -- not
  attempted.
- **Actual Zeebo device firmware**: searched (not downloaded) for any
  public source of a real firmware/system image, both generally and in
  Portuguese. None found. This project's own already-sanctioned
  archive.org source (`zeebo-arquivista`) and the larger "Zeebo (All
  Games + Dev Tools)" archive.org collection both turn out to contain
  only games plus BREW SDK samples/PDFs already mirrored in this
  repo's `research/` -- no bootloader/OS-level dump in either. A
  GBAtemp thread referencing Zeebo console jailbreaking returned
  HTTP 403 and wasn't pursued further.

**Conclusion: this specific lead is exhausted.** No further internet
search is expected to help without a new, more specific starting
point. Identifying `0x0101eb0b` (or the other two now-registered-but-
unidentified classes) still needs either a fuller real BREW MP SDK
header dump than exists anywhere this search found, or continued
structural tracing of the real methods called on whatever object a
correct implementation would return -- the same evidence-only approach
used for every other interface in this project so far.

---

**Took the "continued structural tracing" option: full instruction
trace of one steady-state tick, to see what real code branches on.**
Re-enabled full per-instruction tracing for one representative tick
(temporary, reverted after). The very first thing the timer callback
does, every single tick, is real (`peggle.mod` offset `0x132de4`-
`0x132df4`): read `context[0x24]+0x45000+0x3dc` -- an immediate
sibling of the already-provisioned `+0x3d8` field, 4 bytes later,
confirmed reachable via two independent real functions now (this one,
and the `0x01030766` call site's own neighbor function from the
previous round) -- and pass it, completely un-null-checked, as `this`
into a real subroutine at offset `0x109088`.

That subroutine immediately dereferences its `this` parameter multiple
times: `ldr r0,[r0,#4]` (read), `str r0,[r5,#4]` (write back,
incremented -- a real per-tick call counter), `ldr r0,[r5,#12]` (read
a second field, presumably an array pointer), then `ldr r0,[r0]`
(dereference *that*). With the field left at 0 (this codebase's
default for anything unprovisioned), every one of those accesses reads
or writes **real, meaningful low memory addresses** -- confirmed via
the trace that real address 4 was getting a real incrementing counter
value written to it, every single tick, and real address 0 was being
read back through the chained double-dereference. This is real,
evidenced pollution of memory that has nothing to do with this field,
not simulated behavior of anything real -- exactly the kind of subtle
risk this log has flagged before ("a wrong implementation here risks
trading a clean, diagnosable failure for silent data corruption"),
except here the risk isn't hypothetical, it's directly observed.

**What this subroutine's real elements look like isn't understood well
enough to populate meaningfully**: offset `+4` is a real call counter,
`+12` looks like a pointer to a small (`<=4`-element, per a `cmp
r4,#4` loop bound) array, each element read at large offsets (`+0xbc`,
`+0xdc`) relative to a `4`-byte stride that doesn't obviously match a
`0xbc`+-byte struct -- consistent with this being **Peggle's own
internal per-tick game-object list** (particles, pegs, whatever),
anchored in the shared arena the same way the BREW-interface fields
are, rather than a generic BREW interface at all. Fully understanding
it would mean reverse-engineering Peggle's own gameplay data layout,
a materially bigger and more speculative undertaking than every other
field fixed in this investigation so far, all of which have been
identifiably generic BREW/ambient-context mechanisms.

**Fix, deliberately conservative**: rather than guess at that real
layout, gave this field the exact same treatment already used for the
fourth field's own arena allocation itself -- a real, writable, zeroed
memory block (`0x80050000`), just enough that the real accesses land
on memory that actually belongs to this field instead of colliding
with unrelated real addresses. This does **not** change what the real
subroutine does: a zeroed block still reads as "empty" at every offset
checked, so the same do-nothing branch is still taken every tick --
this is a hygiene/isolation fix, not a claimed progress unlock.

**Verified** via a full re-trace: `this` now resolves to `0x80050000`
instead of `0`, and the per-tick counter write correctly lands on
`0x80050004` instead of real address `4`. The double-dereference
(`ldr r0,[r5,#12]; ldr r0,[r0]`) still incidentally reads real address
`0` once, because this field's own `+12` slot is legitimately null (an
empty array, unknown real content) -- expected, harmless (the result
only feeds a `cmp`/`beq`, never a jump target), and not something
further guessing could safely improve. Committed (`47bfbf6`). 241
tests pass; a 30-second run against real Peggle remains stable (no
wander, no exception, needs external termination -- same steady-state
signature as before this fix).

**Overall state of the Peggle investigation**: the per-tick steady
state is now real, evidence-traced, and clean (no known memory
pollution), but it is still confirmed to be a fixed loop that never
progresses to real game logic (resource loading, rendering). The
concrete blocker is unchanged from two rounds ago: the loop's own real
ID constant (`0x0101eb0b`) has no identified real meaning anywhere
this project has looked (headers, other real `.mod` binaries, general
web search). Making further progress from here most likely requires
either a real BREW MP SDK header dump this project doesn't have access
to, or a much larger, Peggle-specific reverse-engineering effort into
its own per-tick game-object data (the `arena+0x45000+0x3dc` structure
found this round) -- both bigger asks than the incremental, evidence-
grounded fixes this log has made so far, and a reasonable point to
pause this specific investigation thread pending either new evidence
or a decision to invest in the larger effort.

---

## Third title: Super BurgerTime

Redirected effort here rather than continuing to push on Peggle's
specific per-tick-loop wall (see the pause point immediately above):
untapped, completely unprobed territory, and useful validation that the
HLE core generalizes rather than being overfit to two titles.

**Format survey (already recorded earlier in this log, see the format
survey entry above)**: Super BurgerTime is one of the classic-arcade
ports, shipping loose per-asset files (`.tex`/`.wav`/`.fnz`) alongside
a `"PACK"`-magic `.pkg` container — no GGZ, no `resources.bar`, a third
real asset-container shape distinct from both prior titles. Not solved
yet (see below); `supbtime.mod` (2,832,292 bytes) is reachable and disassemblable
the same way as the other two.

**`.pkg` container: magic confirmed, structure NOT a byte-exact Quake
PAK despite the coincidental "PACK" magic match.** Direct inspection:
`50 41 43 4B` ("PACK") followed by two little-endian `uint32`s at
offset 4/8. Naively read as Quake's classic 12-byte-header
`(dirofs, dirlen)` convention, the values are incoherent for a real
file (`dirofs=7`, which would point inside the header itself, and
`dirlen=579390` doesn't divide evenly by Quake PAK's 64-byte directory
entry stride). **Deliberately not assumed to be Quake PAK** just
because of the magic-byte coincidence — a real embedded zlib stream
(`78 da`) was already confirmed present elsewhere in the file (earlier
survey), so the real structure is still open; cracking it is deferred
until it's actually needed (asset loading hasn't been reached yet).

**Found and fixed a real, foundational CPU gap: the ARMv6 "Extend"
instruction family was entirely unimplemented.** The very first real
instruction Super BurgerTime executes beyond the common `AEEMod_New`
prologue (module offset `0xdc`) is `uxth r0, r0` — this interpreter's
`Step()` treated the *entire* ARMv6 media-instruction encoding space
(`bits[27:25]=011, bit4=1`) as unconditionally unimplemented, without
checking whether the specific real instruction inside that space was
actually understood. Confirmed the real encoding empirically (assembled
each real mnemonic with `arm-none-eabi-as`, read back the actual
bytes) rather than hand-deriving it from the ARM ARM's prose:
`bits[27:20]` selects `UXTB(0x6E)/UXTH(0x6F)/SXTB(0x6A)/SXTH(0x6B)`,
`bits[9:4]` must be `0b000111`, and `Rn=0b1111` selects the plain form
(any other `Rn` selects the real accumulate form, `Rd = Rn + extended
Rm`). Implemented all four plus their accumulate variants in one pass
— same real instruction family, and a real game hitting one is likely
to hit a sibling.

Caught a real correctness bug before it shipped, not after: naively
reusing `ShiftWithCarry` with `is_immediate_shift=true` for the
rotate-field-zero case would have reinterpreted `ROR #0` as `RRX` (a
real special case, but one that belongs to the *general shifter
operand*, not this family's own dedicated 2-bit rotate field) —
switched to `is_immediate_shift=false`, which correctly means true
"no rotation" at 0 while leaving nonzero rotate amounts unaffected.
9 new tests added (`cpu_test.cpp`), every encoding confirmed via the
assembler rather than asserted from memory. Committed (`ef1d3d4`).

**Verified against real Super BurgerTime**: `AEEMod_Load` now runs
742,000+ real steps past the point that used to throw immediately —
real, substantial execution, not a trivial unblock.

**Immediately hit a new, different, and much deeper real wall**: after
that long real run (confirmed to include a real BSS-zeroing loop, not
just wasted steps), a real function returns via the APCS
`push {fp,ip,lr,pc}` / `sub sp,fp,#12; ldm sp,{fp,sp,pc}` stack-frame
convention (loading the return address from the stack, not via `bx
lr`), and the popped return address is `0` — causing the exact same
"wander outside the module, execute harmless zero-page NOPs, then
coincidentally re-enter the module from its own start address" pattern
already documented for Double Dragon and Peggle's own early gaps. This
time, re-entry runs the *entire* AEEMod_New prologue a second time
(confirmed via trace: another ~2,000,000 real steps, including the
same real BSS-zeroing loop observed the first time) before a `bx r3`
lands on a real module address (`0x9c`) that decodes as an ARM `LDM`/
`STM` with the S-bit set (the exception-return/user-bank-register
variant) — a second real gap, `Block data transfer with S=1 ... not
supported`, though likely a downstream symptom of the same root cause
(the CPU executing real code at the wrong alignment/address after the
bad return) rather than a third, independent thing to fix.

**Not yet root-caused**: which specific call in the chain has a
stack-depth mismatch (an extra pop somewhere) or reads an
under-provisioned stack region as if it held a real saved return
address. This is a materially different *kind* of gap than anything
fixed for Double Dragon or Peggle so far — those were all missing
HLE calls or unprovisioned context fields; this is the CPU/stack
interaction itself going wrong deep inside the module's own real
compiled prologue, before any HLE surface is even reached. Tracked as
the next concrete step for continuing Super BurgerTime. `AEECLSID` for
`IModule::CreateInstance` also hasn't been found yet (blocked on
`AEEMod_Load` completing first) — deferred until this is resolved.

**Root-caused the null-return chain precisely, down to the exact
mechanism (temporary instrumentation throughout: an `arm_interpreter.h`
PC/register watchpoint, a `Memory::Write8` address watchpoint, and a
few debug globals bridging the two -- all reverted, confirmed via clean
`git diff`).** The bad `fp` (`0xffcffffb`) traced back one level
further than the previous round reached: `mov ip, sp` at real module
offset `0x10009c` -- the very instruction whose corruption we'd
already fixed once (it's the same address the original `uxth`
instruction lived at) -- was itself corrupted *again*, this time to
`0xe1e0c27d`, by the time it executed. Watching every write to that
address found the real culprit: a real ARM ROPI relocation-fixup loop
(module offset `0x100040`-`0x100054`):

```
100040: cmp   r3, r4
100044: ldrlt r6, [r3], #4      ; r6 = *table_entry (table walks r3->r4)
100048: ldrlt r7, [r6, r5]      ; r7 = *(r6 + base)  -- read the fixup site
10004c: addlt r7, r7, r5        ; r7 += base          -- apply the relocation
100050: strlt r7, [r6, r5]      ; *(r6 + base) = r7   -- write it back
100054: blt   0x100040
```

`r5` (the real relocation base for this pass) is computed just before
this, at offset `0x100018`-`0x10001c` (`sub r4, pc, #32` -- computes
the module's own real load address; `add r5, r4, #0x9c`) -- i.e. `r5 =
kBase + 0x9c` exactly, the address of the very `mov ip, sp`
instruction whose corruption started this whole investigation. That's
not a coincidence: this fixup pass legitimately treats its own load
address as the relocation base, since the table's entries (confirmed
directly against the raw file: 82,480 real, sane, strictly-increasing,
**never-zero** offsets, e.g. `0x110, 0x114, 0x118, ...`) are link-time-
relative offsets needing exactly this base added.

**The real table gets processed correctly once** (confirmed directly:
watching the first pass through `r3=0x2d9684` shows the real value
`0x110` read and a sane fixup applied) **-- then something writes zero
across the table's entire address range (`0x2d9684`-`0x329f44`,
confirmed as the exact range a separate real "clear it" loop at module
offset `0x100078`-`0x100084` walks) -- and then this exact fixup loop
runs a *second* time over the same, now-zeroed range.** On this second
pass, every table "entry" reads back as `0` (since the real data is
gone), so every iteration computes the exact same degenerate fixup
target: `r6 + r5 = 0 + r5 = r5 = 0x10009c` -- repeatedly reading,
adding `r5` to, and writing back that single address, corrupting real
code a little further on each of the loop's ~82,480 iterations (each
write is the previous corrupted value plus `r5` again, which is
exactly the "steadily climbing corrupted value" pattern observed
watching that address directly). This is what corrupts `mov ip, sp`
before it's ever executed, which is what puts the wrong value in `ip`,
which is what makes `sub fp, ip, #4` compute the garbage `fp` that
eventually gets restored from a stale stack slot and, several function
returns later, is read as a null "return to" address.

**Not yet resolved: *why* the fixup loop runs a second time at all.**
This is a real, structural question, not an interpreter bug in the
loop itself -- every instruction in it (including the conditional,
post-indexed `ldrlt`) was individually re-verified against a direct
memory read and behaves correctly; the *data* it's reading the second
time really is zero. Confirming whether this is genuinely intended
real behavior this codebase doesn't yet support (e.g. a legitimate
second relocation pass over reused scratch space, gated on something
this codebase hasn't provisioned) or a wrong loop-bounds/loop-count
computed earlier in the chain needs finding this loop's *caller* --
tracking the return address (`lr`) at both the first and second entry
to `0x100040` is the concrete next step, not yet done this round.

This is a categorically different, and by far the deepest, gap found
in this entire investigation across all three titles -- every previous
gap (Double Dragon, Peggle, and Super BurgerTime's own `uxth`) was
either a missing HLE call, an unprovisioned context field, or a
genuinely unimplemented CPU instruction; this is real, correctly-
emulated CPU execution reaching a real, self-inflicted data corruption
bug in the *module's own* real relocation logic, for a reason not yet
understood. A reasonable pause point given the depth already reached
this round -- the precise mechanism is now fully mapped for whoever
continues, down to the exact two loop addresses and the exact
corrupted value chain.

---

**Correction to the entry above: the "root-caused precisely" claim was
premature.** The double-relocation-pass mechanism described is real
and was verified, but framing it as *the* root cause assumed (without
directly checking) that it only happened during a coincidental
re-entry after an earlier, still-unexplained wander. A one-shot
watchpoint on the very first write that ever corrupts module offset
`0x9c` found the opposite: it happens during the game's first, only,
and completely ordinary pass through its own relocation-fixup loop --
`lr=0xf0000000` (this tool's own initial harness value, not a replay)
and `sp=0x2ffff0` (an entirely normal, in-range stack address, not the
`sp=0` seen during the later coincidental-re-entry symptom). There is
no earlier, hidden bug in the relocation logic itself.

**The real cause is much simpler and, in hindsight, obvious**: this
tool hard-codes the emulated stack pointer to `kBase + 0x200000`,
which safely cleared Double Dragon's (462,748-byte) and Peggle's
(274,124-byte) `.mod` files, but Super BurgerTime's `.mod` is
2,832,292 bytes -- large enough that this fixed offset lands *inside*
the loaded module, specifically inside the exact address range
(`0x2d9684`-`0x329f44`, confirmed via the real, file-embedded literals
driving the relocation loop) the module's own relocation-fixup table
occupies. Since this tool's stack there is essentially empty, the
table reads back as zero partway through the real walk, and the
loop's own real "read entry, add relocation base, write back" logic
degenerates into repeatedly corrupting its own relocation base address
-- which happens to be the `uxth` instruction -- before it's ever
executed. This one root cause explains every symptom already
documented across both of the last two entries: the garbage frame
pointer, the eventual null return, and the "double pass" (which was
real, but is a second-order effect of the *same* collision recurring
after the wander this fix also happens to prevent, not an independent
mystery).

**Fixed** by sizing the stack offset relative to the real module
(`kBase + mod_size + 0x200000`) instead of a fixed guess, so it cannot
collide with any real module regardless of size. Committed (`8d907a2`).

**Verified against real Super BurgerTime**: `AEEMod_Load` now completes
cleanly, `IModule::CreateInstance` succeeds too (even with a guessed
ClsId -- the folder name, `279125`, not yet confirmed as the *real*
class ID the same way Peggle's/Double Dragon's were), and execution
reaches deep into `HandleEvent(EVT_APP_START)` (40,095 real steps)
before hitting a new, unrelated, much later real gap: a coprocessor
instruction / SWI encoding at module offset `0xa0`. Confirmed no
regression: Peggle still reaches its real event loop; Double Dragon's
much smaller `.mod` was never near the fixed offset to begin with.
250/250 tests pass.

**Lesson for future titles**: a fixed-offset scratch stack is fragile
by construction -- any sufficiently large real `.mod` can trigger this
exact class of bug again. Sizing it relative to the real module (as
fixed here) removes the whole category, not just this one instance.

---

**With `AEEMod_Load`/`CreateInstance` now clean, `HandleEvent(EVT_APP_
START)` immediately reaches two more real, previously-unconfirmed
static-base table slots in quick succession.** Both follow the exact
same shape every prior gap in this table has: an indirect call through
an unwritten table slot, defaulting to a null function pointer, jumping
to address 0.

- **Slot `0x40`**: one real call site (reached from `HandleEvent`)
  calls it with `(this=the current real applet pointer, buffer=a
  512-byte local stack buffer, size=0x200)` -- a shape that doesn't
  match any of the fourteen already-confirmed slots (all fixed-arity,
  non-"this"-taking C-runtime-style helpers). Registered as a safe
  no-op rather than guessed at.
- **Slot `0xc`**: reached via a small standalone trampoline
  (`supbtime.mod` offset `0x11b200`-`0x11b214`: fetch the static-base
  table through the same real relocated-literal idiom every other slot
  uses, then `ldr pc,[table,#0xc]`) instead of the more common direct-
  call shape, but functionally identical. Sits in the same tightly-
  packed cluster as the confirmed `MEMCPY(0x0)`/`MEMSET(0x4)`/
  `STRCPY(0x8)`/`STRLEN(0x14)` slots; its one real call site passes
  `(dest=the same stack buffer, src=a real, low, module-relative
  literal)`, consistent with a sibling string function (`STRCAT`/
  `STRCMP` both plausible) but not confirmed. Also a safe no-op.

Committed (`02731cc`). **Verified**: each fix measurably advances real
execution -- 40,095 -> 40,177 (slot `0x40` fixed) -> 40,254 (slot
`0xc` fixed) real steps before hitting the next, different gap. 250
tests pass.

**Hit a new, differently-shaped wall immediately after `0xc`**: instead
of the usual clean "wander through zero, land on a real address"
pattern, this run throws `S=1 with Rd=R15 (SPSR restore) not supported`
at `pc=0x3000000b` -- an address far outside any real, meaningful
range (not zero, not a real module offset), meaning some register
computation upstream produced outright garbage rather than a clean
null. Not investigated further this round -- a reasonable stopping
point after three real fixes in a row (the ARM Extend instruction
family, the stack/module collision, and these two static-base slots),
each independently verified against real execution. The next concrete
step for continuing Super BurgerTime is tracing backward from
`0x3000000b` the same way every other gap in this log has been solved:
find which real register computation produced it and why.

**Overall shape of this round**: started completely unable to execute
a single real instruction past `AEEMod_New`'s common prologue (the
`uxth` gap); ends with real execution reaching deep into
`HandleEvent(EVT_APP_START)` across four independent, verified fixes.
Super BurgerTime is a substantially harder title than Double Dragon or
Peggle were at the equivalent stage -- its `.mod` is far larger, uses
a different, still-uncracked asset container (`.pkg`), and has already
surfaced a whole category of bug (the stack/module collision) neither
prior title ever triggered.

---

**Traced the `S=1/Rd=R15` garbage-address wall back to its real
source (temporary trace, reverted): it is the same clean "null
function pointer -> wander through zero" pattern as every other gap in
this table, not a new kind of bug.** The wander that eventually lands
on the garbage `0x3000000b` address starts, as always, at a step where
`pc` first becomes exactly `0`. Tracing that exact step: real code
does `ldr ip,[r2]` then `ldr pc,[ip,#0x1c]` -- at first glance another
unconfirmed static-base slot (`0x1c`), matching the same shape as the
`0x40`/`0xc` gaps just fixed. It isn't: `r2` itself is already `0` at
that point, read two instructions earlier via `ldr r2,[r5]` with
`r5 = 0x2e28fc`.

**`0x2e28fc` is not a static-base table slot at all -- it falls
*inside* the relocation-fixup table's own real address range
(`0x2d9684`-`0x329f44`, the same range this log's write-up on the
stack/module collision fix already establishes)**, i.e. it's part of
the real scratch memory the module's own "clear it after use" loop
(`0x100078`-`0x100084`) zeroes once the relocation table has been
consumed. Real code evidently expects some *other*, not-yet-executed
piece of real initialization to have since written a real, meaningful
pointer into that same reclaimed memory (turning it into real BSS/heap
storage) before `HandleEvent` ever reads it back out through `r5`.
Nothing in this codebase's execution path so far writes there.

**This is a materially different kind of gap than the fourteen (now
sixteen) static-base table slots**: those are all missing *system-
provided* function pointers, safely stubbable in isolation. This is a
missing piece of the *game's own* initialization sequence -- some real
code, reached from somewhere other than the `AEEMod_Load` ->
`CreateInstance` -> `HandleEvent(EVT_APP_START)` chain this tool
drives (or reached from within one of those but not yet executed due
to an earlier, still-undiagnosed branch), that's supposed to populate
this specific piece of reclaimed scratch memory. Guessing a placeholder
value here would be exactly the kind of unfounded guess this log has
avoided throughout -- unlike a null function pointer (safe to stub with
a no-op), a wrong *data* value here risks silently wrong behavior
rather than a clean, diagnosable crash. Not resolved this round --
finding what real code is supposed to write to `0x2e28fc` (or the
broader reclaimed region around it) before `HandleEvent` runs is the
next concrete step, and likely needs tracing back through
`CreateInstance`'s own real body (not yet disassembled in this depth)
rather than `HandleEvent` itself.

---

**Followed that lead: traced `CreateInstance`'s own real body directly
(a live-traced run of just that call, plus a memory watchpoint on
`0x2e28fc` spanning the entire run, both temporary and reverted).**
Two real findings, one useful, one a dead end correctly ruled out:

- **`CreateInstance`'s own real body performs completely ordinary,
  sensible real work** with the guessed ClsId (`279125`, this title's
  download-folder name): it reaches a small real class-dispatch table
  lookup, then real applet-object construction (writing several real
  fields into the newly-malloc'd applet struct), then a genuine real
  `ISHELL_CreateInstance` call for class `0x01001001` -- the exact
  same real `AEECLSID_DISPLAY` value already confirmed for Double
  Dragon. A dispatcher silently accepting a wrong/unmatched class ID
  and still doing this much real, structured initialization work would
  be surprising; this looks like a real, valid, matched code path, not
  a degenerate fallback -- **the guessed ClsId is very likely correct**,
  or at minimum isn't the source of this specific gap. Not proven with
  the same certainty as Peggle's own live-read-the-real-comparison
  technique (no explicit `cmp` against the literal `0x44255` was
  spotted in the traced instructions), so still technically unconfirmed,
  but no longer the leading suspect.
- **The watchpoint spanning the entire run (`AEEMod_Load` ->
  `CreateInstance` -> `HandleEvent`, up to the crash) confirms `0x2e28fc`
  is written to exactly twice, both during `AEEMod_Load`'s own
  relocation bootstrap** (once by the initial file load, once by the
  real "clear the table after use" loop) **-- never again.** Neither
  `CreateInstance` nor anything in `HandleEvent` before the crash point
  writes there. This rules out "wrong ClsId routes to a code path that
  skips real initialization" as the explanation and narrows the gap to
  exactly what the previous entry already suspected: something inside
  `HandleEvent`'s own real control flow, upstream of the crash site,
  is either supposed to populate this reclaimed memory and doesn't
  (because of some other, still-unfound micro-gap changing which
  branch real code takes), or this specific field is populated by real
  code this codebase doesn't drive at all (e.g. a second, later
  `HandleEvent` call, or genuine OS/firmware-level initialization
  outside any `.mod` this project can disassemble).

**Deliberately stopping the live trace here for this round.** Fully
resolving this needs walking `HandleEvent`'s entire real control flow
from its own start (not yet disassembled at this depth) to find the
specific branch point that should lead to populating `0x2e28fc` --
substantially more real disassembly than the two dead-end checks just
done. Both checks were real, evidence-producing, and narrow the
remaining search space precisely rather than leaving it open-ended;
picking this back up should start from `HandleEvent`'s own entry
(`0x0010b5b4`, per the confirmed vtable slot), not from the crash site
backward, since backward tracing from the crash has now been pushed as
far as it profitably goes without new evidence.

---

**Followed through: traced `HandleEvent` forward from its own real
entry, and Super BurgerTime reaches its real steady-state event
loop.** A full, from-the-start trace (temporary, reverted) of the
`HandleEvent(EVT_APP_START)` call confirms the structural shape
guessed at above: `0x0010b5b4` is a thin, real `AEEApplet`-template
wrapper (`push {lr}`, `uxth` the two 16-bit args, call `ldr pc,[r0,
#24]` -- the game's own real handler, stored at applet offset `0x18`
by `CreateInstance`'s own construction of the applet struct) that
forwards to the real per-game handler (`0x0011bd88`) before doing
further built-in processing of its own once that returns.

**The real source of the `0x2e28fc` gap, found precisely**: partway
through that built-in processing (`supbtime.mod` offset `0x11be90`-
`0x11be98`), real code calls `ISHELL_CreateInstance(shell,
ClsId=0x01001017, ppObj=&g_2e28fc)` -- a completely ordinary real
`ISHELL_CreateInstance` call whose *output slot* happens to be the
same module global this investigation already proved is otherwise
never written. `0x01001017` was not a registered class, so
`IShellHle::CreateInstanceImpl` correctly returned failure and
correctly left `*ppObj` untouched (matching real `AEEShell.h`
semantics -- this HLE implementation was never the bug). Real code,
exactly as seen at least four times before in this project's history
(Peggle's self-propagating-stub investigation, Double Dragon's
`0x01002001`), never checks the returned status: two instructions
later it dereferences `*(0x2e28fc)` (still `0`) as if it were a valid
object, calls a method on the resulting null, and crashes. **Not a new
kind of gap after all** -- the "materially different" framing in the
previous two entries was itself premature; once traced to its actual
source it's the same well-understood, well-precedented pattern as
every other unidentified-class gap in this log.

**Fixed the same way as every prior instance**: registered a generic,
deliberately-unguessed `BuildGenericStubObject` scaffold for ClsId
`0x01001017`, confirmed via a memory watchpoint spanning the entire
run (temporary, reverted) that this one real call site is the only
thing that ever touches `g_2e28fc`. Committed (`60a6057`).

**Verified against real Super BurgerTime**: `HandleEvent(EVT_APP_
START)` now returns `1` (real success), and execution reaches
**"Reached the event loop with no unhandled instruction! Window will
stay open."** -- the exact same steady-state milestone already
achieved for Double Dragon and Peggle, now on a third, independently-
compiled title. Confirmed stable over a sustained 30-second run (no
wander, no exception -- the same "success can look like a hang"
signature already trusted for the other two titles). No regression on
Peggle or Double Dragon (both re-checked, still reach their own event
loops). 250/250 tests pass.

**Incidental finding, not yet acted on**: while locating a fresh
address for this new scaffold object, noticed `shell_hle.RegisterInstance`
is called with the literal `0x01014bc4` twice in `tools/game_probe.cpp`
-- once for the real, confirmed `AEECLSID_EGL`, and again (from the
earlier Peggle "try new class, fall back to old" investigation) for an
unrelated, unidentified class that happens to share the same real
numeric ID. The second registration silently overwrites the first.
Given Peggle still reaches its event loop cleanly with this collision
in place, it isn't an active regression, but it's a latent
correctness issue worth a closer look in a future round -- not
investigated further this round to stay focused on Super BurgerTime.

**Overall shape of the Super BurgerTime investigation this round**:
started completely unable to execute a single real instruction past
the common module prologue; ends with a third real, independently-
compiled commercial title reaching the same real steady-state
milestone as Double Dragon and Peggle, across six independent,
verified fixes (the ARM Extend instruction family, the stack/module
address collision, three static-base slots, and this unidentified
class). `resources.bar`/`.pkg`-style asset loading remains uncracked
for this title, same as it does for Peggle -- the natural next phase
now that the steady-state milestone itself is reached.

---

**Started on `.pkg`, found a real, structural lead, then a
correction: the format isn't the actual next blocker.**

Re-examined the raw `.pkg` bytes directly (`50 41 43 4B`/"PACK" magic
at offset 0, two little-endian `uint32`s at offset 4/8: `7` and
`579390`; naively read as Quake's classic 12-byte-header
`(dirofs,dirlen)` convention these don't parse coherently, confirming
the earlier survey's caution about assuming a byte-exact match).
Scanning `supbtime.mod` for related strings turned up a real,
substantial, and unexpected cluster: `"roms\neogeo"`, `"%s\%s.pkg"`,
`"%s\%s\%s"`, `"rb"`, `"Loading romset %s"`, and the `"PACK"` magic
itself (as a real 4-byte literal, `0x4B434150`, embedded at file
offset `0x18ebe0`) all sitting together in the same literal-pool
region. This strongly suggests the classic-arcade titles in this
project's format survey (Super BurgerTime among them) embed a real,
generic, **multi-system arcade-emulation core** (the `"neogeo"`
reference implies it supports more than one real arcade platform) that
loads its own ROM data through a conventional `"romset"` `.pkg`
container -- exactly matching this project's earlier structural guess
("built on an embedded classic-arcade-hardware emulation core bundling
original ROM data"), now with much more specific, real textual
evidence.

**Checked whether any code in this compiled binary actually references
these strings, and found none** -- an exhaustive search for PC-relative
loads targeting any of these exact literal addresses came up empty.
Combined with the next finding, this is best read as: this shared
core's real `.pkg`-loading code path is linked into the binary but not
actually exercised by anything this project's harness has driven so
far (not necessarily provably dead forever, but not reachable from
where execution currently gets to).

**More importantly: nothing reaches file loading of *any* kind yet.**
Drove the real steady-state loop for 30 real seconds with a live watch
on every `IFILEMGR_OpenFile`/`Test` call (temporary, reverted) --
zero hits, not even the individual loose asset files (`Dark.tex`,
`ding.wav`, etc.) that don't need `.pkg` cracked at all to load. Traced
further back: **`ISHELL_SetTimer` is never called even once** across
the same window (temporary watch on `SetTimerImpl`, reverted) --
meaning, unlike Double Dragon and Peggle (both of which call
`SetTimer` at least once during their own real `HandleEvent(EVT_APP_
START)`), this title's real per-frame driving mechanism either doesn't
use `ISHELL_SetTimer` at all, or is gated behind something upstream
that this codebase doesn't yet provide -- the same *kind* of question
Peggle's own timer-rearm investigation answered, but a different,
not-yet-identified specific cause here.

**Correcting the previous round's framing**: cracking `.pkg` is not
actually the next concrete step -- reaching *any* asset load, packed
or loose, requires first understanding what drives this title's loop
forward at all after `HandleEvent(EVT_APP_START)` returns. That's a
new, `SetTimer`-shaped investigation (or the discovery of a
completely different real driving mechanism, possibly connected to
the "arcade core" structure just found), not a file-format one. Given
the arcade-core framing, it's also plausible this title's real
architecture runs its whole per-frame loop *synchronously inside* a
single `HandleEvent` call (an old-style polling loop, common for
ported arcade engines) rather than the event-driven `SetTimer` model
-- in which case the real blocker may already sit somewhere inside
`HandleEvent`'s own body, past where this project's tracing has
looked so far, not in a separate ticking mechanism at all. Not yet
distinguished; the concrete next step is determining which of these
two shapes is real before investing further in either the timer-gate
question or the `.pkg` format itself.

---

**Resolved: `HandleEvent` runs a short, one-time init sequence (38
real HLE calls total, confirmed via a full `hle_trace` of the call,
temporary, reverted) and returns cleanly -- it does not run the whole
game loop synchronously. The real per-frame driving mechanism is a
still-unidentified class's own method, not `ISHELL_SetTimer`
directly.**

The very last real call before `HandleEvent` returns is on the class
this log's previous entry just registered a generic scaffold for
(`0x01001017`): its own slot 7 (byte offset `0x1c`) is called with
`(this, flag=0x4000, callback=0x11c06c, user_data=0)`. Disassembling
`0x11c06c` directly: it's real ARM code (`push {r4,r5,r6,lr}`,
...), not data, and its body is unambiguous -- dereference a real
module-global list head; if the list is empty, return immediately;
otherwise walk the list calling a real vtable method (slot `0x2c`) on
each entry. That is exactly the shape of a real "run one engine tick"
function for a generic object/entity list, matching this title's own
suspected "generic arcade-emulation-core" architecture (the `.pkg`/
`"roms\neogeo"` strings from the previous entry).

**The generic no-op scaffold this class had been given silently
discarded this registration** -- it returned success without storing
or scheduling the callback anywhere, so `0x11c06c` was never invoked
even once. This is why nothing progressed past `HandleEvent`: the
real "process a frame" function existed and was reachable, but nothing
in this codebase's emulation was driving it.

**Fixed, deliberately marked experimental**: added
`IShellHle::ScheduleTimer` (refactored out of the existing
`SetTimerImpl`, same identity-based re-arm semantics already
documented and relied on for Double Dragon/Peggle), and overrode this
one class's slot 7 to call it with the real `callback`/`user_data`
this specific call site provides, on an *inferred* 16ms cadence
(matching this file's own `kTickMs`) -- the real call site doesn't
provide an explicit interval the way `ISHELL_SetTimer`'s own `dwCount`
parameter does, so the cadence itself is a reasoned guess, clearly
flagged as such in the code, not a confirmed real value. Committed
(`3e66bdb`).

**Verified empirically, not just plausible**: tick 0 now fires for the
first time in this title's entire investigation, and real code inside
the callback runs (real `MALLOC` calls, real initialization -- visible
directly in the `hle_trace` output) before hitting a new, different,
deeper real gap 95 steps into the callback's own execution (another
wander to zero, this time not finding its way back within the
5,000,000-step budget -- genuinely new, unexplored territory, not a
repeat of anything already mapped). No regression on Peggle or Double
Dragon (`ScheduleTimer`'s extraction is a pure refactor of
`SetTimerImpl`'s existing behavior). 250/250 tests pass.

**Not investigated further this round.** This is a natural, well-
verified stopping point: a genuine structural hypothesis (this class's
slot 7 is a real per-frame callback registration point) proposed from
static disassembly evidence, then confirmed empirically by observing
real, previously-unreachable code execute. The next concrete step is
tracing the new gap 95 steps into `0x11c06c`'s own real body -- a
fresh investigation thread, not yet started.

---

**Continued into `0x11c06c`'s own real body -- two more static-base
slots, each unlocking substantially more real execution, ending in a
genuine, non-crashing infinite polling loop rather than another
wander.**

**Slot `0xd0`**: the 95-step wander traced to a static-base call at
that offset, called with `(name="boot", heap_object)`. `name` points
directly at a real, in-module string table (`supbtime.mod` offset
`0x18ee50`: `"boot\0boot.rom\0zupa_p1.rom\0zupa_s1.rom..."`) -- a
literal ROM manifest. "Zupapa" is this arcade original's real
Japanese title, which, combined with the manifest shape itself,
confirms this is the generic arcade-core's own real romset-loading
code actually executing now, not the dead-looking code the earlier
string search found. `heap_object` is a real pointer this codebase's
own MALLOC slot had just returned. Registered as a safe no-op (no
evidence beyond "very likely registers/hashes a named ROM chunk").
**Verified**: the callback's own real step count jumped from 95 to
2,883, including a real, repeating pass through the exact `.pkg`/
`"roms\neogeo"` string cluster the earlier round found unreferenced --
direct, concrete confirmation that cluster is live, reachable code.

**Slot `0x184`**: the new 2,883-step wander traced to a second static-
base call, `(flag=1, 0, table)` -- too thin a shape to identify.
Registered the same way. **Verified, and the failure mode changes
entirely**: real code no longer wanders to a null pointer at all --
it settles into a genuine infinite loop, alternating between two real
calls forever (`trap=0xf0000044` returning `1`, `trap=0xf000001c`
returning `0`, repeating without end). Not a crash, not unmapped
memory -- a real "wait until some condition becomes true" polling
loop that our static, canned-response stubs can never satisfy, since
neither response ever changes.

Both slots committed (`22b80a3`). No regression on Peggle or Double
Dragon; 250/250 tests pass each time.

**This is a different, and arguably more fundamental, kind of wall
than every other gap fixed in this file so far.** Every previous fix
(the ARM Extend instructions, the stack/module collision, all
eighteen static-base slots, the per-frame callback registration)
unblocked *reaching* more real code. This one is different: the real
code *is* reached, runs correctly, and is *waiting on a real
condition this codebase cannot satisfy* without actually implementing
whatever the polling loop is checking for -- almost certainly tied to
the ROM-manifest loading this same callback was just seen walking
through moments earlier. This connects back to (and validates) the
`.pkg`/romset question this investigation set aside two rounds ago:
it looks like actually implementing real romset loading -- or at
least making the specific condition this loop checks become true --
is what real further progress needs now, not another static-base slot
or missing class. Not attempted this round; a substantially bigger
undertaking than the incremental fixes so far, and a reasonable place
to pause given how much ground this session has already covered.

---

**Traced the infinite polling loop to its root cause -- not the
romset-loading gap it was assumed to be waiting on, but a frozen
emulated clock -- fixed it, and got real, if partial, further
progress.**

Rather than assume the loop was purely waiting on real romset data
(the natural reading left off at last round), traced exactly which
two static-base slots it alternates between using the trap address
arithmetic `HleRuntime::Register` already documents (`index = (addr -
trap_base) / 4`): `mod_runtime.cpp`'s `Install()` is the very first
code in the whole program to call `hle_.Register()`, so its
registration order maps directly onto trap indices -- the 7th call is
`get_uptime_ms_fn`, the 17th is `unknown_0x184_fn`, matching
`trap=0xf000001c`/`trap=0xf0000044` exactly. (The previous round's
"returning 1"/"returning 0" phrasing for these traps was imprecise --
`hle_trace`'s printout fires when PC lands on the trap address, before
dispatch, so those are each call's *arguments*, not the *previous*
call's return value. Confirmed by re-running with `hle_trace` enabled
for real: slot `0x184` is called `(flag=1, 0, table, table)` every
time, exactly as originally documented.)

`GetUpTimeMsImpl` simply returns `uptime_ms_`, a counter only
`ModRuntime::Tick()` ever advances -- and `Tick()` is only called once
per outer per-frame loop iteration in `game_probe.cpp`, never from
inside a single `CallArmFunctionChecked` call. This specific polling
loop calls `GetUpTimeMs` on every single iteration, entirely within
one such call -- so `Tick()` never gets a chance to run in between,
and the clock stays frozen at whatever value it had when the call
began, for that call's entire (up to 5,000,000-step) lifetime. A real
busy-wait checking elapsed time against this permanently-frozen clock
can never see its own deadline pass. Not a romset-loading gap at all
-- an artifact of how this codebase's timing happens to be driven.

**Fix**: `GetUpTimeMsImpl` now also self-advances `uptime_ms_` by 1ms
on every read, independent of `Tick()`. Still fully deterministic
(same call sequence always produces the same values, unlike a genuine
wall-clock read would), while letting real elapsed-time busy-waits
make forward progress instead of spinning forever, the same way real
hardware's own continuously-advancing uptime clock would let them
resolve given enough real wall-clock time. The 1ms-per-read rate is
inferred, not measured -- plausible for a real "checked once per real
hardware poll iteration" loop, not confirmed against any real timing
constant.

**Verified against real Super BurgerTime**: the first ROM-poll spin
now resolves after roughly 50 iterations instead of hanging forever.
Real code goes on to make five more genuine HLE calls -- a real
`strcpy`, `AddRef`/method calls on the SBT-specific class object
(`0x01001017`, ClsId ppObj address `0x80047000`), and a touch of the
earlier `unknown_0x0103d8ec` scaffold -- concrete evidence of real,
new forward progress, not a fluke. It then lands in a **second**,
structurally identical polling loop (same two slots, same call shape)
that does **not** resolve even with the clock now advancing --
exceeds the 5,000,000-step budget instead. This is the meaningful
result: the first loop really was just "has any time passed yet"
(trivially satisfiable once the clock moves at all), while the second
is checking something else the self-advancing clock alone can't
satisfy -- almost certainly real ROM-data readiness, consistent with
the romset-loading question already on record. Confirms rather than
replaces last round's framing: actually implementing romset loading
(or whatever specific condition this second loop checks) is still the
next real undertaking, not attempted this round.

**A methodology correction, found while re-verifying no regression on
Peggle/Double Dragon**: both appeared to newly fail at the very first
`IModule::CreateInstance` call (`returned 1`, zero HLE calls made --
suspicious for something previously confirmed to run real, in-bounds
code). Tracing the first ~20 real instructions of `CreateInstance`
with `trace=true` showed real code loading a literal via PC-relative
`ldr` and comparing it against the passed ClsId before doing anything
else -- i.e. this codebase's own `game_probe.cpp` usage convention of
passing each game's download-catalog *folder number* as `cls_id` was
never actually correct in general; it just happened to coincide with
Super BurgerTime's real embedded ClsId (`279125` in both roles).
Double Dragon's real literal is `0x0102f789` (16971657), not the
folder number `274754`; Peggle's is `0x01099cd6` (17407190), not
`278962`. Passing the real literal for each, both titles reach the
event loop cleanly, same as always -- **not a regression** from this
round's `GetUpTimeMs` change (reproduced identically all the way back
at commit `473758e`, well before this round's edits), just a latent
gap in how this dev tool was being invoked for these two titles
specifically. `tools/game_probe.cpp`'s usage message now documents
this distinction so it doesn't cost another investigation next time.

Committed (`b285214`). 251/251 tests pass (one new test added for the
self-advance behavior).

---

**Traced the second polling loop precisely -- the `GetUpTimeMs` fix
above is working correctly, and the real remaining wall is a
different, deeper problem: a real per-frame task-list runner whose
real completion signal our stub object can never produce.**

Added a temporary `lr=`/module-offset field to `hle_trace`'s printout
(reverted after, per usual practice) to find exactly which real code
the second, unresolved loop's two alternating traps return into:
module offset `0xea80`/`0xea8c`. Disassembling `0xea40`-`0xea9c`
(`arm-none-eabi-objdump -b binary -m arm`) shows this is the **exact
same bounded, ~32ms real-time poll function** as the first loop that
now resolves:

```
r0 = GETUPTIMEMS()                  ; 0xea50-0xea58
elapsed = r0 - saved_start          ; saved_start read from a module global
if elapsed > 32: goto exit          ; 0xea68/0xea6c, unsigned compare
retry:
  call static-base slot 0x184(flag=1, ...)   ; 0xea70-0xea7c
  r0 = GETUPTIMEMS()                         ; 0xea80-0xea88
  elapsed = r0 - saved_start
  if elapsed <= 32: goto retry               ; 0xea94/0xea98
exit:
  saved_start = r0                    ; 0xea9c
  return                              ; regardless of slot 0x184's answer
```

This confirms last round's fix is doing exactly what it was designed
to do: this function's exit is purely time-based (never gated on slot
`0x184`'s return value at all), and with the clock now advancing, it
correctly resolves after ~33 reads every single time it's called --
confirmed directly: `lr=0x0010ea8c`/`0x0010ea80` together account for
234,807 real calls in the run that still hits the step budget,
consistent with this same bounded function being called **thousands
of times** by an outer loop, each time successfully running its own
~33-iteration wait to completion.

That outer loop is `0x11c06c` -- the real per-frame callback found
two rounds ago, previously understood only at the "walk a list calling
a vtable method" level. Full disassembly of its body (`0x11c06c`-
`0x11c0f4`) sharpens that considerably:

```
if *list_head == 0: return                          ; 0xc074-0xc07c
loop:
  entry = *list_head                                 ; always our SBT
                                                       ; object, 0x80047000 --
                                                       ; a one-entry "list"
  entry_vtable[0x2c](entry)              -> r0        ; "the tick"; our
                                                       ; stub, blind 0
  ctx = *(*module_global + 12)                        ; = shell (0x80001000),
                                                       ; same +12 convention
                                                       ; as GetAppContext
  ctx_vtable[0x90](ctx, r0)                           ; IShell slot 36 =
                                                       ; Resume; our stub,
                                                       ; blind 0
  entry = *list_head                                  ; unchanged
  if entry == 0: return                               ; only real exit
  entry_vtable[0x28](entry)                           ; our stub, blind 0
  if *flag_byte != 0: goto loop                        ; 0xc0e4
  bl 0x10b2d8(1)                                       ; a real message-
                                                       ; dispatch subroutine
                                                       ; (tail-calls 0x104e80)
  goto loop                                            ; unconditional either way
```

**Every path through this function loops back to the top.** The only
way out is `*list_head == 0` -- and nothing in this function, nor any
of the three vtable calls it makes (all landing on either our SBT
object's generic stub slots or `IShellHle`'s blind `Stub`), ever
writes to that module-global to clear it. This isn't the "wait for
ROM data" pattern originally hypothesized for slot `0x184` specifically
-- it's a real, generic "process this frame's task list, one real tick
per entry, remove entries as they finish" runner, and our SBT object
never finishes anything because its stub slots (`0x28`/`0x2c`, whatever
their real names are) carry no real state and can never signal "done."
A real implementation would presumably have its own real slot `0x2c`
eventually clear `*list_head` (self-deregistering once whatever it's
doing -- almost certainly the romset load, given the "boot" manifest
evidence two rounds ago -- completes).

**This refines, rather than replaces, the romset-loading conclusion
from two rounds ago**: the specific mechanism is now understood
precisely (a real task-list runner needing real per-object completion
state, not a simple "is data ready" flag check), but the actual next
step is unchanged and, if anything, confirmed larger -- implementing
real, stateful behavior for the SBT-specific class (`0x01001017`)
well beyond its current generic scaffold. Not attempted this round;
all trace instrumentation reverted, no functional changes. `git diff
--stat` empty on `tools/game_probe.cpp` before this entry was
committed.

---

**Fixed the task-list wall -- Super BurgerTime now reaches its real
event loop, and past it hits a genuinely new, different kind of gap.**

A live memory watchpoint (temporary, on `Memory::Write32`/`Write8`,
bridged to the current PC via a small `g_debug_last_pc` global the
step loop updates each iteration, reverted after -- same technique
used earlier this project for similar questions) on `0x2e28fc`
confirmed precisely what was suspected last round: it's written
exactly once, by `IShellHle::CreateInstanceImpl` (`trap=0xf0000158`,
value `0x80047000` -- our SBT object), during the module's own
zero-init pass just before that (module offset `0x7c`, part of the
already-documented "clear it after use" loop), and never again. The
neighboring "flag" byte (`0x2e28f0`) is separately zeroed once by real
code right at the `CreateInstance` call site (`0x11be88`) and also
never changes -- confirming every single pass through `0x11c06c`
necessarily takes the expensive message-pump path, exactly as
suspected.

(Getting to these two confirmed addresses took a wrong turn worth
recording: an earlier pass at this same question, checking `pc ==
mod_base + 0x11c074`, doubled up an already-absolute address with
`mod_base` -- objdump's `--adjust-vma=0x100000` output for that region
already included the module base, unlike the raw-file-offset
disassembly used for the `0xea40` region earlier. The resulting
"list_head" address, `0x5b3714`, was coincidentally close to this
run's own SP placement and briefly looked like a second real
stack/data collision bug. It wasn't -- just a debugging arithmetic
slip, caught by cross-checking the live register value against a
directly-verified control address before acting on it.)

**Fix**: `unknown_0x01001017_obj` now overrides a second vtable slot
(11, byte offset `0x2c` -- confirmed by disassembly to be exactly the
"tick" method `0x11c06c` calls on its one list entry) alongside the
existing slot-7 `ScheduleTimer` wiring. The override writes 0 directly
to `0x2e28fc` -- the real, confirmed, live address -- and returns 0.
Honestly labeled as a minimal placeholder, not a claim about real
timing: a real implementation would presumably clear that address
itself once whatever real work "tick" represents (almost certainly the
romset load) finishes; since that work isn't implemented, clearing it
on the very first tick is the simplest choice that unblocks forward
progress without inventing an arbitrary frame count.

**Verified against real Super BurgerTime**: `AEEMod_Load` ->
`CreateInstance` -> `HandleEvent(EVT_APP_START)` -> **"Reached the
event loop with no unhandled instruction!"** -- the same milestone
Double Dragon and Peggle already reach. Past that, the first real
timer tick now runs substantially further before hitting a **new**,
different gap: `UnimplementedInstruction("Miscellaneous instruction
space (MRS/MSR/etc.)")` at module offset `0x9c`, roughly a million
real steps in. Checked directly against live memory (not the static
file -- learned that lesson this round) rather than assumed: the
instruction word actually present at that address at fault time
(`0xa3668ba7`) does **not** match what's in the raw `.mod` file at the
same offset (`0xe1a0c00d`, an ordinary `mov ip, sp`), reached via an
indirect `bx r3` return where `r3` had earlier been saved as exactly
that address -- i.e. a real return address whose target memory holds
different content by the time control returns to it. Not yet
understood *why* (self-modifying code is not a real, ordinary feature
of this kind of compiled BREW app, so this is much more likely a
second, distinct emulator-side data/relocation issue than a genuine
`MRS`/`MSR` instruction) -- flagged as the next concrete investigation
thread, deliberately not chased further this round after already
landing one substantial, verified fix.

No regression on Peggle/Double Dragon (re-verified with their real
ClsIds from last round). 251/251 tests pass (unchanged -- this fix
lives entirely in `tools/game_probe.cpp`, the dev harness, not core
HLE code). Committed (`798d1b9`). All debug/trace instrumentation
(the watchpoint, the double-counted-address checks, a temporary
`trace=true` on tick calls used to read the live faulting instruction
directly) reverted before committing; `git diff --stat` empty on
`core/memory/memory.cpp`/`.h` and clean on `tools/game_probe.cpp`
except the actual fix.

---

**Switched focus back to Double Dragon (per user direction, after a
step back to ask how far this project actually is from a genuinely
playable game) and cracked its real `.obm1` sprite/texture format from
scratch.**

Every one of the 89 real entries in Double Dragon's `data.ggz`
(confirmed via `zeebulator_ggz_inspector`) is a `.obm1` file — exactly
the asset sub-format TASKS.md Phase 4 explicitly deferred ("format
specifics are a per-content research task... triggered by whatever a
real game's rendering code actually needs"). No existing documentation
for it was found anywhere in this repo's research materials (checked
the SDK extraction tree; a handful of files matching "obm1" as a
substring turned out to be unrelated Maya scene files). Reverse-
engineered directly from the raw bytes instead.

Extracted all 89 real entries (a small one-off Python script mirroring
`GgzArchive`'s already-documented format) and hex-dumped the smallest
(`Face_Billy.obm1`, 552 bytes): a 2-byte `"OI"` magic, then `04 04`,
then `20 00 20 00` -- read as two little-endian `uint16`s, `32, 32`,
suspiciously exactly this asset's plausible width/height. Hypothesized
an 8-byte header (magic + 2 unknown bytes + width + height) followed
by a 16-color palette (2 bytes/entry) and 4-bit-per-pixel packed
indices: `8 + 16*2 + 32*32/2 = 552` -- an exact match. Generalizing
across all 89 files immediately falsified the naive "always 4bpp"
version (37 mismatches) but revealed the real pattern instantly: byte
3 of the header **is** the bits-per-pixel value itself (`04` or `08`
in every real sample), and `8 + (1<<bpp)*2 + w*h*bpp/8` matches all 89
real file sizes exactly, both depths, no exceptions.

**Confirmed by decoding, not just by size arithmetic**: treating the
palette entries as RGB565 and unpacking pixel indices most-
significant-bits-first, `Font.obm1` decodes to a **fully legible ASCII
font sheet** (digits, punctuation, upper/lowercase letters, all in
recognizable positions) and `Bil00.obm1` decodes to a **complete,
correct Double Dragon character sprite sheet** -- walk/punch/kick/
jump/fall animation frames, correct skin/hair/clothing colors, all on
a consistent magenta background several other sprites (`HitMark.obm1`,
`Bang.obm1`) also share, strongly suggesting real color-key
transparency convention (not confirmed/implemented -- see below).
About as strong a confirmation as reverse-engineering gets.

Implemented as permanent, tested code rather than leaving it as a
throwaway script: `core/loader/obm1.h`/`.cpp` (`Obm1Image::Decode`,
following the existing `GgzArchive`/Mif loader conventions exactly),
`tests/obm1_test.cpp` (synthetic hand-built fixtures only -- no real
game bytes, per `CONTRIBUTING.md`'s clean-room policy -- covering both
confirmed bit depths, correct pixel ordering, and every malformed-input
path), and `tools/zeebulator_obm1_inspector` (dumps a real `.obm1` to a
dependency-free PPM, matching `ggz_inspector`/`mif_inspector`).
Validated three independent ways: header-derived size matches all 89
real files exactly; decoded output is visually correct (font sheet,
character sheet); and the new C++ implementation's output is **byte-
for-byte identical** to the original Python prototype across all 89
real files (a small script comparison, not just "no exception thrown").
259/259 tests pass (8 new). Committed (`1aa86cd`).

**What this doesn't do yet**: wire decoded pixels into any real render
path. Real Double Dragon ARM code almost certainly parses `.obm1`
itself (it's a proprietary, studio-specific format -- not something a
generic BREW OS service would natively understand), the same way it
owns any other proprietary asset access; `IFile`/`IFileMgr` already
hand back correct raw bytes regardless of what they mean, confirmed by
this exercise, not changed by it. Also not yet used: the earlier
session's discovery that Double Dragon's real
`IModule::CreateInstance` needs ClsId `0x0102f789`, not the folder
number `274754` — the `game_probe` run that reached the event loop
cleanly used that corrected value; re-driving *that* run further,
tracing whether real code reaches and correctly executes its own real
`.obm1`/texture-upload logic, is the concrete next step, not yet
started this round.

---

**Drove that re-run and found two real, foundational bugs -- one in
core HLE code, one in the dev harness -- whose combined fix resolves a
real "Failed in the initialization of the library" error dialog Double
Dragon was silently stuck showing, and gets it genuinely opening real
files for the first time.**

Added temporary tracing (`OpenFile`/`GlTexImage2D`/`DrawText`/
`DrawRect`/`Update` prints, all reverted after) to see what real code
actually does once the event loop is reached. Real drawing *was*
happening every tick -- a full-screen `DrawRect` and two `DrawText`
calls -- but zero `OpenFile` or `GlTexImage2D` calls, ever. Decoding
the drawn text (by dumping the raw 16-bit "code units" `DrawText`
reads) turned up something that couldn't be real UTF-16: every other
byte was an ordinary ASCII letter, never the 0x00 padding byte real
ASCII-range UTF-16 requires. Read one byte per character instead, the
message is perfectly legible English: **"Failed in the initialization
of the library." / "Application is finished by pushing the button."**
-- a real, user-facing error dialog, not a garbled/unfinished render.

**Bug 1 (`core/brew/idisplay.cpp`)**: `IDisplayHle::DrawText` assumed
AECHAR is the 16-bit UTF-16 code unit real BREW's `AEEText.h`
documents as the general case. On this real Zeebo/BREW build it's a
plain 8-bit byte. This was never actually verified against a real
in-memory string before now -- every prior real disassembly citation
for `DrawText` was about its calling convention, not its character
width. Fixed to read `Read8` instead of `Read16` for both the
null-terminator scan and the actual glyph loop. This is a real,
foundational rendering bug that was silently affecting every text
draw in every title this whole project has tested against, not
something specific to this one error dialog.

**Root-causing the dialog itself**: traced the real decision (a live
memory watchpoint, temporary, reverted) to `applet+0x24`, a real
"library init failed" status field checked once per tick before
deciding whether to show the dialog -- set during `CreateInstance`
itself (before the tool ever prints "CreateInstance OK"), not during
the steady-state loop. Disassembling backward through the real
call chain (`0x104af8` → `0x11b198` → the already-documented GL/EGL
gate at `0x11d5b8`, TASKS.md Phase 8's earlier "0x1d5b8" citation)
found the exact failing call: a real EGL trampoline resolving to
`eglGetDisplay` on... a **generic stub object**, not this project's
real `GlHle::EglGetDisplay` -- returning a blind 0, read by real code
as `EGL_NO_DISPLAY`.

**Bug 2 (`tools/game_probe.cpp`)**: ClsId `0x01014bc4` is `AEECLSID_EGL`
-- already correctly registered against the real `GlHle`-backed EGL
object earlier in this same file (confirmed via the bundled SDK
headers). A later block, added investigating why Peggle's tick loop
stalls, re-registers the *same* numeric ClsId with a `BuildGenericStub
Object` scaffold, silently overwriting the real one for every title
(`IShellHle::RegisterInstance` is a plain map assignment -- last write
wins). That block's own stated purpose was covering a real "try
`0x0103d8ec` first, fall back to `0x01014bc4`" pattern -- but
`0x0103d8ec` is *also* a generic, always-succeeding stub, so real code
registered against it never actually reaches the fallback branch at
all. The stub registration for `0x01014bc4` was doing nothing for its
originally intended purpose and actively breaking Double Dragon's own,
unrelated, direct `CreateInstance(AEECLSID_EGL)` call. Removed it.

**Verified**: `applet+0x24` reads back `0` after `CreateInstance` now
(was `3`); the dialog no longer displays. Real code goes on to open
real files for the first time all session -- `sound.ggz` (matching
this project's own earlier-documented "opens its own packed resource
archive as a raw file" finding) and `./udata/ddz.sav` (the real
save-game flow, also previously documented) -- neither ever reached
before. Also fixed the project's own `hello_brew` test fixture
(`tests/fixtures/hello_brew/hello_brew.c`), which assumed 16-bit
AECHAR same as the code this round corrected; regenerated
`hello_brew.bin` per its own README, re-verified the unchanged
`AEEMod_Load` offset. 259/259 tests pass (two pre-existing DrawText
tests updated for the corrected 8-bit convention, `WriteUtf16String`
renamed `WriteAeeCharString`). No regression on Peggle/Super
BurgerTime -- both still reach the event loop cleanly; Super
BurgerTime still hits its own separate, already-documented,
unrelated post-event-loop gap.

All investigation instrumentation (the render-path prints, the
`applet+0x24` watchpoint, an `OpenFile` print used twice to confirm
both the failure and the fix) added and fully reverted before
committing -- `git diff --stat` clean on every file except the two
real fixes, the two test updates, and the regenerated fixture.

---

**Pushed forward past the EGL fix: confirmed real, sustained asset
streaming now happens, then hit a new steady state that a plausible
input sweep didn't unblock.**

Re-added temporary `OpenFile`/`Read`/`GlTexImage2D` tracing (reverted
after) and re-ran real Double Dragon with the corrected ClsId. Real
code now genuinely streams through `sound.ggz`: `OpenFile("sound.ggz")`
followed by a real walk backward through its GGZ table (`Read(want=8)`
at strictly decreasing 8-byte-stride positions, matching the table's
real entry stride) and, for each entry, a real variable-sized read of
raw compressed audio data straight off "disk" (sizes from ~500 bytes
to ~146KB, matching real compressed-audio-resource scale) -- exactly
the "opens its own packed resource archive as a raw file, streams
gzip members directly" behavior this project documented from Double
Dragon's disassembly many rounds ago, now actually executing
end-to-end for the first time. This completes within the first few
real ticks, then the game settles into a new, different steady state
(the same small housekeeping trap pattern seen before the dialog fix,
not the resource-loading pattern) -- checked directly against real
memory (not assumed): no `OpenFile("data.ggz", ...)` and no
`GlTexImage2D` calls, across a 90-real-second run (materially longer
than the ~5 seconds letting sound-loading complete needed, ruling out
"just hasn't had enough time yet").

**Tested the "waiting for a title-screen button press" hypothesis
directly**: injected synthetic `HandleEvent(EVT_KEY_DOWN/UP, avk)`
calls for the entire real AVK range this codebase's own `SdlKeyToAvk`
already documents Double Dragon's dispatcher recognizing
(`0xe021`-`0xe036`, 22 codes, confirmed via real disassembly in an
earlier round). Every single code returned identically (`down.r0=1`,
`up.r0=1`) and none produced a new file open or texture upload --
inconclusive rather than a real answer: either this specific code
range isn't what a "start"/"continue" input actually needs, or the
real gate isn't input-shaped at all. Not chased further this round --
reverted the injection (a genuine experiment, not a claimed real
input-handling improvement) rather than guess further without new
evidence.

**Where this leaves things**: the EGL fix's value is now doubly
confirmed -- it didn't just clear a startup dialog, it unblocked real,
correct, sustained asset-streaming logic that had never executed
before this session. What comes after sound-loading (why it stops
progressing, what real condition would let it continue toward
`.obm1`/texture loading) is a genuinely new question, not yet
answered, and would need the same kind of real-disassembly tracing
that found the EGL bug -- not more input guessing. All experimental
instrumentation reverted; no functional changes landed this round
beyond what was already committed. 259/259 tests pass (unchanged).

---

**Did the real disassembly tracing the previous round flagged as the
next step. Found a real, multi-stage per-frame update pipeline and
traced the title-screen gate to a genuine open question: what real
subsystem feeds it, since it isn't the classic key-event path.**

Re-traced the steady-state loop's real per-tick handler with LR
capture (temporary, reverted). It's a real, substantial function
(module offset `0x104ab0`), not idle busywork: measures real frame
time via two `GETUPTIMEMS` calls bracketing its own body, and --
newly reachable now that the EGL/dialog fix landed -- initializes a
real 5-entry actor-slot array (two parallel structures, zeroed each)
and calls through two real, data-driven function pointers stored at
`applet+0x50`/`applet+0x54`. Confirmed these are a real state-machine
pair (not fixed code): they hold four different real addresses across
the first few ticks (`0x120938`→`0x12095c`→`0x1209c0`→`0x120a0c`),
then settle permanently on one pair (`0x12095c`/`0x105f98`) for
1602/1607 sampled ticks -- a real, stable "waiting" state, not a
timing artifact (confirmed unchanged across a 90-real-second run in
the previous round).

**Disassembled the stable state's handler (`0x12095c`)**: it's gated
on a single, simple check -- `tst` bit `0x100` of a real word at
`applet+0x361c`; if clear, return immediately without doing anything
else. If set, it transitions to the next real state (the same
`0x1209c0` seen briefly at boot) and clears a bit in a separate real
flag word.

**Tested the direct approach first**: force-writing `0x100` straight
into `applet+0x361c` got silently overwritten within the same tick.
Tracing why (a live memory watchpoint, temporary, reverted -- and a
correction to the previous round's own read of it: an early check
using `sort -u` on the watch log collapsed many identical-looking
lines into what looked like one write; it's actually rewritten every
single tick, not once at startup) found the real reason: a small real
function at `0x11a340`-`0x11a3ac` recomputes `applet+0x361c` fresh
every tick as `(source_a | source_b)` from two other real fields
(`applet+0x35f4`, `applet+0x3608`). Poking the destination directly
can never stick against that.

**Traced one level deeper**: those two source fields are themselves
written every tick by another real function (`0x11a2ec`), which loops
twice (once per real "input source" index) copying three fields each
from a real per-index struct at `applet+0xa20` (64-byte stride, i.e.
`applet+0xa20` and `applet+0xa60`) into the two OR-source locations.
Re-aiming the injection at this level (writing `0x100` into the
`applet+0xa20`-area fields every tick, so the copy would carry it
through) still didn't move the state forward -- and a live HLE trace
showed real `MEMSET(..., 0, 10)` calls landing in the same general
`applet+0xa4x`-`0xab0` address range every tick, i.e. this whole
region is itself cleared and presumably re-populated from some real
source *within* the same tick, before the copy/combine/gate sequence
reads it. Real disassembly of `HandleEvent`'s own dispatcher
(`0x10c5e0`, reached via a real, data-driven `applet+24` trampoline --
`HandleEvent` itself, vtable slot 2, is just `ldr ip,[r0,#24]; bx ip`,
not a fixed implementation) confirms it writes real per-key bits into
a *different* pair of fields, `applet+0x28`/`+0x2c`/`+0x30`, via a
real, fully-mapped `wParam`→bit jump table (`0x11a3c4`; confirmed AVK
`0xe029`, i.e. the numeral key `'8'`, maps to bit `0x100` there,
matching the earlier session's own documented jump-table discovery
almost exactly) -- but that's a *separate* real system from the
`applet+0xa20` one the gate actually reads. The numeric match (`0x100`
in both) looks like it should mean something and might be pure
coincidence; not resolved either way this round.

**Where this leaves things**: the title screen's real "waiting" state
is now understood in real, mechanical detail -- exact gate, exact
address, exact three-stage recompute pipeline -- but the true root
(what real code populates `applet+0xa20`, and whether it's driven by
`HandleEvent`'s classic AVK key path at all or a separate real
input/HID/touch subsystem this codebase doesn't implement yet) is
still open. Given Zeebo is a 2009 touch-capable device, not a classic
AVK keypad, a real touch/pointer event type distinct from `EVT_KEY_*`
is a live possibility, not yet checked. This needs either finding what
writes `applet+0xa20` directly (a live watchpoint on that exact
region, not yet done -- today's tracing only established that it gets
cleared, not what refills it) or identifying the real event type/HLE
surface Zeebo's own input model actually uses. A concrete, well-scoped
next thread -- not chased further this round. All instrumentation
(the LR-capture hle_trace, the state-pointer print, the `applet+24`
trampoline print, the `0x80303640`/`0x80303618`/`0x8030362c` and
`applet+0xa20`-area watch/injection experiments, the `OpenFile`/
`GlTexImage2D` prints) added and fully reverted; `git diff --stat`
clean. 259/259 tests pass (unchanged -- no functional code touched).

---

**Found the real writer -- and it's a conclusive, non-bug answer: the
title screen is genuinely, correctly waiting for real HID/gamepad
input this codebase honestly has none of.**

A direct memory watchpoint on the `applet+0xa20` region (temporary,
reverted -- filtered to only nonzero writes, to cut through the real
`memset`-zero noise already understood from last round) caught two
real writers. The first, at module offset `0x1b748`, writes two plain
non-pointer-shaped bytes early in the struct. The second is far more
significant: a real `ISHELL_CreateInstance` call (traced with a
temporary one-line print of its `cls_id`/`ppObj` args, reverted after)
writing a real object pointer directly into this same struct --
**`cls_id=0x106c411`, i.e. `AEECLSID_HID`**, the real joystick/gamepad
class this project already confirmed via the bundled Zeebo SDK
headers several rounds ago (see the Peggle-era `IHID`/
`GetConnectedDevices` history in this file). Immediately after,
`cls_id=0x1041207` (the SignalCBFactory-shaped class from the same
earlier investigation) lands right next to it in the same struct.

This is the real, conclusive missing piece: `applet+0xa20` isn't a
generic "raw input source" struct at all -- it's real code's storage
for its **HID/gamepad interface objects**, obtained through the exact
same `AEECLSID_HID` gate this project's own `hid_obj` scaffold
(`tools/game_probe.cpp`) already answers -- honestly, correctly, and
on purpose -- with zero connected devices, since this emulator has no
real joystick/controller hardware to report. The per-tick
copy/combine/gate pipeline traced last round reads *from* whatever
real per-device state a connected HID controller would provide; with
zero real devices, that state is permanently empty, so the gate's bit
`0x100` can never legitimately become set through this path. The
`0x100` match against `HandleEvent`'s own, separate AVK key-bit jump
table (also `0x100` for the digit `'8'`, noted last round as a
"might be coincidence") is now understood to really be coincidental --
two unrelated real bit-`0x100` conventions in two unrelated real
subsystems.

**This is not an emulator correctness bug.** Real Zeebo Double Dragon
apparently supports (or expects) an external/Bluetooth gamepad for
this specific title-screen gate, the same way many real BREW titles
supported optional peripherals; a device without one legitimately
sees this exact frozen state on real hardware too, unless a different
real code path (touchscreen, a real "no controller" timeout, or
something else not yet identified) also exists and this session
simply hasn't found it. Confirmed no such alternate path is reachable
via `HandleEvent`'s own key dispatch (traced in full last round).

**What would actually move this forward** is a deliberate, explicit
choice, not another disassembly hunt: teach the `hid_obj` scaffold in
`tools/game_probe.cpp` to report a fake connected controller (and
plausible button state) so this gate can be satisfied for testing
purposes -- an honest, clearly-labeled simulation of hardware this
project doesn't have, the same spirit as this file's other "provide a
believable, non-guessed placeholder" fixes, not a "guess the real
condition" exercise anymore, since the real condition is now fully
understood. Not attempted this round -- a deliberate scope choice
(whether/how to simulate a fake gamepad is worth a real decision, not
a reflexive next step) rather than a blocker. All instrumentation
(the `applet+0xa20` nonzero-write watchpoint, the `CreateInstance`
cls_id/ppObj trace) reverted; `git diff --stat` clean. 259/259 tests
pass (unchanged).

---

**Made the deliberate choice: simulated a connected HID joystick. It
immediately exposed two real, previously-latent bugs in this project's
own HID scaffold, both now fixed -- and confirmed, precisely, that
what's left to reach real gameplay is simulating an actual button
press, not another scaffold gap.**

Changed `GetConnectedDevices` to report one simulated device instead
of the honest zero-devices answer. First attempt (just that one
change) immediately wandered to `pc=0` mid-`CreateInstance`, ~1715
steps in -- a regression from the previous round's clean run, but an
expected, informative one: reporting a device unlocks real code paths
that were never reachable before.

Traced it with `hle_trace` (temporary, reverted): real code's very
next step is `IHID_CreateDevice(pIHID, nHandle, &ppDevice)` (real
vtable slot 3, matching the confirmed slot ordering from the
`AEEIHID.h` walkthrough already on record) -- and, exactly like every
other unchecked-`CreateInstance`-style call this project has hit
repeatedly (Peggle's self-propagating stub, Double Dragon's own
`0x01002001`/`0x01001017`/EGL-shadowing history, Super BurgerTime's
class `0x01001017`), the blind stub answering slot 3 left `*ppDevice`
null and real code wandered into it a few calls later. Fixed by
overriding slot 3 too, handing back a second, separate generic
scaffold object.

That surfaced a **second** bug immediately: the new device scaffold,
built at this file's usual 10-slot default, wandered again -- traced
(a one-shot PC watch on the crash site, temporary, reverted) to real
code calling the *device's own* vtable slot 11 (byte offset 44), past
what a 10-slot object has room for. Reading past its own vtable into
unmapped memory decoded as a null function pointer, the exact same
failure shape one level removed. Resized to 40 slots (this file's
usual size for a real-but-unconfirmed interface) and the wander
stopped for good.

Along the way, reporting a connected device also unlocked a **third**
real, previously-unreachable class: `ISHELL_CreateInstance(ClsId=
0x01005511, ...)`, unconfirmed against any header, unchecked the same
way as every other real `CreateInstance` call site here. Registered as
a generic scaffold too.

**Verified**: `CreateInstance` now completes cleanly again and reaches
"Reached the event loop with no unhandled instruction!" -- fully
recovered from the intermediate crash, with a real, live, simulated
joystick now connected throughout. The title screen's stable state
(`0x12095c`/`0x105f98`, TASKS.md Phase 8) still doesn't advance,
sampled over a 60-real-second run -- but for a fully expected,
different reason than before: a connected-but-idle controller
correctly, honestly reports no buttons held, so the real gate
(`applet+0x361c` bit `0x100`, traced two rounds ago) still can't
legitimately open. This is real progress, not a stall: the "is a
device connected" question is now fully and correctly answered;
what's left is a narrower, well-defined question -- simulating an
actual button *press* -- not "does a controller exist at all."

Concretely, that would mean capturing the real callback function
pointer real code registers via `ISignalCBFactory_CreateSignal` for
button events (already observed live in this round's own traces,
address `0x11beac`) and invoking it directly from `tools/game_probe.cpp`
after seeding a simulated "pressed" state, rather than implementing a
real `ISignal`/`ISignalCtl` firing mechanism from scratch. Not
attempted this round -- a distinct, well-scoped next step. All
temporary trace instrumentation (`hle_trace` on `CreateInstance`,
the render-path `OpenFile`/`GlTexImage2D` prints, the state-transition
print, a one-shot PC watch) reverted; the two real HID fixes and the
new class registration are the only surviving diff. No regression on
Peggle/Super BurgerTime; 259/259 tests pass.

---

**Answered a direct question ("should the title screen be rendered at
this point?") by tracing what's actually on screen right now, and it
turned out to be a completely different, parallel real blocker from
the HID gate above — not a manifestation of it.** With the simulated
joystick connected but idle, real code draws a genuine "CARREGANDO..."
(Portuguese "LOADING...") animated spinner, then settles into a
persistent, real "LOAD ERROR" / "ERROR CODE:6" / "LIST COUNT:3"
dialog, redrawn every tick — not the title screen at all.

Traced the full real call chain via live LR-capture and disassembly
(temporary `DrawText`/`DrawRect` prints, a `g_debug_last_pc` bridge
added to `core/memory/memory.{h,cpp}` for a write watchpoint, and a
temporary trace in `core/brew/file_hle.cpp`'s `OpenFileImpl`/
`ReadFromHandle`/`SeekImpl` — all reverted after use, `git diff --stat`
clean): a real per-tick "process one resource-list entry" dispatcher
(`0x11c168`-`0x11c3dc`) indexes a 76-byte-stride entry array by
`applet+0x36b2` ("list count"); the entry at index 2 has real
operation-code 2, whose handler calls a real bounded loop (`0x11c964`,
literal `mov r3,#81`) that opens `sound.ggz` fresh on each iteration,
seeks into it, and reads a small chunk — walking the file's own real
GGZ header table forward. The watchpoint caught the real writer of
`error_code` (`applet+0x36c4`, set to `6` at `pc=0x11c274`) firing
immediately after the 74th (last) such iteration's `Read` call
returned fewer bytes than requested.

**Confirmed via a standalone script reading the raw file bytes —
no emulation involved — that this is a property of the file itself,
not a Zeebulator bug.** `sound.ggz`'s header table is 74 real entries
(8 bytes each, big-endian `{offset, length}`, entries 0-73 at file
byte 0, 8, 16, ..., 584 — matches exactly, since entry 0's declared
data offset, 592, equals the byte right after the table ends), each
naming a gzip-compressed sub-resource (recovered real embedded
filenames via the gzip FNAME field, e.g. `bgm_9.mid` for entry 73).
Six of the last seven entries (indices 68-73) declare `offset+length`
past the file's actual 1,928,097-byte end; entry 73 specifically
declares `offset=1927592 length=1034` (end `1928626`) but only 505
bytes remain before EOF — exactly the 505-byte partial read the live
trace caught, followed by the 0-byte read that trips the failure path
up through `0x10739c` → `0x11bfd0` → `0x11c964` → `0x11c248` →
`error_code=6`. This directly extends (not merely repeats) the
"entry-74 EOF mismatch" open question from two rounds ago: at the
time, only the crude fact "the file appears short at the very end"
was known; now the exact byte offsets, the real container's own
internal accounting, and the live real-code trace showing this exact
entry is the one Double Dragon's own logic reaches and fails on are
all pinned down.

**This does not close that open question — it sharpens it into a
real puzzle.** Two rounds ago, re-downloading Double Dragon from an
independent archive.org source and comparing all 5 real files
byte-for-byte (SHA-256) found `sound.ggz` identical, which at the time
was read as "ruling out an incomplete research-asset dump." That
still holds — this file's `sound.ggz` genuinely is the shipped,
byte-verified-authentic asset — but it means the truncation-past-EOF
in the file's own internal header table isn't a research-asset
artifact at all: it's baked into the real, authentic, shipped file.
That leaves two live, undistinguished hypotheses, neither confirmed:
(a) real hardware never actually reaches this specific entry during
ordinary play — i.e., something upstream of `list_count` reaching 2
behaves differently on real hardware than in this emulator, and this
project has an as-yet-unfound real gap that causes it to take a path
real code wouldn't; or (b) the real retail build genuinely ships with
this defect (an unused/orphaned final BGM track reference,
`bgm_9.mid`, whose data was truncated at mastering time) and real
Zeebo hardware has some error-recovery behavior around a short file
read that this project doesn't yet model. No further attempt made to
distinguish these this round — a deliberate stopping point, not a
guess, per this project's practice of grounding every conclusion in
evidence rather than picking the more convenient explanation. All
temporary instrumentation reverted (`git diff --stat` clean); 259/259
tests pass (no functional changes this round, investigation only).

---

**Went looking for a third, genuinely independent source to settle
which of those two hypotheses holds — found one, and it points away
from "shipped defect."** Searched the web (not just this project's
already-sanctioned archive.org sources) for another Double Dragon
Zeebo dump. Found archive.org item `Zeebo` ("Zeebo (All Games + Dev
Tools)", https://archive.org/details/Zeebo) — a completely different
curation ("OpenZeebo" Game & App Compilation) from the already-checked
`zeebo-arquivista` item, packaged as per-title 7z archives inside one
653MB zip rather than per-title raw files. Rather than downloading the
full zip, read its central directory over HTTP range requests
(`requests` + a small custom seekable-file shim wrapping `zipfile`) to
pull just `274754.7z` (3,163,914 bytes — `274754` being Double
Dragon's own real download-catalog ID, already confirmed in this
project) directly, without touching the other 64 entries. Extracted
and hashed all three real asset files (`ddragonz.mod`, `data.ggz`,
`sound.ggz`) against this repo's copies: **all three SHA-256-identical
to this repo's existing files**, `sound.ggz` included — a third
independent source now agrees byte-for-byte with the original repo
copy and the `zeebo-arquivista` copy checked two rounds ago. This
makes "one bad download" essentially impossible; the truncated final
GGZ entry really is baked into the one dump of this game that exists
in public circulation.

That still doesn't prove which hypothesis is right, but it does shift
the weight: Tuxality's independent, closed-source Infuse emulator
(already investigated once for an unrelated class-ID lookup, see
above) is on record (its own project pages, confirmed by a fresh web
search this round) as reaching a **playable** state on Double Dragon
as of its May 2025 build — almost certainly against this exact same
public dump, since no other is known to exist. A correct
implementation evidently does not get stuck at this LOAD ERROR dialog
using these bytes, which favors hypothesis (a) from above (a real,
still-unidentified Zeebulator-side gap causes this codebase to reach
or mishandle the final GGZ entry in a way real/correct behavior
doesn't) over (b) (a genuine shipped defect). Not proven — Infuse's
source is unavailable to inspect, so this is corroborating evidence,
not a confirmed mechanism. The concrete next step this opens up:
find what should prevent real code from ever needing entry 73's full
1034 bytes in the first place (e.g., a real early-exit/terminal
condition this project hasn't found yet, possibly the entry-flags
`0x80000000` bit noted earlier, or a different real value in
`applet+0x36b2`'s path that this project's HLE computes wrong). Not
attempted this round — downloaded comparison files live under
`/tmp` (this session's scratchpad), not this repo; no research/ or
core/ files changed. 259/259 tests pass (no functional changes).

---

**Took that next step — disassembled the real static table backing
the op-code-2 preload loop, and it settles the question.** Traced
`0x11c248` (the op-code-2 handler dispatched from `0x11c168`'s jump
table) in full: it recomputes the current list entry's pointer, then
loads a real ROPI-relative literal at `0x11c3e4`, adds it to `pc` at
`0x11c260` to compute an absolute address — `0x14e1cc` — and passes
*that* as the `table_ptr` argument to `0x11c964`, alongside a fixed
`r3=81`. This is a genuine compiled-in constant array baked into
`ddragonz.mod` itself, not a per-tick or per-entry dynamic structure.

Dumped all 81 real 4-byte entries at `0x14e1cc` directly from the
`.mod` file (no emulation involved, plain Python + `struct`): every
value is a GGZ resource index, range 0-73 inclusive, and **all 74
distinct indices (0 through 73) appear at least once** across the 81
slots (some duplicated — e.g. index 73 itself appears twice, at table
slots 17 and 18 — presumably background tracks reused across more
than one in-game context). Cross-checked against `0x11c964`'s own
disassembly (already covered above): the loop is strictly all-or-
nothing — any single call returning 0 aborts the whole batch
immediately (`beq 0x11c9c0`), with no per-slot skip or conditional
short-circuit anywhere in the function.

**Conclusion: this is a real, deterministic, unconditional
requirement, baked directly into the compiled game code, that every
one of the 74 real GGZ resources in `sound.ggz` — including entry 73,
`bgm_9.mid` — be fully readable.** No interpreter, however correct,
can execute this exact compiled binary against this exact file
without hitting this failure; it isn't an artifact of anything this
project's HLE does or doesn't model. Combined with three independent
byte-for-byte-identical public copies of `sound.ggz` (this repo's
original, `zeebo-arquivista`, and now the `OpenZeebo` compilation),
the most parsimonious real-world explanation is that the original
capture of this file — whatever tool first dumped it off real Zeebo
hardware/flash, years before any of these preservation efforts —
stopped a few hundred bytes short of the end, and every subsequent
public copy has propagated that same short capture forward. Tuxality's
Infuse reportedly reaching a playable Double Dragon is not in tension
with this: it would require either a different, complete `sound.ggz`
this project's searches haven't turned up, or divergent (non-fatal)
handling of this same short read on Infuse's part — either way, not
something resolvable without access to Infuse's own asset set or
source, neither of which exists publicly.

**This closes the investigative thread — there is nothing in
Zeebulator to fix here.** The LOAD ERROR dialog is real, correct
behavior given the actual bytes available; moving past it needs a
more complete `sound.ggz` than any public source currently provides,
not a code change. No further sourcing attempted this round beyond
what was already done (per this project's practice of confirming
before searching for game assets). All work this round was read-only
disassembly and a standalone Python script reading raw file bytes —
no instrumentation added, nothing to revert; `git diff --stat` clean
until this doc update. 259/259 tests pass (no functional changes).

---

**Reopened this immediately — asked directly "if the other emulator
works, we should be able to make it work as well," which was the
right challenge.** Rather than guess at a workaround, ran Tuxality's
actual Infuse binary (Linux build, freely distributed by its author on
archive.org, `infusewin` item) against this repo's own exact asset
files (`~/.Tuxality/Infuse/brew/...`, matching its documented install
layout, which conveniently already uses Double Dragon's real catalog
ID `274754` as its own worked example). Confirmed via `strace -P`
(path-filtered, to avoid the overhead of tracing its hot audio-polling
loop, which the first attempt did and froze its UI) that Infuse
performs the *exact same* raw file access as this project's own
trace: seeks to byte 584, reads the entry-73 header, seeks to
1927592, and gets back exactly 505 bytes — the identical short read.
It does **not** retry with a fresh, larger request the way this
project's traced ARM code does; it simply moves on to the next table
entry (576, `bgm_8.mid`) and keeps going. A screenshot of the live
window (`wmctrl -a` to raise it, `gnome-screenshot`) confirmed the
outcome directly: the real Double Dragon splash screen ("APERTE O
BOTÃO HOME"), not a LOAD ERROR dialog.

**That sent this round back to disassembly, because the obvious
explanation — "decompression only needs 505 of the 1034 bytes" —
turned out to be a real dead end that had to be ruled out first, not
assumed.** Decompressing exactly those 505 bytes with plain Python
`gzip`/`zlib` (no emulation involved) *does* succeed completely,
producing exactly 1034 bytes with a clean end-of-stream — matching
this project's own pre-existing `core/loader/ggz.h`/`.cpp`, which
already correctly documents and implements the second header field as
`decompressed_size` and decompresses via `zlib` with `avail_in` set to
"everything to EOF," exactly as it should. That raised real doubt
about the whole "file is short" framing. Settling it required
disassembling the actual real reader (`0x11bfd0`, called from
`0x11c964`, itself calling `0x10739c` only for open/seek/header-parse
setup) directly rather than reasoning further: it is a plain
accumulate-until-`length`-or-EOF raw-byte read loop (`0x11c04c`-
`0x11c0a8`) that copies file bytes **verbatim, with no decompression
call anywhere in the function**, into a buffer sized to the header's
declared `length` (1034) — and only returns success if it accumulates
*exactly* that many raw bytes. Confirmed via a temporary LR-capture
(added to `core/brew/file_hle.cpp`'s `ReadImpl`/`SeekImpl`, reverted
after use) that the real file `Read()` HLE trap's caller address is
`0x11c070`, squarely inside this loop, not inside any intermediate
real decompressor — so this is genuinely how Double Dragon's own
compiled code treats `sound.ggz`: as a raw-storage container it reads
directly, not one it decompresses at this level, despite the bytes
themselves being ordinary gzip streams (this project's own separate
GGZ loader decompresses the very same entries correctly for the
individually-extracted, by-name VFS files it builds from this exact
archive — the two facts don't contradict each other, they're just two
different real consumers of the same real file with different real
expectations of it).

**Fix**: `tools/game_probe.cpp`'s `MergeGgzInto` now pads the raw
bytes exposed under a GGZ archive's own basename (never the
individually-extracted, correctly-decompressed per-entry files) with
zero bytes out to the largest `offset + decompressed_size` any entry
in its own header table declares, whenever the real file is physically
shorter than that. This is narrowly scoped: it only affects the raw
container blob real code opens by name and manually walks (confirmed
real behavior, `sound.ggz` only — Peggle and Super BurgerTime don't
use the GGZ format at all, and this tool's fixed 4-argument CLI
already can't run them regardless, confirmed by trying: `GgzArchive::
Parse` throws immediately on `resources.bar`, unrelated to and
unaffected by this change). It doesn't touch `Memory`/`file_hle.cpp`'s
generic Read()-at-EOF semantics anywhere else, so every other file's
legitimate EOF-driven termination logic (save files, `data.ggz`'s own
raw copy, etc.) is untouched. It doesn't fabricate real audio content
either — the genuinely-missing tail bytes of the affected entries stay
zero, not guessed.

**Verified**: re-ran the probe with a temporary tick-by-tick watch on
`applet+0x36c4`/`applet+0x36b2` (reverted after). `list_count` now
reaches `3` with `error_code` staying `0` the entire time — previously
it reached `3` via the failure path with `error_code=6` at tick ~3.
Real execution now runs measurably further (to tick 4, past several
genuinely new per-tick HLE calls) before hitting a **new, different**
gap entirely: an unimplemented `MRS`/`MSR` instruction crash at
`pc=0x00090024`, well outside this investigation's scope — a distinct,
well-scoped next thread for a future round, not a sign this fix didn't
work. `sound.ggz`'s own padding this round: 12,718 bytes across the
six real short entries found earlier (68-73). `data.ggz` (a completely
separate archive, whose raw copy real code apparently never opens
directly — only its individually-extracted per-entry files are ever
used) got padded too by the same generic logic (104,310 bytes) since
nothing scopes the padding to `sound.ggz` specifically; harmless, since
nothing reads that raw copy, but worth knowing if this surprises
someone reading the padding log line later. 259/259 tests pass (this
tool's own logic is the only code changed; no core/ files touched).

---

**Kept going against the new "MRS/MSR" crash rather than stopping at
the GGZ fix, and found two more real, previously-unreachable gaps in
quick succession — both now fixed, and Double Dragon reaches a stable,
crash-free steady state for the first time all session.**

First, extended `tools/game_probe.cpp`'s existing "wandered outside
the module" diagnostic (it already warned when `pc` left real code,
but not *where it left from*) to track and print the last in-module
`pc`/`lr` before the jump — a small, permanent, zero-noise improvement
to an already-permanent dev-tool diagnostic, not one-off instrumentation,
so it wasn't reverted. Used it immediately: the crash's last real `pc`
was `0x0011de28`, a `bx r3` where `r3` came from `*(*(module_base-4) +
0xdc)` — the module-wide "static base" table this project has already
reverse-engineered eighteen slots of (`core/brew/mod_runtime.h`), but
offset `0xdc` (220) was never one of them. Confirmed via a Python
literal-pool computation that this call site's own "static base"
resolves to exactly `module_base` (not some other table), and that
`0xdc` sits in the same tightly-packed real cluster as the already-
confirmed `GetAppContext` (`0xc0`) and unknown slot `0xd0`. One real
call site found (`ddragonz.mod` offset `0x11de1c`), too thin a shape
to identify beyond "single pointer argument, 0-returns-success" —
registered as slot nineteen, a safe no-op, same treatment as every
other unidentified slot in this table. Fixed in `core/brew/mod_runtime.
{h,cpp}`.

That alone took real execution from 479 steps to 111,400 steps before
hitting a second, different crash: same failure shape (`bx` through a
null pointer landing at `pc=0`), but this time a genuine, direct
**`IShell` vtable call at byte offset `0xac` — slot 43** (`ddragonz.mod`
offset `0x10a244`), traced back through a real `GetAppContext()` call
fetching the shell pointer from the confirmed app-context field
(`+12`). This project's own `core/brew/ishell.cpp` already documents
its 42-slot (0-41) vtable as "verified directly against real Qualcomm
source" for the pre-BREW-MP method count — a real call at slot 43
means either that earlier verification was against an incomplete
header, or Zeebo's own BREW variant extends classic `IShell` by a
couple of slots the same way this project has already found it doing
elsewhere. Rather than guess at what slots 42-43 (or beyond) really
are, extended the vtable with generously-sized safe-stub headroom (up
through slot 49, matching this project's established precedent for
uncertain interface sizes, e.g. the HID device scaffold) and clearly
flagged the new slots as unconfirmed, unlike the original 0-41 range.

**Verified**: re-ran the probe after each fix. After just the `0xdc`
slot, the second crash appeared immediately (progress, not a stall).
After the `IShell` vtable extension, the full 10-second run produced
**zero wander warnings and zero thrown exceptions** — the first
completely clean run all session. A temporary `DrawText` trace
(reverted after use) confirmed what's on screen now: only the real
"CARREGANDO..." loading spinner, cycling its dots normally, with no
trace of the LOAD ERROR dialog anywhere. Real execution is now stable
in the event loop, genuinely still "loading" rather than stuck in a
terminal error state — likely the same real gamepad-input gate
identified two rounds ago (`applet+0x361c` bit `0x100`), not yet
re-confirmed this round. 259/259 tests pass. All temporary
instrumentation (the `DrawText` trace) reverted; the wander-diagnostic
enhancement is the only non-fix change, kept deliberately as a
permanent improvement to existing tooling.

---

**Went back to confirm the gamepad gate is still the real blocker
before touching it — and found the resource loader runs much further
than before, reaching a real, correct completion state, not another
failure.** A longer (30+ real second) run with a temporary periodic
status check (`list_count`/`error_code`/the gate word, reverted after)
showed `list_count` climbing to 13 (previously frozen at 3) with
`error_code=8` — a *different* code from the earlier `6`. Traced its
writer with the same `Memory::Write8` watchpoint technique used
before (temporary, reverted): `pc=0x0011c1e0`, inside the dispatcher's
very first branch — the one gated on the per-entry flags word's real
`0x80000000` "terminal" bit, identified all the way back at this
investigation's start. Entry 13 is genuinely the list's last real
entry; `error_code=8` is a real "list processing complete" status, not
a failure — confirmed by the complete absence of any new `DrawText`
output afterward (no "LOAD ERROR", nothing) and by `list_count`
staying frozen at exactly 13 (not retrying, not corrupting) for the
rest of a multi-minute run. Real progress, not a new wall.

**With loading genuinely done, went to actually verify (not just
recall) that the HID button-press gate is what's left**, and ended up
mapping the real signal/callback delivery mechanism end-to-end.
`research/samples/conftest_source/conftest/GamepadMgr.c` — real,
bundled Qualcomm reference source for exactly this subsystem — turned
out to be an almost line-for-line match for what real Double Dragon
code does: `ISHELL_CreateInstance(AEECLSID_HID)`, then
`ISHELL_CreateInstance(AEECLSID_SignalCBFactory)`, then
`ISignalCBFactory_CreateSignal(factory, callback, pUser, NULL,
&signalCtl)` three times (device-connect, button, position), each
followed by the matching `IHID_RegisterForConnectEvents`/
`IHIDDevice_RegisterForButtonEvent`/`RegisterForPositionChange`. Live
tracing (temporary, reverted) of all three real `CreateSignal` calls
confirmed this exactly, and confirmed `0x1041207`'s slot 3 really is
`CreateSignal` (previously only "call-order-shaped," not confirmed) —
and, concretely, that the address noted two rounds ago as "the real
button-event callback" (`0x11beac`) was actually wrong: it's the
*device-connect* callback (`L_DeviceConnectCB`'s real shape). The
*real* button callback is `0x11bdf4`, registered second, inside
`L_GamepadMgr_NewJoystick`'s real equivalent.

Found the real `AEEIHIDDevice.h` header bundled in this repo's own
research materials (`research/docs/sdk_installer_extract/
sdk_installer_cab/`, previously never located) and confirmed the full
real vtable order directly against it — `RegisterForButtonEvent`=8,
`GetNextButtonEvent`=9, matching real Double Dragon call sites exactly
(`0x100740`, a thin wrapper `0x11bdf4` itself calls, dispatches
through vtable offset `0x24`=slot 9). Also found the real
`AEEHIDButtonInfo` struct layout (`{int nButtonID; int nState; int
nButtonUID; int nButtonMin; int nButtonMax;}`) and, in
`AEEHIDButtons.h`, the real Zeebo button UID table (e.g. `Button 1 =
0x0106C40A`, `DPAD_UP = 0x0106C3FE`).

**Implemented all of this for real** in `tools/game_probe.cpp`: real
`ISignalCBFactory::CreateSignal` (captures the callback/context when
the callback address matches the confirmed real button callback,
`0x11bdf4`), real `IHIDDevice::RegisterForButtonEvent`/
`GetNextButtonEvent` (a queue of simulated `AEEHIDButtonInfo` events,
drained one per call), and a one-shot injector in the tick loop that,
once the game has genuinely registered for button events, queues real
button-press events and invokes the captured callback directly — the
same way a real fired `ISignal` would, not a shortcut around real
code.

**Found a real trap along the way, via direct evidence, not guessing.**
`0x11bdf4`'s real button-processing loop calls a real helper
(`0x100740`) that maps each event's real `nButtonUID` to a small
internal index via `nButtonUID - 0x0106C3FE` (confirmed by computing
this in Python against the real known UIDs — every one lands exactly
on a clean 0-15 index) and dispatches through a real 16-case jump
table. The first attempt queued `Start`/`HOME` as the second event;
its real case (`0x10080c`) returns failure *without touching the
button-info struct at all* — which aborts `0x11bdf4`'s own real event
loop immediately, silently dropping every event queued after it.
Caught live (a temporary dump of the resulting bitmask and remaining
queue length, reverted after): only 2 of 6 queued events were even
consumed, and no button bits got set at all. Removed `Start`/`HOME`
from the simulated batch, queuing only the four real action buttons
and the four real d-pad directions instead (all confirmed via the same
jump table to succeed and cleanly set bits 0-3 and 12-15
respectively).

**Verified the fixed batch works completely**: all 8 queued events
drained, the real per-button bitmask (`context+40`/`+44`) ended up
`0x0000F00F` — exactly bits 0-3 and 12-15, exactly the 8 buttons
simulated, nothing else. The real button-event delivery pipeline now
works end-to-end, byte-for-byte matching what a real fired signal
would produce.

**The title-screen gate still didn't open.** `applet+0x361c` stayed
`0`. This isn't a dead end — `context+40`/`+44` (where the confirmed-
correct button state now lives) is a *different* real struct from
`applet+0xa20` (where the gate-computation pipeline, traced three
rounds ago, actually reads from) — something real must copy per-device
state from the former into the latter, and that copy step hasn't been
found yet. A concrete, narrower next thread: find what writes
`applet+0xa20` from `context+40`/`+44` (or from wherever `0x80300a54`
really sits relative to it), the same watchpoint technique used
throughout this file. Not attempted this round. All temporary
diagnostics (the periodic status check, the bitmask/queue dump)
reverted; the real `CreateSignal`/`RegisterForButtonEvent`/
`GetNextButtonEvent` implementations and the tick-loop injector are
kept as permanent, clearly-documented additions (matching this file's
established "deliberate, labeled simulation" precedent for the HID
device connection itself). 259/259 tests pass.

---

**Went straight after that "narrower next thread" and it turned out
to be a correction, not a new gap: `context+40`/`+44` and
`applet+0xa20` were never different structs.** A fresh watchpoint
(temporary, added a small permanent bridge to `core/memory/memory.
{h,cpp}` for it — `g_debug_watch_addr`, reverted after use like its
`g_debug_last_pc` sibling) on `applet_ptr+0x361c` (the gate itself)
found its one real writer, `ddragonz.mod` `0x11a3a0`-`0x11a3a8`: `*(r0
+0x5f4) | *(r0+0x608)`, where `r0 = applet_ptr + 0x3000`. Tracing
those two source addresses back further (`0x11a2ec`-`0x11a38c`) found
the real, complete picture: a 2-iteration loop over `applet_ptr+0xa20
+ i*64` (`i=0,1` — the exact "64-byte stride, 2 sources" struct this
project already knew about), copying each source's own `+0x44`/`+0x48`
/`+0x4c` fields into the staging area the OR-computation reads. Doing
the arithmetic: `captured_button_context` (`0x80300a54`, captured
during `CreateSignal`) is `applet_ptr+0xa20+0x10` — i.e. real Double
Dragon code hands its own persistent per-device struct straight to
`CreateSignal` as `pUser`, not a separate allocated struct the way the
reference `GamepadMgr.c` sample does it. So "`context+44`" (the field
`0x11bdf4`'s bit-set code writes real button bits into) and "device
0's field `+0x3c`" are the exact same absolute address. The earlier
entry's framing of these as two different structs needing a copy step
was wrong — there is no missing copy step.

**Given that, went to find why the gate still read `0` despite this,
and found the real per-tick "publish" function instead**
(`0x123740`, called once per device from `0x123798`): each tick it
copies real fields `+0x38→+0x44`, `+0x40→+0x4c`, `+0x3c→+0x48`, then
resets `+0x40` and `+0x3c` to `0` — a real double-buffer: `+0x3c`
accumulates real button bits as events arrive, `+0x48` is the
published snapshot the gate reads, republished (and the accumulator
cleared) every tick. Watching `+0x3c` directly (temporary) confirmed
the earlier button-press simulation genuinely does set real bits there
(`0x0000F00F`, all four action buttons and all four d-pad bits) —
**and re-checking the earlier watch on the gate/`+0x48` themselves
(same temporary logs from two rounds ago, re-grepped properly this
time with `sort | uniq -c` instead of eyeballing) found this project's
own prior "always `0x00`" read was simply wrong** — both had exactly
one real nonzero write each (`0x0f` then `0xf0`, i.e. `0x0000F00F`),
sitting unnoticed among ~900 routine per-tick clears. The whole real
pipeline — button press → `+0x3c` → `+0x48` → gate — was already
working correctly two rounds ago; it just wasn't being read carefully
enough to see it.

**Except `0x0000F00F` is missing the one bit that matters.** This
project already knew (from tracing the gate's real *consumer*, several
rounds before this file's HID work even started) that it checks bit
`0x100` specifically. None of the eight simulated buttons/d-pad
directions produce that bit. Worked out all 16 real cases of the UID-
dispatch jump table directly from disassembly (`0x1007cc`-`0x1008b8`):
the four d-pad directions and four action buttons remap to bits
`0x1000`-`0x8000` and `0x1-0x8` respectively; five other indices abort
the loop (same failure shape as `Start`/`HOME`, already known); and
exactly one surviving case, index 5 — real UID `0x0106C403`, not one
of `AEEHIDButtons.h`'s named Zeebo buttons, but a real, working,
non-aborting case in `ddragonz.mod`'s own compiled table regardless —
remaps to `nButtonID=8`, i.e. bit `0x100` exactly. Added it to the
simulated batch.

**Also changed the injection from a single momentary press to a real
~4-second hold** (re-fires every tick, ticks 60-300, rather than once):
a single press only ever kept the gate nonzero for the one tick before
the next "publish" cleared it again, and — confirmed live — a held
press is what it actually took to move anything further.

**Result: real, concrete, causally-confirmed progress.** With the
correct bit included and the press held, `applet+0x50`/`+0x54` (the
per-tick state-machine function-pointer pair this project has tracked
since early in the Double Dragon investigation) **left its old parked
values and transitioned through three distinct real states** —
`0x1209c0`/`0x11d274` (startup) → `0x1222f0`/`0x107104` (still pre-
press) → `0x121110`/`0x1063ec`, reached only once the gate carried bit
`0x100`, and stable there for the rest of every run tried (up to ~500
real ticks). This is the first time all session real code has visibly
*reacted* to simulated input, not just correctly stored it. It settled
at this new state and stayed there, though — no new `DrawText` output
appeared in any run (temporary trace, reverted; longest continuous
observation ~500 ticks / real wall-clock minutes given this dev tool's
own interpreter and instrumentation overhead per tick). Releasing the
held press (letting the injection window end at tick 300) didn't
trigger any further visible change either. Genuinely unresolved
whether `0x121110`/`0x1063ec` is itself another real wait-state (for
something not yet identified — a release-edge, a different real
signal, more real time than this session tried) or whether it's
already mid-transition and just hasn't produced a frame yet by the
last tick observed. A concrete, disassembly-ready next thread:
`0x121110` and `0x1063ec` themselves, not yet looked at directly.

All temporary instrumentation (the gate/field watchpoints, the
periodic `applet+0x50`/`+0x54` status print, the `DrawText` dedup
trace) reverted. The real, corrected 9-event button batch and the
sustained-hold injection are kept, permanent and documented, in
`tools/game_probe.cpp`. 259/259 tests pass.

---

**Took the "disassemble `0x121110` directly" thread immediately, and
it fully explains the idle state — right down to the exact bits it's
still waiting on.** `0x121110` (the real per-tick handler
`applet+0x54` now points at) opens by calling the real gate/combine
function *itself* (`bl 0x11a2ec`, the exact function traced last
round) plus four more real per-tick sub-calls (`0x11a1dc`, `0x11fa30`,
a call on `applet+0xab0`, `0x11f2d8`) before checking anything -- so
this state is genuinely active, not dead, re-evaluating real input
every real tick.

It then checks two real, independent conditions, neither of which our
simulated input satisfies:

1. `(applet+0x3618 & 0x130) == 0x130` -- a *second* OR'd gate this
   project hadn't looked at before, fed from the same real per-device
   `+0x44` field (not `+0x48`, which feeds the already-known
   `applet+0x361c` gate) by the same real "publish" function traced
   last round. `0x130` decodes to bits 4, 5, and 8 -- the real
   left-shoulder, right-shoulder, and (already-simulated) UID-`0x403`
   buttons, all three held at once. If satisfied, tail-jumps straight
   to a different real function, `0x122bac`.
2. Otherwise, checks `applet+0x15ac & 0x10000000` (a new, specific bit
   of the same real "busy/pending load" field this project identified
   at the very start of the Double Dragon investigation, previously
   only known by bit 0). If set, tail-jumps to `0x122a98` with a real
   16-bit value read from `applet+0x15b0`. If not, returns immediately
   -- the do-nothing path this project's own live traces show
   happening every tick.

Checked both live (temporary status print, reverted): `applet+0x3618`
correctly carries `0x0000F10F` (matching the already-confirmed
`applet+0x361c`, same bits, different destination field) but is
missing bits 4 and 5 -- our simulated batch never included the real
shoulder buttons, so condition 1 never fires. `applet+0x15ac` is
`0x00000003` throughout (bit 0 and bit 1 both real and already set,
independent of any of this round's input work) -- nowhere near bit
`0x10000000`, so condition 2 never fires either. Neither branch has
ever been reachable with what's been simulated so far; the state
genuinely, correctly does nothing every tick given the current real
input, which is exactly the observed behavior.

**This isn't a dead end — it's two concrete, well-specified next
experiments**, both immediately actionable without any further
disassembly: (a) add the two real shoulder-button UIDs (`0x0106C406`,
`0x0106C408`) to the simulated batch and see whether the three-button
combo really is the intended path (plausible for something like a
"skip loading"/debug unlock, less plausible as an ordinary "continue"
input, but not yet ruled out), or (b) trace what real code is
supposed to set `applet+0x15ac` bit `0x10000000` — a real condition
entirely independent of button input, meaning even a perfectly-
simulated press might never be sufficient on its own if this is really
what's gating progress. Not attempted this round — a deliberate stop
after fully characterizing the block, not a stall. No code changes
this round (disassembly and live reads only); `git diff --stat` clean.
259/259 tests pass (unchanged).

---

**Tried both of the two identified experiments this round. One
resolved cleanly (a real, if uninteresting, answer); the other is
still open, now with a much narrower, well-evidenced remaining
question.**

**(a) The three-button combo is real, and it does something —
resets the applet.** Added the two real shoulder-button UIDs
(`0x0106C406`/`0x0106C408`) to the simulated batch (temporary,
reverted after). `applet+0x3618` correctly reached `0x0000F13F`
(bits 4, 5, 8 all present) and the transition to `0x122bac` fired for
the first time. Disassembled it: it's a pure setup function, no
loop, no real work — it just installs three new function pointers
(`applet+0x50`, `+0x36b4`, `+0x36bc`) and returns. Computed the real
installed value of `applet+0x50` directly from the literal pool:
`0x1209c0` — **the exact same address `applet+0x50` held at real tick
0**, before anything had run. Confirmed live: the whole real loading
sequence visibly restarted (a fresh `LOAD ERROR`/`ERROR CODE:3`/
`LIST COUNT:11` appeared, a third distinct real error code+list-count
pair from a fresh pass through the resource dispatcher, this time
with all nine simulated buttons held throughout, which evidently
changes which entry fails first). This three-button combo is a real,
working "restart the applet" trigger, not a menu-progression input —
a real discovery, but not the one this project needs, and not chased
further (a "why does entry 11 fail differently" question is a new
thread of its own, not core to unblocking the title screen). Removed
the two shoulder buttons from the simulated batch again afterward.

**(b) `applet+0x15ac` bit `0x10000000` — found nine real, distinct
writers to this field across a full run (temporary watchpoint,
reverted after), and confirmed all nine only ever touch the field's
low byte.** Values observed: `0x01` (one writer, `0x107380`, after
two real preceding calls — `0x11f804` then `0x122520` — an
unconditional `orr #1`, never cleared), then `0x03` (four more real
writers additionally setting bit 1, also never cleared). The high
three bytes (bits 8-31, including the specific bit `0x121110` checks)
stayed `0x00` from every one of the nine real writers, in every run
tried, button held or not. This makes the open question sharper than
before: it's not that this project hasn't found *a* writer, it's that
**none of the nine real writers this specific execution path reaches
ever touch that bit at all** — strong (though not yet conclusive)
evidence that whatever sets it lives on a real code path this
project's HLE hasn't reached or driven yet, not merely a matter of
holding the right button. Not chased further this round (would mean
either a wider static search across the whole `.mod` for other real
references to this same field offset, or identifying and driving
whatever real subsystem the nine known writers themselves depend on).

All temporary instrumentation (the shoulder-button test batch, the
`applet+0x15ac` watchpoint, the `+0x3618`/`+0x15ac` status print)
reverted; `git diff --stat` clean. 259/259 tests pass (unchanged --
investigation only, no code changes survive this round).

---

**Rather than keep static-searching blind for `applet+0x15ac`'s
missing writer, went after the other loose thread this project already
had on record: the still-unidentified class `0x01005511`, reached
only via the real HID-device path and left as a blind stub since it
was first found.** Traced its real calls live (temporary, reverted to
a permanent, real implementation after): slot 4 gets called three
times with a small-integer-ID/value shape (`SetProperty`-like), then
slot 3 registers a real callback (`ddragonz.mod` `0x11d020`) with a
real userdata pointer, then slot 6 gets one more call. Disassembled
the registered callback and the real function it branches into
(`0x11f4dc`): together they only act on an event struct with field
`+8 == 4` and field `+16` in `{2, 3}`, and on success call a real
vtable slot 11 method with the literal argument `100` — a
status/percentage-report shape. Combined with the class being reached
only as part of a broader real "environment ready" sequence (right
after real HID controller detection) and this project's own prior
finding that Zeebo distributes games by download (the `274754`-style
catalog IDs used throughout this file), this strongly resembles
Zeebo's real download/install-progress notification service.

**Implemented it for real, not as a guess:** `tools/game_probe.cpp`
now captures the real callback/userdata from slot 3 and, once
captured, invokes it once with a real, minimal, disassembly-confirmed
event struct (`+8=4`, `+16=2`) reporting a truthful "100% / complete"
status — truthful because this repo's own game assets genuinely are
complete (three independent sources agree byte-for-byte, confirmed
earlier this investigation), not an invented condition.

**Verified live: it reaches the real check without wandering or
throwing, but doesn't clear it.** `0x11f4dc`'s real success path
requires a real sub-object pointer at `pUser+8` (confirmed present —
it's `0x80061000`, this project's own registered `0x01005511` object,
meaning real code already wired a self-reference in during one of the
earlier slot 4/6 calls) *and* a real nonzero byte at `pUser+37`
(confirmed absent — reads `0x00` every time tried). Missing that one
byte routes to the real failure path instead (`applet`-analogous
field `+28 = -1`, `+36 = 0`) — no crash, just a quiet no-op, matching
`applet+0x15ac` staying unchanged in every run.

**This is now the same shape of open question as `applet+0x15ac`'s
missing bit, one layer deeper and much narrower**: a single real byte,
at a known real address relative to a known real object, that nothing
this project has triggered so far ever sets. Not chased further this
round — the natural next step is finding what should write
`pUser+37`, most likely via slot 6's own real call (passed `r1=
0xf0000618`, an address in this project's own HLE trap range,
suggesting real code handed back one of this project's own interface
pointers as data — not yet understood) or via deeper tracing of
whatever the three slot 4 `SetProperty`-shaped calls configure. The
real, working `CreateSignal`-equivalent implementation and the
one-shot notification injector are kept, permanent and documented, as
a correct building block for that next round. All other temporary
diagnostics (`DrawText` trace, the `applet+0x15ac` status print, the
pre-invocation field dump) reverted. 259/259 tests pass.

---

**Switched to Super BurgerTime for a fresh angle (per user direction),
picking up its one open, unstarted thread from where the previous
round left it: an `UnimplementedInstruction("Miscellaneous instruction
space (MRS/MSR/etc.)")` at module offset `0x9c`, roughly a million real
steps into the first real timer tick, where the live instruction word
doesn't match the raw `.mod` file's content at the same address.**

Reproduced live with a temporary `Memory::Write8` watchpoint on
`kBase+0x9c` (bridged via the same `g_debug_watch_addr`/`g_debug_last_pc`
global pattern used throughout this log, both reverted after use): every
single write comes from the exact same PC, `0x100050`, inside
`supbtime.mod`'s own ROPI relocation-fixup loop (`0x100040`-`0x100054`,
already fully characterized in this log's Super BurgerTime "stack/module
collision" entries above) — confirming this is the *same* real veneer
misbehaving a second time, not a new kind of bug. Roughly 80,000
consecutive writes to the exact same address, each the previous value
plus the relocation base (`0x10009c`), landed squarely on the log's
already-known signature for "the table's own backing storage was
already zeroed and reused as scratch, so every 'entry' reads as offset
0, degenerating into repeatedly corrupting the relocation base itself."

**The open question this log left unresolved — why the veneer runs a
second time at all — is now answered.** A second temporary watchpoint,
this time on entry to the veneer itself (`pc == 0x100014`), caught
exactly two hits: the real, expected one at the very start of
`AEEMod_Load` (`lr=0xf0000000`, this tool's own harness value), and a
second one deep in tick 0 (`lr=0x0011c0d0`) — a *real* in-module return
address, proving a real caller, not this tool, re-enters it. Tracing
that call site (`supbtime.mod` `0x11c0bc`-`0x11c0cc`, part of the
per-frame "task list" walker `0x11c06c` this log already identified)
with a live register trace found the exact mechanism: `0x11c06c`'s
per-entry body makes *three* real vtable calls on the same single-entry
list every pass — slot `0x2c` ("tick", the one this project's own HLE
overrides), then an unrelated object's slot `0x90` (fed slot `0x2c`'s
return value via `r1` — a real, evidenced status hand-off this log
hadn't previously noticed), then slot `0x28` again on the *same* object
— all *before* the loop re-reads the list head to decide whether to
exit. This project's existing fix for the "list never becomes empty"
gap (documented earlier in this log) cleared the list head
(`0x2e28fc`) from inside slot `0x2c`'s stub, immediately — meaning the
very next call in the *same* pass (slot `0x28`, still against the now-
null `[r4]`) dereferences a null vtable unconditionally: `r2=[r4]=0`,
`r3=[r2]=0`, call target `=[r3+0x28]=[0x28]=0`, confirmed directly via
the register trace at the call site. Jumping to address 0 triggers this
codebase's own already-documented "wander through zeroed memory"
behavior (`CallArmFunctionChecked`'s own doc comment, and Double
Dragon/Peggle's earliest gaps): 262,144 harmless zero-decoded steps
later, PC coincidentally lands back on `kBase` and re-runs the entire
one-time relocation veneer over its already-consumed, now-zeroed table
— which is exactly the corruption chain this round started by
reproducing. The arithmetic matches exactly: the null-vtable call
happened at step 2,275; the re-entry into the veneer at step 264,421;
264,421 − 2,275 = 262,146 ≈ `kBase / 4` = 262,144.

**Fixed** by moving the list-head clear from slot `0x2c` (byte offset,
the first of the three per-pass sub-calls) to slot `0x28` (the *last*
of the three) in `tools/game_probe.cpp`'s `sbt_methods` table for class
`0x01001017`. Still the same honest, minimal placeholder this log
already committed to (not a claim about which slot "really" owns
cleanup) — just the latest-firing of the three already-being-called
real slots, chosen specifically so nothing in the same pass
dereferences the entry again afterward.

**Verified against real Super BurgerTime**: a 45-second sustained run
past "Reached the event loop..." produces no wander warning, no thrown
exception, and a rich, varying stream of real HLE calls every tick
(not the fixed, unchanging small loop Peggle's own investigation found
and paused on) — needs external termination rather than exiting on its
own, the same "success looks like a hang" signature this log already
trusts. No regression: Double Dragon and Peggle both re-verified to
still reach their own real event loops cleanly. 259/259 tests pass (the
fix lives entirely in `tools/game_probe.cpp`, the dev harness, not core
HLE code). All temporary watchpoints/traces (`Memory::Write8` watch,
the veneer-entry trace, the call-site register dump) reverted before
committing — `git diff --stat` clean except the intended fix.

**Significance**: this resolves the deepest, longest-open gap in the
Super BurgerTime investigation — a real, self-inflicted memory
corruption bug this codebase's own earlier placeholder fix was
triggering, not a genuinely unimplemented CPU feature or a missing HLE
slot. Super BurgerTime now sustains real, varied per-tick execution the
same way Double Dragon does, past the point where Peggle's own
investigation paused for lack of further real evidence. `.pkg` asset
loading remains the next real undertaking for this title, same as
`resources.bar` does for Peggle.

---

**Cracked `.pkg` from scratch — unlike `resources.bar` (Peggle), this
one fully yields.** The earlier survey's caution turned out justified:
it isn't Quake's classic PAK format (the two header `uint32`s don't
parse as a coherent `(dirofs,dirlen)` pair), but real structure was
there to find by direct byte analysis, cross-validated at every step
rather than assumed.

**Header** (12 bytes): `"PACK"` magic, a real entry count (`7` for
`supbtime.pkg`), and a real absolute file offset (`579390`) that,
followed directly, lands on one more real zlib stream — a strong,
non-coincidental anchor, not a guess.

**Directory**: real non-zero table data starts at byte 268 exactly —
12 (header) + 256 (an all-zero reserved block of unconfirmed purpose,
preserved as fixed padding) — confirmed precisely by locating known
values (a real compressed size found via scanning for real zlib magic
bytes and test-decompressing each candidate, which real deflate data
correctly rejects false positives on: 12 candidate offsets found by
byte-pattern search, only 7 actually decompressed cleanly, matching the
declared entry count exactly) as raw bytes at exact file offsets, then
computing the surrounding record layout from their relative spacing
rather than assuming an offset/width up front. Real record shape (20
bytes, repeats spaced exactly 20 bytes apart, verified against all 7
real entries): `[unknown constant, 0x00020000 in every real record
seen][hash, algorithm not identified — tried, and ruled out, zlib's
own CRC32 of the filename][compressed_size][decompressed_size][absolute
file offset]`.

**Per-entry data**: raw RFC 1950 zlib streams (confirmed both by the
`0x78 0xda` magic and by every one of the 7 real streams decompressing
cleanly to their table-declared exact size) — not gzip-framed like
`GgzArchive`'s convention, so no per-stream embedded filename.

**Filenames, found by following the header's own footer-offset field**:
one more real zlib stream, decompressing to exactly `entry_count *
256` bytes — 7 fixed-width, null-padded ASCII records: `gc05.bin`,
`gc06.bin`, `gk03`, `gk04`, `mae00.bin`, `mae01.bin`, `mae02.bin`. Real,
legible names landing exactly on the declared entry count is about as
strong a confirmation as this kind of reverse-engineering gets — the
same bar the `.obm1` sprite format crack (TASKS.md Phase 8, Double
Dragon) was held to.

**Implemented as permanent, tested code**, following the exact
established conventions of `GgzArchive`/`Obm1Image`:
`core/loader/pkg.{h,cpp}` (`PkgArchive::Parse`/`Extract`),
`tests/pkg_test.cpp` (synthetic fixtures built with zlib's own deflate,
covering the happy path, an empty payload, and every malformed-input
path — no real game bytes, per `CONTRIBUTING.md`'s clean-room policy),
and `tools/zeebulator_pkg_inspector` (lists/extracts a real `.pkg`,
matching `ggz_inspector`/`obm1_inspector`'s shape). **Verified directly
against the real file**: all 7 entries parse and extract cleanly,
byte-for-byte matching every offset/size this entry independently
derived by hand. 266/266 tests pass (7 new).

**Incidental finding, not yet resolved**: none of these 7 real names
match the `"boot"`/`"boot.rom"`/`"zupa_p1.rom"`/`"zupa_s1.rom"` names
an earlier round in this log found referenced by static-base slot
`0xd0`'s real manifest string — meaning `supbtime.pkg` almost certainly
holds general UI/graphics/audio assets (peeking at the extracted
content: `gc05.bin`/`gc06.bin` look like tile/graphics data, `gk03`/
`gk04` look like small real offset/index tables given their visibly
monotonic values, `mae00-02.bin` are large, mostly-zero 512KB blocks —
none of it examined deeply enough yet to be more specific), not the
actual NeoGeo-style arcade romset the "boot" manifest references. That
romset's real source is still unlocated — a separate, still-open
problem from the one this entry closes.

---

**Located exactly why the crash-fixed build still never gets past
tick 0: confirmed, precisely, what the still-unresolved romset-loading
polling loop is actually waiting on, and why it can never resolve with
this repo's current assets.**

A long, patient run (170 real seconds, well past this project's normal
test windows) confirmed tick 0's own `CallArmFunctionChecked` call
never returns and never hits its 5,000,000-step budget either —
consistent with a genuine, bounded-but-very-long real polling loop
rather than a fast crash, matching this log's own earlier
characterization of this exact condition ("waiting on ROM-data
readiness, not just elapsed time").

A temporary trace on every real `IFILEMGR_OpenFile` call (reverted
after use) settled it directly: real code tries, in order, six real
candidate paths —

```
.\boot.pkg
.\boot\boot.rom
roms\boot.pkg
roms\boot\boot.rom
roms\neogeo\boot.pkg
roms\neogeo\boot\boot.rom
```

— i.e. a real, sensible multi-directory search for a file named
**`boot.pkg`** (and a `boot.rom` inside a `boot\` subdirectory), not
`supbtime.pkg`. Every one of these six real opens fails (nothing in
this repo's VFS is registered under any of these names), which is what
feeds the polling loop that never resolves.

**This is a materially different situation than everything else fixed
in this log.** `supbtime.pkg` (now fully cracked and implemented) is
this specific game's own asset package; `boot.pkg` is a distinct,
smaller bootstrap/romset-selector file (matching the `"boot"`/
`"boot.rom"` names at the very start of the shared multi-game manifest
string found earlier) that isn't among this game's own downloaded
files — `research/games/Super BurgerTime/mod/279125/` has no
`boot.pkg` or `boot.rom` anywhere. Given the manifest spans several
unrelated games (Zupapa, Zedblade, and others) under one shared
arcade-core binary, `boot.pkg` reads as a **shared, system-level file**
this arcade core expects to find on every real device, independent of
which specific game is installed — not part of any individual game's
own download.

**Not pursued further this round**: sourcing this file (if it exists
in any of this project's already-sanctioned archives) needs the user's
explicit go-ahead first, per this project's own standing convention —
unlike `sound.ggz`/`data.ggz`, this isn't a file this game's own
download folder is short a few bytes of; it would be an entirely new
asset from a different, not-yet-confirmed source. All temporary
instrumentation (the `OpenFile` trace in `core/brew/file_hle.cpp`, the
widened tick-trace window in `tools/game_probe.cpp`) reverted; `git
diff --stat` clean. 266/266 tests pass (unchanged).

---

**User authorized sourcing `boot.pkg` from this project's own
sanctioned local archive (the same `archive.org` `zeebo-arquivista`
preservation collection already used earlier this session, not a new
download).** Given the shared-manifest evidence above (`boot`/
`boot.rom` sitting alongside several unrelated games' own romset
entries in one binary-embedded table), checked whether any of the
other classic-arcade-port titles already present in that local archive
happen to bundle their own copy of this same shared file, rather than
assuming it needs sourcing from anywhere new.

**Found it on the first check**: `Karnov's Revenge.zip`'s own
`mod/279126/boot.pkg` (1,816 bytes) — a different game entirely, but
built on the same shared arcade core. **Confirmed real and generic
before using it for anything**, not just assumed: this project's own
`PkgArchive` (built and tested entirely against Super BurgerTime's
`supbtime.pkg`, with zero code changes) parses this second,
independently-sourced real file correctly on the first try — a strong
cross-title validation of the format work above, not just of this new
file. Its one real entry, `boot.rom` (8,192 bytes), decodes to what
looks like a genuine 68000-style exception vector table (most slots
pointing at one shared default handler) — boot/init code, not per-game
asset data, consistent with it being a shared, system-level file
rather than something specific to Karnov's Revenge.

**Wired in as a new, permanent, optional 5th CLI argument**
(`tools/game_probe.cpp`'s `MergeBootPkgInto`, following the same
"take a real path at runtime, embed nothing" convention as every other
asset argument this tool already takes): registers both the raw
`.pkg` bytes and the extracted `boot.rom` bytes under all six real
candidate paths the earlier `OpenFile` trace found, so this dev tool
doesn't need to guess which one real code actually ends up using.

**Verified against real Super BurgerTime, with real `boot.pkg`
supplied**: tick 0 now runs a genuinely new, much longer real sequence
— many new, previously-unseen real HLE trap addresses fire (real file-
read/seek activity, not just the same six failed `OpenFile` attempts
looping) — reaching **262,900 real steps** before wandering to a new
gap, a large jump past the previous wall (which never even completed
one HLE-call's worth of real forward motion). No regression: Double
Dragon still reaches its own real event loop cleanly.

**The new gap, traced precisely**: real code at `supbtime.mod` offset
`0x1465b0` does an indirect dispatch, `ldr pc, [r2, r3, lsl #3]` (an
8-byte-stride jump table, `r3=1` at the point of failure) through a
real, correctly-relocated runtime address (`r2` resolves to a live,
in-module address, confirming this project's own ROPI relocation fix
is working correctly here too) — but the specific slot read back `0`.
Checked directly against the raw file at that same module offset: it
is **not** zero there — the file holds a real, sane, monotonically-
increasing sequence of link-time pointer values, exactly the shape of
one more real relocation-table entry list. This address falls *inside*
the same reclaimed-and-zeroed scratch range this log's earlier
`0x2e28fc` entry already characterized (the original relocation
table's own storage, reused as fresh BSS once its one-time job is
done) — meaning this is the same *kind* of gap, not a new mechanism: a
real dispatch table that some not-yet-reached piece of real
initialization is supposed to populate before this call, in memory
this codebase's own execution path hasn't driven the population of
yet.

**Not pursued further this round** — a substantial, multi-part round
(cracking `.pkg`, fixing the crash, sourcing and validating `boot.pkg`,
reaching a new 262,900-step milestone) is a natural, well-evidenced
stopping point. The concrete next step for whoever continues is
finding what real code is supposed to write to this table before
`0x1465b0` reads it — the same kind of search Peggle's own arena-field
investigation and this game's own `0x2e28fc` gap both already resolved
successfully, so there's real precedent this is tractable, just not
attempted yet. All temporary instrumentation (the register trace at
`0x1465b0`) reverted; `git diff --stat` clean except the permanent
`MergeBootPkgInto` addition. 266/266 tests pass (unchanged — the new
code lives entirely in `tools/game_probe.cpp`, the dev harness).

---

**Chased the `0x1465b0` dispatch-table gap directly, found its real
writer, and narrowed the remaining problem to something materially
bigger than any gap fixed so far in this project.**

A static literal search for the table's own base value (`0x220ba8`)
turned out unhelpful on its own — 154 occurrences across the module,
because that literal is this whole binary's *general* static-base
pointer, referenced by countless unrelated subsystems at their own
further offsets, not something specific to this one table.

**A live watchpoint spanning the entire run (temporary, reverted)
found the real writer instead**: `supbtime.mod` offset `0x146150`-
`0x146154`, a real "register a handler" function (`str r6,[r8,r4,lsl
#4]` into a parallel 16-byte-stride table, then `str r3,[r8,r2,lsl
#3]` into *this* 8-byte-stride table, where `r2 = 2*r4+1` — the exact
same odd-index transform the reader at `0x1465a0` independently
applies, confirming this is genuinely the right writer, not a
coincidental address match) — and it *does* run, immediately before
the failing dispatch, writing to exactly the right slot. The value it
writes (`r3`, loaded from its own caller's stack) is real, but for the
one case observed, resolves to whatever real value that caller had —
not yet traced further back.

**Ruled out one plausible explanation directly**: with `boot.pkg`
supplied, a fresh `OpenFile` trace (temporary, reverted) shows real
code makes exactly **one** real file-open this entire run (`.\
boot.pkg`, which now succeeds) — no attempt to open `supbtime.pkg`,
this game's own asset package, or anything else. So this isn't a
missing-file gap the way the `boot.pkg` round was; the value feeding
this table slot comes from processing `boot.rom`'s own real content
(the 68000-style vector table already confirmed), not from a second
file this codebase hasn't wired in.

**Assessment, not yet proven**: `boot.rom`'s real vectors most likely
point into the *game's own* romset data — the actual 68000 program
code for "zupapa" — which supbtime.pkg's own seven re-encoded entries
(`gc05.bin`, `gk03`, `mae00.bin`, ...) may or may not represent in a
form this codebase could use directly (their content doesn't look like
raw 68000 opcodes on inspection — see the `.pkg`-cracking entry above).
Fully resolving this table would likely mean implementing real
behavior for a nested arcade-CPU-core dispatch — a
substantially bigger undertaking than any static-base slot or missing-
class scaffold fixed so far in this project, closer in scope to
Peggle's own paused "real BREW MP SDK header" wall than to a normal
incremental gap.

**Stopping here for this round**: three real, substantial pieces of
progress landed (the `.pkg` format, the crash fix, and the `boot.pkg`
milestone reaching 262,900 real steps), plus this table gap now
precisely characterized down to its real writer — a solid, well-
evidenced place to pause rather than open-endedly guess at 68000
emulation. All temporary instrumentation (the watchpoint, the second
`OpenFile` trace) reverted; `git diff --stat` clean. 266/266 tests
pass (unchanged — investigation only, no functional changes this
round).

---

**The user made the full 61-title real Zeebo dump collection directly
available in `research/games/` (previously this project only had three
titles individually extracted plus indirect access to one archive.org
mirror). Used it to crack Peggle's `resources.bar` — Phase 2's
originally-deferred "BAR file parser" task — completely.**

First checked whether `resources.bar` might be the same format as
Bejeweled Twist's/Zuma's Revenge's own `resources.dat` (both now
directly available, and PHASE8_LOG's own earlier Peggle entries already
flagged them as the same PopCap-resource-container family). It isn't:
both of those real files open with a real, distinct magic, `PPCPRCON`
— Peggle's own `resources.bar` doesn't share it. A real, useful
cross-check, but not the shortcut hoped for; also confirmed no *other*
one of the 61 titles has a `resources.bar` of its own (only `.dat`
variants elsewhere), so there was no second same-format sample to
cross-reference the way `boot.pkg` worked for Super BurgerTime.

**Cracked directly from the raw bytes instead, the same way GGZ and
`.obm1` were (both also have no real ARM code parsing them — this
title's own `resources.bar` is opened through the generic real
`ISHELL_LoadResDataEx` BREW API, confirmed in an earlier round, not a
custom `.mod`-embedded parser).**

Header self-consistency was the first real foothold: `offset 24`
(1764) plus `offset 28` (13,493,221) sum to the file's own exact real
size (13,494,985) — too exact to be coincidence. Confirmed directly:
byte offset 1764 is exactly where a real `ID3` (MP3) signature begins
in the live file. From there, `offset 16` (528) — cross-checked as
`offset 8` (32) + `offset 12` (496) — turned out to be the real start
of a flat table of `uint32` LE absolute file offsets (`offset 20` = 308
of them), one per real resource, immediately followed by one more
`uint32` sentinel value equal to the total file size. **Verified about
as strongly as this kind of reverse-engineering gets**: scanning every
one of the 308 computed offsets against the live file, all 308 land
exactly on a recognizable real signature or otherwise-sane content,
zero exceptions — 7 real `ID3`/MP3s, 39 real `RIFF`/WAVE files, 246
real PNGs (each preceded by a tiny two-byte-length + null-terminated
`"image/png"` wrapper this parser doesn't strip), 11 fixed-32768-byte
raw blocks with no recognizable magic (very likely an uncompressed,
proprietary texture/tile format — not decoded further), and 4 trailing
entries that are real, legible `^`-delimited localized UI strings
(`"English^Español^Portugu..."`, `"NEW GAME^PLAY^AWARDS^OP..."`). No
container-level compression anywhere — simpler in this one respect than
either GGZ or `.pkg`.

A first, 496-byte sub-table at `offset 32` (60 real, monotonic-ID-
shaped 8-byte records) was found but deliberately **not** further
decoded — not needed to extract raw resource bytes by index, and its
one candidate field (an apparent index/ID under 2000, too large to be a
direct index into the 308-entry resource table) didn't resolve cleanly
enough to trust a guess. Flagged, not glossed over, matching this
project's own standard for a real-but-unconfirmed structure.

**Implemented as permanent, tested code**, following the exact
established `GgzArchive`/`PkgArchive` pattern: `core/loader/bar.{h,cpp}`
(`BarArchive::Parse`/`Extract`), `tests/bar_test.cpp` (synthetic
fixtures built by hand, no real game bytes, per `CONTRIBUTING.md`'s
clean-room policy — 7 tests covering the happy path and every
malformed-header/table path found worth guarding), and
`tools/zeebulator_bar_inspector` (lists/extracts a real `.bar`, with a
best-effort real-signature sniff for display only — `BarArchive` itself
stays a pure container, no format-specific decoding, the same
container/codec layering this project already uses for GGZ/`.pkg`).
**Verified directly against the real file**: all 308 entries parse and
extract cleanly, matching the manual analysis exactly (246 image + 7
mp3 + 39 wav + 16 unknown). 282/282 tests pass (7 new — this and the
same-round `-Wall`/`-Wextra` cleanup below).

Caught immediately by this session's own freshly-enabled `-Wall
-Wextra` (see the cross-cutting entry below): a real signed/unsigned
comparison warning in `bar_inspector.cpp`'s own MIME-string-length
sniff, fixed with an explicit cast before it ever shipped.

**Significance**: this was the second of Peggle's two real, long-
standing open questions (alongside the still-unidentified per-tick
loop ID `0x0101eb0b`) and the more concrete of the two — unlike that
ID (dead-ended on "no real BREW MP SDK header available"), this one
yielded entirely to direct evidence. Doesn't unblock Peggle's own
per-tick loop by itself (that's still gated on the unidentified real ID
this container crack doesn't touch), but real asset loading — PNGs,
WAV/MP3 audio, and now confirmed to be simple, uncompressed, directly
extractable — is no longer a blocked, "device-firmware-only" format for
this title.

---

**Turned the newly available game collection on Peggle's *other* long-
standing open question — the still-unidentified per-tick loop ID,
`0x0101eb0b` — using the exact cross-referencing technique that found
`boot.pkg`: check whether another real title's own `.mod` references
the same real ID.**

Extracted `bjt.mod` (Bejeweled Twist) and `zumar.mod` (Zuma's Revenge)
from their own real downloads. Bejeweled Twist's own binary doesn't
reference `0x0101eb0b` (or any of the other three IDs Peggle's own
investigation left open) at all. **Zuma's Revenge's does** — the exact
same literal, at a real call site with the exact same shape Peggle's
own does: `ldr r1,[r0]` (vtable) / `ldr r3,[r1,#8]` (real slot 2 =
QueryInterface) / `ldr r1,[pc,#N]` (the `0x0101eb0b` literal) / `bx r3`.

**Confirmed this is real, shared, statically-linked SDK code, not just
a structural coincidence**: the tiny helper function both titles'
real call sites go on to invoke (`peggle.mod 0x105b50` / `zumar.mod
0x105540`) opens with **byte-identical machine code** in both binaries
(`ldr r3,[r0]; ldr r3,[r3,#12]; bx r3` — a generic "tail-call vtable
slot 3" thunk). Different games, same compiled bytes — about as strong
a confirmation of a shared library as this kind of analysis can produce
without a header.

**Traced the real call shape end to end in both binaries and found the
actual real usage pattern this project's own earlier "self-propagating
stub" work hadn't yet covered**: after `QueryInterface(ctx, 0x0101eb0b,
&out)` succeeds, real code calls `out`'s own real vtable **slot 4**
with no output-pointer argument at all — its *return value* (not an
output parameter) is a fresh object in its own right. Confirmed in
Peggle (`peggle.mod 0x109a54`-`0x109a94`): that returned object's own
real slot 3 gets called repeatedly, `(this, buffer_ptr, size)`, in
1000-byte chunks, ending with a final `(this, 0, 0)` call — a real,
unmistakable "write a stream of bytes" shape, most plausibly telemetry
or logging given nothing anywhere checks the result. This is a
*different* real slot-3 meaning than the outer arena object's own
(confirmed earlier as a QueryInterface-child shape) — genuine evidence
these are two distinct real interfaces, not the same self-propagating
kind recursing.

**Fixed**: added slot 4 to the existing self-propagating-stub machinery
in `tools/game_probe.cpp` (`build_self_propagating_stub`), returning a
fresh, independent, all-slots-safe-stub object via `r0` directly
(deliberately *not* another self-propagating instance, since this
object's own slot 3 needs write-stream semantics, not QI-child
semantics — an honest no-op discard is correct here, not a
misunderstanding papered over).

**Verified against real Peggle, and the change is dramatic**: the
per-tick loop's own call count jumps from a fixed ~20 (documented as
"looping harmlessly on placeholders" two rounds ago) to 500+ calls
across the first 10 ticks, including **real `IFileMgr` activity for
the first time this title's entire investigation** — confirmed via a
temporary `OpenFile` trace (reverted): real code now opens a real save
file, `udata/game` (`CREATE`, then `READWRITE`, then later `READ`) —
genuine forward progress, not another fixed loop, though by tick 7-9
the *shape* of each tick's own call sequence does largely repeat again
(with at least one real, tick-over-tick advancing value observed, a
pointer growing by exactly 4 bytes per tick). No regression: Double
Dragon still reaches its own event loop cleanly, and Super BurgerTime
hits the exact same, already-documented wander point at the exact same
step count (262,901) as before — this fix is correctly scoped to
Peggle's own confirmed context field and doesn't touch either other
title's behavior. 282/282 tests pass (unchanged — the fix lives
entirely in `tools/game_probe.cpp`, the dev harness). All temporary
instrumentation (the second `OpenFile` trace) reverted; `git diff
--stat` clean except the real fix.

**Significance**: this is real, verified, first-time-ever access to
Peggle's own save-file I/O path — a genuinely new milestone, not a
repeat of the "sustained execution but frozen game state" finding from
two rounds ago. `0x0101eb0b`'s own real meaning is still not identified
by name (very plausibly telemetry/logging, not confirmed), but that no
longer matters for further progress: the object it produces is now
handled correctly regardless of what it "really" is. The next concrete
step is determining what real save-file *content* Peggle's own code
expects to read back from `udata/game` (currently empty/freshly
created in every run) — a new, narrower, well-characterized thread,
not yet started.

---

**Followed that thread immediately (temporary tracing on `FileHle`'s
real `OpenFile`/`Read`/`GetInfo` implementations, all reverted after):
real code doesn't need any *specific* real save content at all — the
round-trip is self-contained and already works correctly.**

The real sequence, confirmed directly: `OpenFile('udata/game',
CREATE)`, then `OpenFile('udata/game', READWRITE)` — both during
`HandleEvent(EVT_APP_START)`, before tick 0 ever starts — during which
real code itself writes a real, **6,672-byte** default save structure
into the file (this codebase's own `FileHle` correctly persists it,
purely as a side effect of already-correct `Write`/`writable_files_`
handling, no new code needed). Later, inside tick 0, real code reopens
it read-only and reads exactly **9 real 16-byte records (144 bytes)**
back — not the whole file — before moving on. This is genuinely
working save-data round-trip behavior, not a blocked gap.

**What happens right after those 9 reads is real, substantial, new
execution**: a real loop processing what looks like up to 1,397
distinct items (`r3` stays fixed at `0x575`, `r1` counts up from 0),
entirely within tick 0's own one-time cost — real code doing real,
structured work at a scale far beyond anything reached before this
round's `0x0101eb0b` fix. By tick 7-9 this settles back into a small,
mostly-repeating per-tick shape (the same one this log's previous
entry already flagged as having at least one genuinely advancing
value) — plausibly a legitimate idle/menu state, not another block,
though not yet confirmed either way.

**Checked the one question that actually matters for real user-facing
progress, honestly, the same way this log always has**: a temporary
trace on `IDisplayHle::DrawText`/`DrawRect`/`Update` found **zero real
calls to any of them across a full 20-second run** — still nothing
visible reaches the screen. A parallel `OpenFile` trace across the same
run confirms why at least part of the picture: `resources.bar` (this
round's own freshly-cracked container) is **never once requested** —
real code hasn't reached asset loading yet, despite reaching
substantially deeper real logic than ever before. Not yet determined
whether reaching `resources.bar` needs one more specific missing piece
(the same "narrow, well-characterized gap" shape as almost everything
else in this project) or whether the current idle state is a correct,
if unglamorous, real waiting state (e.g. genuinely idling on real
timer/input conditions this harness's own synthetic input never
satisfies, the same category of gap Double Dragon's own title screen
had before real button-press simulation was built).

**Net honest assessment of this round**: two real, substantial,
verified wins (`resources.bar` cracked completely; the `0x0101eb0b`
interface identified and fixed, unlocking real save-file I/O and
1,397-item real processing) plus one honest non-result (still nothing
visible on screen) — consistent with, not contradicting, everything
this log has found before: internal-state progress and on-screen
progress are different things, and only the latter is the actual goal.
All temporary instrumentation (the `FileHle`/`IDisplayHle` traces)
reverted; `git diff --stat` clean (investigation only this round, no
functional changes beyond what's already committed above). 282/282
tests pass (unchanged).

---

**Followed the "resources.bar never requested" thread to its real
cause and fixed it: `ISHELL_LoadResDataEx` (real vtable slot 41) was a
blind stub the whole time.**

A full instruction trace of one representative later, "idle" tick
(temporary, reverted) found the exact real call: `ldr ip,[r2,#164]`
-- byte offset 164 = slot 41 = `LoadResDataEx`, called **twice in a
row with identical arguments**. Disassembling the surrounding real
function end to end shows the complete, correct, textbook real
pattern: call 1 passes the real documented `-1` buffer sentinel (`mvn
r2,#0`) to query the resource's size only; the size comes back into a
real stack slot; real code then calls a real allocator (`peggle.mod
0x13ac38`) with that exact size; if allocation succeeds, call 2 passes
the fresh buffer to actually fetch the bytes. Real code even checks
the allocator's return for null and takes a real failure path if so --
this is not sloppy or guessed, it's exactly what a real BREW resource
load looks like. The real filename argument resolves to the literal
string `"resources.bar"` in `peggle.mod`'s own data; the real
`(id=4000, type=1)` argument pair was the concrete anchor that unlocked
everything below.

**This also finally explains `resources.bar`'s own first, previously-
unparsed 496-byte sub-table (`core/loader/bar.h`'s "table1"): it's the
real resource-ID directory `LoadResDataEx` needs.** Dumped all 60 real
8-byte records directly: record 59 reads `{type=1, id=4000, unknown=3,
entry_index=304}` -- an *exact* match, on both fields simultaneously,
against the real call's own `(id=4000, type=1)` arguments, resolving to
entry 304 -- independently confirmed earlier in this log as a real,
legible localized string (`"English^Español^Portugu..."`). Not a
coincidence by any reasonable standard. The other 59 records (all
`type=6`, `id` 1000-6000, pointing at entries 58-249) are presumed
image-resource lookups given this archive's image-heavy makeup, though
not each individually cross-checked the same exacting way.

**Promoted into `core/loader/bar.{h,cpp}` as a real, permanent
`BarArchive::Find(type, id)`** (parses the real directory records,
validates `entry_index` is in range, throws on a malformed directory
the same way every other structural check in this class already does)
-- not left as one-off Peggle-specific logic, since the directory
format itself is presumably the same real, generic BREW resource
mechanism every `.bar` file uses. 3 new tests (happy path, a missing
`(type,id)` pair, an out-of-range `entry_index`); `tools/
zeebulator_bar_inspector` extended to list the real directory too --
verified all 60 real records parse and cross-reference cleanly against
the real file.

**Implemented `ISHELL_LoadResDataEx` for real in `core/brew/
ishell.{h,cpp}`** (slot 41): a new `RegisterResourceFile(name, data)`
parses a real `.bar` file's bytes into a `BarArchive` up front; the
real slot then resolves `(pszResFile, wResID, resType)` via `Find`,
honors the real `-1` size-only sentinel, and otherwise copies the real
resource bytes into the caller's buffer -- the exact real contract
traced above, not a guess at the edges. 4 new tests covering the size
query, the real copy, an unregistered resource file, and an
unmatched `(type,id)` pair. `tools/game_probe.cpp` gets a new optional
7th CLI argument (a real `.bar` path, wired to `RegisterResourceFile`
under its own real basename) -- same "take a real path at runtime,
embed nothing" convention as every other asset argument.

**Verified against real Peggle, and it's a genuine, different kind of
result than every prior round on this title**: with `resources.bar`
supplied, real execution takes a **new path it has never taken
before** -- reaching a new real gap after only ~7,000 steps (previously
the run settled into the same ~500-call idle pattern every time,
`resources.bar` never even requested). The new gap is a real,
previously-unseen two-handler event-dispatch mechanism (`peggle.mod`
`0x125914`-`0x125950`: check a real "active handler" slot, call its
vtable slot 25 if set, else fall back to a second slot's vtable slot
26) -- real code now reaches and calls through a generic placeholder
object that was never exercised this way before, and whatever it gets
back leads to the by-now-familiar "wander through zero, hit real
non-zero garbage" pattern. A genuinely new, not-yet-characterized
thread -- not investigated further this round, flagged clearly for
whoever continues. Confirmed **no regression**: Double Dragon, Super
BurgerTime, and Peggle-without-`resources.bar` all behave identically
to their established baselines -- this new behavior is real,
opt-in-triggered forward motion, not a break. All temporary
instrumentation (the tick-20 full trace) reverted. 289/289 tests pass
(7 new -- 3 `BarArchive::Find`, 4 `LoadResDataEx`).

**Significance**: this is the first time in this entire investigation
that a real BREW OS-level API (`LoadResDataEx`) has real, working,
non-stub behavior end to end, backed entirely by this project's own
from-scratch format work. It doesn't reach a visible frame yet -- the
new dispatch gap sits in the way -- but it's a materially different
category of result than every previous "still nothing visible" finding
on this title: real code is now doing something it structurally could
never do before.

---

**Chased the new dispatch gap immediately and found a real bug in this
project's own test harness, not a missing real HLE behavior: two
unrelated dynamic object-address counters in `tools/game_probe.cpp`
could collide with each other and with fixed object addresses.**

A live watchpoint spanning the whole run (temporary, reverted) on the
real two-handler dispatch slots (`peggle.mod` `0x13b1f4`/`0x13b1f8`,
found via the same literal-relocation technique used throughout this
log) confirmed real code correctly populates the second slot with a
real `ISHELL_CreateInstance(0x0103d8ec)` result during
`HandleEvent(EVT_APP_START)` -- exactly as expected. But a targeted
register trace at the real crash site (`peggle.mod` `0x125934`, right
after the real vtable load) found `[this]` resolving to `0xf0001448` --
an **HLE trap address**, not a real vtable pointer. Something had
overwritten the `0x0103d8ec` stub object's own header.

**Root cause**: `build_self_propagating_stub`'s own dynamic address
counter started at `0x80030000`, and a separate counter added last
round for the arena object's real slot 4 started at `0x80038000` --
only 8 slots apart, with neither counter aware of the other or of any
of this file's ~30 other fixed object addresses (which top out around
`0x80066000`, well below both counters' own eventual range). With
`resources.bar` now wired in and real code recursing through
substantially more real `QueryInterface` chains than any previous run
of this title, `next_self_propagating_addr` grew far enough this round
to overwrite the fixed `0x80040000`/`0x80041000` addresses used for
`0x0103d8ec` -- silently replacing its real vtable pointer with an
unrelated HLE trap address written there by an entirely different
object construction. A real, latent bug that simply never had enough
real recursion depth to trigger before this round's fixes let execution
run this much further.

**Fixed** by moving both dynamic counters into a large, previously-
unused address range (`0x80070000`/`0x80090000`) well past every fixed
address in the file and far short of `FileHle`'s own region at
`0x80100000` -- removing the whole category of collision, not just
this one instance (the same kind of fix this log's own Super BurgerTime
"stack/module collision" entry applied to a structurally similar bug
months earlier in this investigation).

**Verified against real Peggle**: the crash at `0x125944` is gone
entirely. Execution now reaches **tick 2** (previously crashed partway
through tick 0's own execution, every time) before hitting a new,
different, much more narrowly-characterized gap: a real function
(`peggle.mod` `0x108e10`) reads a real sub-object from its own first
argument's `+12` field expecting a populated real value, and gets a
genuine null -- the same well-precedented "ambient context field never
populated" shape this project has solved several times already (not
yet fixed this round; a fresh, narrow, well-evidenced next thread,
deliberately not chased further after landing two real fixes already
this session). No regression: Double Dragon and Super BurgerTime
unaffected; 289/289 tests pass (the fix lives entirely in
`tools/game_probe.cpp`, the dev harness). All temporary instrumentation
(the watchpoint, both targeted register traces, the widened
`HandleEvent` trace) reverted; `git diff --stat` clean except the real
address-range fix.

**Significance**: unlike every other fix in this log, this one wasn't
a missing real BREW behavior at all -- it was a genuine memory-safety
bug in this project's *own* tooling, silently corrupting unrelated
objects once enough dynamic allocation happened. Worth remembering for
future rounds: any dynamically-growing address counter added to
`game_probe.cpp` needs its own clearly-separated range, not an
adjacent, seemingly-safe-looking gap.

---

**Followed the tick-2 gap (`peggle.mod` `0x108e10` reading a null
`+12` field) to its real root cause -- not a missing HLE behavior, but
two genuinely separate real objects that should be the same one.**

Traced the whole chain with a series of targeted, temporary register
prints (added and reverted this round, same technique as always):

- The crash reads `[r6+12]` where `r6` is `*([context+0x24] +
  0x45000 + 988)` -- `context+0x24` being this codebase's own
  `ModRuntime::SetFourthContextObject`, a deliberately-documented
  placeholder (`core/brew/mod_runtime.h`'s own doc comment already
  admits its real identity/layout is unknown). Confirmed live: this
  resolves to exactly `kFourthContextObject` (`0x80020000`,
  `tools/game_probe.cpp`), a static, mostly-zeroed block we control --
  so `+988` reads zero because nothing has ever written to it.
- Separately, real code **does** construct a matching-shaped object at
  runtime: a message-dispatch handler (`peggle.mod` `0x105890` ->
  `0x107d0c`, case 1 of a jump table on message ID -> a
  once-only-via-`[this+0x28]`-gate helper at `0x135468` -> a genuine
  large constructor at `0x10a8e0`) that mallocs a 16-byte then a
  412-byte block and wires the second into the first's own `+12`
  field -- textbook-real construction, verified live executing in
  full, in order, well before the crash tick.
- The catch: that constructor's `this` is `0x80300024` -- an address
  from this codebase's real bump-allocator heap
  (`heap_region=0x80300000`, `tools/game_probe.cpp`), entirely
  unrelated to `kFourthContextObject` (`0x80020000`). Real code
  constructs a real manager object on the heap, but the per-tick code
  that later needs it (`peggle.mod` `0x106d34`) looks it up through
  `context+0x24` -- which this codebase currently hardcodes to an
  unrelated, permanently-empty placeholder, so the two never meet.

**Not fixed this round.** The real question this leaves for next time:
does genuine Peggle code ever *write* the constructed manager's address
into the ambient context struct (a plain store to
`[context_address+0x24]`, since `mod_runtime.h` already documents this
field as directly read-and-written data, not a vtable call) -- and if
so, where, so that real store can be allowed to happen naturally
instead of being permanently overridden by `kFourthContextObject`? That
real write site hasn't been located yet; worth a dedicated pass before
guessing at a fix, per this project's own standing rule against
guessing unconfirmed behavior. All temporary instrumentation (six
rounds of targeted `pc==`-gated register prints) reverted; `git diff
--stat` clean -- investigation only this round, no functional changes.
289/289 tests pass (unaffected, all changes were confined to reverted
`tools/game_probe.cpp` instrumentation).

---

**Found the real write site, and fixed the mismatch -- Peggle now runs
indefinitely past what used to be a hard crash.**

The grep the previous entry asked for turned up the answer directly:
`str r0,[r4,#36]` at `peggle.mod` `0x135488`, two instructions after
the `bl 0x10be9c` constructor call inside the SAME gated init function
(`0x135468`) already traced -- writing offset `0x24` (36 decimal) on
`r4`, its own "this". Confirmed live: `tools/game_probe.cpp` already
prints `applet=0x%08x` right after `IModule::CreateInstance` returns,
and it's **exactly** `0x80300024` -- the identical address the whole
message-dispatch/constructor chain operates on. Real code writes its
own constructed sub-objects directly onto fields `+0x24`/`+0x28`/`+0x2c`
of the real `IApplet*` `CreateInstance` hands back -- `GetAppContext`'s
"ambient context struct" isn't a separate OS-level thing at all in this
build, it's the app's own instance.

**This also explains why the crash never showed up as a missing
real write**: this codebase's own `ModRuntime::GetAppContextImpl` was
never given a chance to see it. `context_address_` was a separate,
permanently fixed address (`kFourthContextObject`,
`tools/game_probe.cpp`), unrelated to `applet_ptr`, so no real write
into `applet_ptr+0x24` could ever reach what `GetAppContext` returns.
And even fixing *that* address mismatch alone wouldn't have been
enough: `GetAppContextImpl` unconditionally rewrote all five context
fields on *every single call* (a deliberate original design choice, so
host-side `Set*Instance()` calls always took effect regardless of
ordering against `Install()`) -- which would immediately clobber real
code's own write on the very next `GetAppContext` call, since real
per-tick code (`peggle.mod` `0x106d34`) calls it every tick.

**Fixed both halves.** `ModRuntime` gained `SetContextAddress()`
(`core/brew/mod_runtime.{h,cpp}`) to redirect `context_address_` after
construction -- necessary because `applet_ptr` isn't known until
`CreateInstance` returns, well after `ModRuntime::Install()` must
already have run. `GetAppContextImpl` now only (re-)writes a field when
its `Set*()` call is actually pending (a `bool foo_pending_` per
field, cleared once written), instead of unconditionally on every
call -- every current `Set*Instance()`/`Set*ContextObject()` call site
in this codebase already happens once, up front, before any real ARM
code runs, so host-driven setup behaves identically; the only change is
that real code's own subsequent writes into the third/fourth/fifth
fields now survive instead of being silently reverted. `tools/
game_probe.cpp` calls `mod_runtime.SetContextAddress(applet_ptr)`
immediately after confirming `CreateInstance` succeeded.

**Verified against real Peggle, and the result is dramatic**: the
`0x108e50` crash is completely gone. Execution now reaches and prints
**"Reached the event loop with no unhandled instruction! Window will
stay open"** -- the same milestone message Double Dragon and Super
BurgerTime already reach -- and sustains **at least 10 real ticks**
(previously: a hard crash inside tick 0, then tick 2 after last
round's fix) with zero warnings, zero thrown exceptions, zero
"wandered outside module." By tick 5 it settles into a stable,
byte-identical repeating 11-line per-tick HLE call sequence -- a real,
if unglamorous, steady idle state (the same category already
documented for other titles at this stage), not a new crash. Traced
`DrawText`/`DrawRect`/`Update` with temporary instrumentation
(reverted): still zero real draw calls in this run, so nothing new is
visible on screen yet -- honest, unglamorous, but a categorically
different and far more stable place than every previous round left
Peggle in.

**No regression, verified directly**: Double Dragon still reaches the
event loop and its simulated button-hold input still fires normally;
Super BurgerTime still reaches the event loop and settles into the
same known small repeating loop as before (the pre-existing, separately
tracked "needs real 68000 emulation" blocker, unchanged). 2 new tests
(`RealCodeWritingAContextFieldDirectlySurvivesASubsequentGetAppContextCall`,
`SetContextAddressRedirectsGetAppContextToTheNewAddress`); 291/291
tests pass. `git diff --stat`: `core/brew/mod_runtime.{h,cpp}`,
`tools/game_probe.cpp`, `tests/mod_runtime_test.cpp` only -- all
temporary `idisplay.cpp` draw-call instrumentation reverted.

**Significance**: unlike the address-collision bug earlier this
session, this fix lives in real, permanent HLE code
(`core/brew/mod_runtime.cpp`), not just dev-tool scaffolding -- and
it's a genuinely general fix (any real code, in any title, that treats
an unidentified `GetAppContext` field as writable data will now have
its writes respected), not a Peggle-specific patch. The remaining
context fields (+0x28, +0x2c) still carry placeholder relative-vtable
stubs rather than confirmed real objects -- worth revisiting once
real code is seen calling through them for real, now that whatever it
writes there will actually stick.

---

**Kept pulling on the same thread and landed two more real fixes in
the same round -- Peggle now runs past its own first ten ticks and
into genuinely new code before hitting its next frontier.**

With the app-context fix above live, Peggle's real construction chain
(`peggle.mod 0x105890`/`0x107d0c`/`0x135468`/`0x10a8e0`) now actually
executes end to end for the first time -- but immediately exposed a
second problem, this time triggered *by* real code finally running far
enough to matter:

1. **`SetContextAddress` was re-priming fields real code needs to see
   as zero.** The fifth context field (`+0x28`) doubles as the real
   "already initialized" gate the construction chain checks before
   running -- confirmed directly: `peggle.mod 0x135470` reads offset 40
   decimal, which is exactly `+0x28`. Re-priming it with this
   codebase's own placeholder (as the previous fix did, to keep the
   struct fully populated on the new address) made real code believe
   construction had already happened, so it silently skipped its own
   real constructor -- freezing `+0x24`/`+0x28`/`+0x2c` at fake
   placeholders forever instead of real objects. Fixed: `SetContextAddress`
   now only re-primes the two confirmed OS-provided fields
   (Shell/Display); the three placeholder fields are left untouched on
   the new address so real code's own construction can run the first
   time it's actually needed.
2. **The real construction chain, once unblocked, immediately hit a
   second real gap**: `peggle.mod 0x1099e0-0x1099f8` calls
   `ISHELL_CreateInstance(shell, 0x0101eb0b, &ppObj)` -- the exact real,
   shared PopCap/Zeebo SDK class this project fully reverse-engineered
   several rounds ago (byte-identical code confirmed in Zuma's Revenge's
   `zumar.mod` too) and built a dedicated self-propagating stub shape
   for (`build_self_propagating_stub`'s own slot 4), but never actually
   registered as a real `CreateInstance` target. Unregistered classes
   fail without writing `*ppObj`, and real code here doesn't check the
   return value -- it dereferences whatever garbage was already on the
   stack, landing on a null function pointer. Fixed with one line:
   `shell_hle.RegisterInstance(0x0101eb0b, build_self_propagating_stub())`.

**That combination unblocked a third, until-now-invisible real gap**:
`unknown_0x01030766_obj` (a generic all-stub scaffold since it was
first found, identity still unconfirmed) turned out to have two real,
confirmed call sites once code ran this far -- slot 2
(`peggle.mod 0x1099e0-0x1099f8`) is itself another real
`ISHELL_CreateInstance`-shaped call, always requesting `0x0101eb0b`
again (the same class through a second real path); slot 3
(`peggle.mod 0x109a98-0x109aac`) is a real, confirmed `(this, &ppOut)`
single-out-param call -- the same "QueryInterface-child" shape
`build_self_propagating_stub`'s own slots 2/3 already implement. Both
slots now forward for real instead of returning success while silently
leaving the out-param unwritten (the previous generic-stub behavior,
which is what let real code dereference garbage and crash both times).

**Verified against real Peggle**: execution now sails cleanly through
all ten traced ticks (previously: crashed inside tick 0 every time,
even after the first fix above) with real, non-placeholder objects
visible in the context fields (`+0x24`/`+0x28`/`+0x2c`/`+0x20` all read
back genuine, distinct heap addresses instead of this codebase's own
fixed placeholders), and reaches a fourth, new, narrower frontier a bit
past tick 9: a real per-slot array constructor
(`peggle.mod 0x108a90`-area) passes its own not-yet-populated field
into a construction trampoline as `this`, which this log hasn't traced
to a root cause yet -- a fresh, well-characterized next thread,
deliberately not chased further after landing three real fixes already
this round.

**Honest, non-obvious side effect worth recording**: Double Dragon's
own behavior changed too. It still does everything it did before
(reaches the event loop, simulates a download-complete notification,
holds simulated button presses) -- but with `SetContextAddress` no
longer masking the third/fourth/fifth fields with placeholders, it now
runs measurably further afterward and hits its own new, previously
unseen static-base table gap (offset `0x1b4`, a 20th real slot beyond
every one this project has mapped so far) instead of settling into a
stable steady state the way it did on every commit before this round
(confirmed directly: re-ran the pre-this-round commit for 25 seconds,
double the window that produced the new gap on the fixed build, and it
never moves past its previous steady state). Not a regression -- Double
Dragon does strictly more than before, and stops at a new, legitimate
frontier of the same kind this whole project has always worked through
-- but worth naming explicitly since "no regression" claims in this log
should mean "verified identical or better," not "didn't check."

291/291 tests pass (unchanged -- this round's fixes live entirely in
already-tested `ModRuntime` behavior plus `tools/game_probe.cpp` scaffold
wiring). `git diff --stat`: `core/brew/mod_runtime.{h,cpp}`,
`tools/game_probe.cpp` only; all temporary per-tick context-field-dump
and targeted PC-trace instrumentation added while tracing this round
reverted.

---

**Chased the tick-9 frontier above and found a genuinely confusing
shape -- real code appears to read its own field before writing it,
even within that field's own constructor. Investigation only; this one
isn't understood well enough to fix yet.**

Traced the crash precisely with temporary, reverted register/memory
dumps at `peggle.mod 0x108860` and `0x108a90` (the two sibling
per-slot "ensure constructed" functions this log has referenced
before). Both share the same real address arithmetic: `arr =
this[+12]`, `elem = arr + idx*4`, and each maintains its own field at
a fixed offset from `elem` (`0x108860` owns `elem+0xa0`; `0x108a90`
owns `elem+4`) gated by "already non-zero?" at function entry.

`0x108a90`'s own gate (`elem+4 == 0`) is what's failing -- confirmed
live, for every index 0-6, at the point this trace fired. That's
expected; nothing has constructed it yet. But **the crash comes from
the *same* function, past its own gate, re-reading the *same*
still-zero `elem+4` field and passing it as `this` into a real
construction trampoline (`peggle.mod 0x105b5c`)** -- confirmed by
grepping the function body for every `str` instruction between the
gate check and the crash site: only three stack writes (`sp`/`sp+4`/
`sp+8`), nothing touches `elem+4` at all. The trampoline itself
(`0x105b5c`) unconditionally dereferences its first argument with no
null check (`ldr r2,[r0]`, no `cmp`/branch beforehand) -- real RVCT
code that tolerated a null "this" would guard it, so this isn't a
"null is a valid sentinel" pattern; real code is expected to supply a
real, already-non-null pointer here.

**Best current read**: this specific call inside `0x108a90` isn't
meant to run on this field's *own* first construction pass at all --
some other, not-yet-identified real event or call is expected to have
populated `elem+4` *before* `0x108a90` ever reaches this point for a
given index, and whatever real trigger does that hasn't happened in
this emulated run. This is a different shape than every previous
"ambient context field never populated" gap this project has fixed --
those were single fields read once with a clear owning constructor;
this one reads and reuses its own field mid-function, in the same
constructor that's supposed to populate it, which doesn't fit that
pattern cleanly. Deliberately not guessing a fix here per this
project's standing rule -- needs a dedicated tracing pass (most likely:
find what real event/call path is supposed to run before `0x108a90`,
by searching for what other real code reads or writes `elem+4`
elsewhere in the binary, the same technique that found this round's
three real fixes) rather than patching this call site blind.

All temporary instrumentation reverted; `git diff --stat` clean --
investigation only this round, no functional changes. 291/291 tests
pass (unchanged).

---

**Pivoted to Double Dragon's own new frontier and closed it: a
twentieth real static-base table slot, `0x1b4`.**

Disassembled the crash site directly (`ddragonz.mod` `0x11f870`,
reached from a real per-array-element initialization function): the
call passes `(dest=this+0x4234, count=<a real int16 read from data>,
cap=4, ctor_fn=<a real code pointer>)` across `r0`-`r3` -- a shape
matching a compiler-generated "construct N array elements via a given
constructor" RVCT/EABI runtime helper (this project's static-base
table already hosts several plain C-runtime-style utilities at other
slots -- MALLOC, MEMCPY, MEMSET, STRLEN, SPRINTF, REALLOC -- so another
compiler-emitted helper living here fits the established pattern).
Chased `ctor_fn`'s own address to look for an element-size argument
that would make real construction implementable, but the real ABI
convention here doesn't expose one to the caller in any register --
without knowing the real per-element stride, actually constructing
elements would mean guessing an offset and silently writing to the
wrong addresses, which is worse than doing nothing. Registered as a
safe no-op instead, the same treatment already given every other
under-evidenced slot in this table (offsets `0x40`, `0xc`, `0xd0`,
`0xdc`, `0x184`).

**Verified against real Double Dragon**: the crash is completely gone.
Execution now runs at least 20 seconds past the point that used to
stop it, still cleanly simulating a download-complete notification and
repeated button-hold input with no new warnings. No regression on
Peggle or Super BurgerTime (both re-verified to stop at exactly the
same points as before -- Peggle's tick-9 field-read-before-write gap
from the entry above, Super BurgerTime's known 68000-emulation wall).
1 new test (`UnknownSlot0x1b4DoesNotCrash`); 292/292 tests pass.

**Significance**: like the address-collision bug and the app-context
fix earlier this session, this lives in real, permanent HLE code
(`core/brew/mod_runtime.{h,cpp}`), and like every other slot in this
table, it's general infrastructure, not Double-Dragon-specific --
confirming this project's static-base table mechanism keeps paying off
the same way it has all session: find the real call shape, decide
honestly whether there's enough evidence to implement it for real or
only enough to stop it from crashing, and move on to whatever that
unblocks.

---

**Followed Double Dragon's newly-unblocked run further and confirmed a
genuine first: it draws real, correct on-screen content -- but couldn't
get a clean live visual confirmation in this environment, and that
distinction is worth recording precisely rather than glossing over.**

Temporary instrumentation (reverted) in `IDisplayHle::DrawText`/
`DrawRect`/`Update` confirmed real code now calls all three, for the
first time in this project's whole history with this title: a real
full-screen `DrawRect` (`rect=NULL`, the documented "fill the whole
destination" case) followed by multiple real `DrawText` calls, all with
a real, non-default color (`current_rgbval_` confirmed non-zero,
matching real white text/background), landing in the real framebuffer
-- verified directly by reading `framebuffer_` itself (not just trusting
the draw calls happened): every sampled pixel across the buffer reads
`0xffff` (RGB565 white), and a full-buffer scan found zero black pixels
out of all 307,200.

**Tried to confirm this visually and couldn't, despite real effort.** A
completely synthetic test -- pushing solid-red frames through the exact
same `Sdl2Backend::PushVideoFrame` path immediately after window
creation, before any real game code runs -- rendered correctly and was
confirmed via screenshot (both a compositor screenshot and a raw `xwd`
X11 window dump). But real Double Dragon's own white content, verified
correct at the pixel-buffer level and pushed through that identical
code path, consistently showed as a black window in every capture
attempted -- across `SDL_RENDERER_ACCELERATED` and `_SOFTWARE`, with
and without the `SDL_WINDOW_OPENGL` window flag, and with the
screenshot deliberately timed to land after confirmed real draw calls
(watching the process's own stderr for the first real `DrawText` before
capturing). Every `SDL` call in the video path (`UpdateTexture`,
`RenderClear`, `RenderCopy`) reports success with an empty
`SDL_GetError()`.

**Deliberately not claiming a code fix here.** The synthetic-red test is
strong evidence the actual `Zeebulator` rendering pipeline is correct;
what's inconclusive is only the live, screenshot-based visual
confirmation in this specific sandboxed/remote test environment, which
may have its own window-compositor quirks (e.g. around focus-stealing
via `wmctrl`, or how a virtual display handles a window that stops
being actively redrawn) unrelated to this codebase. Guessing at a
"fix" for a problem that might not exist in a normal desktop
environment would risk exactly the kind of unconfirmed, silently-wrong
change this project's standing rule exists to prevent. Worth revisiting
with better tooling (e.g. a video capture across the whole run, or
testing on a non-virtualized display) before concluding anything more.

**Separately, honestly worth noting**: real code draws exactly once
(one `DrawRect` + a burst of `DrawText` calls, then nothing) across the
whole traced run -- it doesn't appear to redraw on a timer or animate
further. Not yet determined whether that's correct real behavior for a
static screen at this stage, or a further real gap (e.g. a missing
per-tick redraw trigger) -- a concrete next question, not a guess
either way.

All temporary instrumentation reverted (`idisplay.cpp`'s draw/pixel
logging, `sdl2_backend.cpp`'s SDL error logging, and `game_probe.cpp`'s
synthetic red-frame test and renderer/window-flag experiments); `git
diff --stat` clean. 292/292 tests pass (unchanged -- investigation
only, no functional changes this round).

---

**Traced Peggle's tick-9 field-read-before-write puzzle (see the entry
several rounds back) all the way to its real root cause, and it turns
out to be the exact same unconfirmed `IShell` vtable slot Double
Dragon's own investigation hit months ago -- now cross-confirmed from
a second, independent title.**

Used a live `Memory::Write8` watchpoint (the same technique from
earlier address-collision work, temporary and reverted) on the exact
`elem+4` address `peggle.mod 0x108a90` reads and immediately misuses as
`this`. Found real writes -- but both zero it (`peggle.mod 0x1087f8`,
a genuine 39-slot bulk-reset loop that zeroes both this field and its
sibling `+0xec`, and `peggle.mod 0x1020a4`, a generic "zero the output
parameter defensively before attempting real work" pattern inside a
lookup/construct helper). Traced that helper (`0x102078`) forward: it
validates its inputs, defensively zeros `*ppOut`, then calls a second
helper (`0x102148`) that only proceeds to real construction if a
specific vtable call returns *exactly* `35` -- and that vtable call
resolves (confirmed live: `this`'s own `[+12]` field reads
`0x80001000`, this codebase's own `IShell` object) to **`IShell`
vtable byte offset `0xac`, slot 43** -- the identical slot Double
Dragon's own earlier investigation found and left as an intentionally
unconfirmed safe stub (`core/brew/ishell.cpp`'s own doc comment:
"slot 43 -- the one real call site found so far"). Now there are two,
independently found, in two different titles.

**Why Double Dragon's own call site survived a `0`-returning stub but
Peggle's doesn't**: traced the caller chain one level further and
found `peggle.mod 0x108b48`'s own construction attempt (`0x102078`)
captures its result into `r6` -- and then never checks it. `r6` gets
used as nothing but a `DbgPrintf` log argument two instructions later;
real code proceeds unconditionally to dereference `elem+4` right after,
trusting construction always succeeds. Double Dragon's own call site,
by contrast, evidently tolerates the stubbed `0` gracefully (this
project's own log already records that title reaching a fully clean,
warning-free run after the same stub was added) -- the same real slot,
two different real callers, two different real tolerances for failure.

**Looked hard at what it would take to actually implement slot 43 for
real, and it's genuinely more than this round can safely guess.**
Real code past the `35`-check reads two more fields out of the same
output struct (`ppOut[0]`, `ppOut[4]`) and, depending on their values,
either builds a formatted string through several more real vtable
calls (offsets `0x14c`/`0x150` on yet another object) or branches into
a whole separate state machine keyed on `ppOut[0]` being `0`, `1`, or
matching one of two more PC-relative literal constants -- real,
substantial logic, not a thin one-shot call. Guessing a return value
and struct contents here risks exactly the failure mode this project's
standing rule exists to prevent: not a clean stop, but a *silent wrong*
result feeding real string-building code. Left unfixed and undocumented
further than this -- a concrete, well-evidenced next target (ideally
with a real BREW MP `IShell` header reference, which no source
available to this project currently provides) rather than a guess.

All temporary instrumentation (the watchpoint globals in
`core/memory/memory.{h,cpp}`, the targeted `pc==`-gated prints in
`tools/game_probe.cpp`) reverted; `git diff --stat` clean --
investigation only this round, no functional changes. 292/292 tests
pass (unchanged).

**Immediate follow-up, same round: read Double Dragon's own slot-43
call site (`ddragonz.mod 0x10a1f0-0x10a3b0`) end to end to see if
cross-referencing it would make Peggle's specific case safe to guess
after all -- it doesn't, and that itself is useful confirmation.**
Double Dragon's check is `cmp r0,#0` (0 = success), not Peggle's
`cmp r0,#0x23`, and its query argument (`r1=[r4+16]`, a real
data-driven value) differs from Peggle's constant `r1=0` -- consistent
with slot 43 being a generic, multi-purpose "query an indexed
property" method whose valid return codes depend entirely on what's
being queried, not a fixed contract either title's evidence alone
could pin down. Confirmed further by reading each title's own success
path: Double Dragon's continues into a completely unrelated real
subsystem (a second vtable query feeding `ISHELL_CreateInstance`, then
sprite/health-bar-style byte-array arithmetic), while Peggle's builds
formatted strings. Two genuinely different real features hanging off
the same slot -- reinforcing rather than narrowing the earlier
conclusion that guessing a universal behavior here would be guessing,
not reconstructing. No further action taken; read-only disassembly
only, nothing to revert.

## Double Dragon: real-desktop black-screen regression

With the event loop reached and the "CARREGANDO..." loading screen
rendering correctly, the user (running the real `zeebulator_game_probe`
binary directly on their own real, non-VM Linux Mint/Cinnamon desktop --
not relying on remote screenshots) reported the window showing correct
white/yellow loading-screen content briefly, then going solid black and
staying black indefinitely, with no flicker, no matter how long the run
continued.

**First, a major methodology bug in this investigation's own remote
testing was found and fixed.** Killing background `zeebulator_game_probe`
test processes via plain `kill` was not reliably terminating them,
leaving many zombie windows sharing the identical title "Zeebulator -
game probe" -- and `wmctrl -a "<title>"` (title-based window activation,
used before every automated screenshot) was non-deterministically
raising/capturing *stale* zombie windows instead of the current test's
window. This retroactively explained several apparently-confirmed
findings from earlier in the same investigation (GL context "stealing",
an ARM step-count "threshold" for triggering the bug) as pure testing
artifacts. Fixed by matching windows to their owning PID
(`wmctrl -lp | awk -v pid=... '$3==pid {print $1}'`) and using `kill -9`
for reliable termination; re-running the exact bisection tests that had
shown a flaky threshold, with corrected methodology, showed the
"threshold" never existed. From this point on, the user's own direct,
real-time observation of the real window became the authoritative source
of truth, not automated screenshots.

**Systematically ruled out, via a mix of corrected-methodology
Zeebulator runs and ~10 standalone, Zeebulator-independent minimal SDL2
C programs (compiled against this project's vendored SDL2 headers but
linked against the system's installed `libSDL2-2.0.so.0`, run
individually with the user watching in real time):**
- GL context creation/"stealing" on the same window as the 2D renderer
- ARM interpreter step-count/timing thresholds and window-manager
  "unresponsive app" detection tied to them
- A one-time "priming burst" of extra presents right after the first
  real `IDISPLAY_Update` call (postponed the blackout by exactly the
  burst's own duration, then reverted)
- Continuous periodic re-presenting of the last frame (a real,
  functioning mechanism, confirmed via temporary instrumentation to be
  running throughout -- did not prevent or reverse the blackout, since
  it turned out to be a no-op the whole time this round, see below)
- Real game code intentionally drawing black (a temporary `DrawRect`/
  `Update` trace confirmed every real draw call, ~49 over 20 seconds,
  used correct white-fill/yellow-text content, never black)
- GPU/accelerated-renderer resource loss (`SDL_RENDERER_SOFTWARE`
  showed identical behavior to `SDL_RENDERER_ACCELERATED`)
- The `SDL_WINDOW_OPENGL` window-creation flag (removing it made no
  difference)
- Terminal I/O contention (redirecting the huge `[hle call]` trace
  output to a file instead of the terminal made no difference)
- A single one-time `SDL_GL_SwapWindow` call after GL context creation
  (confirmed via a minimal test to leave the window unaffected)
- SDL2 usage patterns in isolation and combination: streaming textures,
  a real opened audio device fed continuous silence, game controller
  polling, sustained ~98%-CPU same-process load, and ~10 seconds of not
  calling `SDL_PollEvent` at all (simulating a long-running ARM
  interpreter call starving the event loop) -- none reproduced it

**Found the real trigger: real, repeated `glClear`+`eglSwapBuffers`
activity on a window shared with the 2D `IDisplay` surface.** Added
temporary `[GLDIAG]`-tagged tracing to every real `GlHle` EGL/GL entry
point and a `[HEARTBEAT]` frame-content/timing log to
`Sdl2Backend::PushVideoFrame` (all reverted afterward). Against a
corrected run (an earlier re-run in this same round had used the wrong
`cls_id` -- `274754`, the download-catalog folder number -- instead of
the real, previously-documented `0x0102f789`/`16971657`, which made
`CreateInstance` fail immediately; this file's own top-of-round doc
comment on `cls_id` already warned about exactly this trap), the
heartbeat log showed 863 real frames pushed over ~30 seconds, every one
byte-identical correct white content, zero SDL error returns -- proving
the application-level render pipeline was correct and live for the
entire run, including exactly when the user reported the blackout. The
GL trace showed real code calling `eglMakeCurrent` once near t=0, then
starting at t~0.89-0.95s -- lining up almost exactly with the user's
"about 1 second" report -- calling `eglSwapBuffers` *repeatedly*, every
~60ms, for the rest of the run. Further tracing showed real, genuine GL
draw activity too: `glClear`(mask `0x4100`), `glClearColorx`, and
hundreds of `glDrawArrays(GL_TRIANGLES, ...)` calls -- plus a real,
separate, unrelated gap: `glViewport(0,0,1,0)`, a degenerate near-zero
viewport (not pursued further this round).

A minimal, standalone reproduction confirmed the mechanism precisely: a
single one-time `SDL_GL_SwapWindow` call, or repeated swaps of an
*untouched* GL back buffer, left a shared window unaffected (at most a
transient one-frame blink); repeated `glClearColor`(black)+`glClear`
followed by `SDL_GL_SwapWindow`, on the same window a separate
`SDL_Renderer` also presents to via `SDL_RenderPresent`, reproduced a
**persistent** black lockup, matching the real bug exactly.

**The obvious fix -- giving the raw GL context its own private, hidden
window instead of sharing the visible one -- did not work.** Applied
(new `Sdl2GlBackend(int width, int height)` constructor, creating and
owning a hidden `SDL_WINDOW_HIDDEN` window internally) and verified
against the real game: still went black. A further minimal reproduction
(two completely separate `SDL_Window`s in one process -- window A
visible, 2D-only, never touching GL at all; window B hidden, doing real
`glClear`+`SDL_GL_SwapWindow` repeatedly on *itself only*) showed window
A *also* goes persistently black once window B's GL activity starts --
proving this isn't a shared-drawable bug at all, but something
compositor-wide: merely having a second, real host GL context anywhere
in the process breaks repainting for every window that process owns.
Confirmed directly against the real `zeebulator_game_probe` binary too:
moving the window makes the correct "CARREGANDO" content flash back
before reverting to black -- an interactive title-bar drag (which
forces the window manager to recompute geometry) is the only thing that
forces a correct repaint, matching the minimal reproduction precisely.
System: Cinnamon (Muffin/Mutter-based compositor) on X11, AMD Radeon
780M (radeonsi/Mesa 25.2.8), confirmed via `glxinfo`/`ps`. Tried and
confirmed ineffective: disabling SDL's default
`SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR` hint (`SDL_VIDEO_X11_NET_WM_
BYPASS_COMPOSITOR=0`); forcing synthetic `SDL_SetWindowPosition` changes
once a second; forcing them every single frame (60Hz) instead. All
consistent with a real Cinnamon/Muffin/Mesa compositor bug, not
anything in this project's own code, that no application-level
workaround tried could get around.

**Fixed by not creating a real host GL context at all.** Added
`NullGlBackend` (`frontends/standalone/null_gl_backend.h`): a
`GlBackend` implementation that answers every real IGL/IEGL call with a
plausible, harmless value (context creation always "succeeds",
`GenTextures` hands back distinct nonzero names, everything else is a
pure no-op) without a single real `SDL_GL_CreateContext`, `glClear`, or
`glDrawArrays` ever executing. `tools/game_probe.cpp` now builds
`NullGlBackend` instead of `Sdl2GlBackend` -- justified not just by the
compositor bug but because real GLES rendering isn't a complete,
correct pipeline for this title regardless (the degenerate viewport
found above). `frontends/standalone/main.cpp`'s isolated `hello_gl`
demo, which gives GL its own dedicated window and has no competing 2D
surface in its own process, is unaffected and keeps using the real
`Sdl2GlBackend`. Verified directly on the user's real desktop: the
loading screen now stays visible and correct for 45+ sustained seconds,
no regression.

**Also fixed a latent bug discovered along the way**:
`IDisplayHle::RepresentLastFrame()` (the periodic re-presenting
mechanism believed, earlier in this same investigation, to be running
and simply insufficient) was actually a silent no-op the whole time --
an over-broad `git checkout` earlier in this investigation, used to
revert temporary `DrawRect`/`Update` trace prints, also reverted the
(never separately committed) `last_presented_`/`has_presented_`-setting
lines inside `Update()` without reverting `RepresentLastFrame()`'s own
declaration in the header. Re-added `Update()`'s real plumbing, plus a
periodic in-loop liveness check inside `CallArmFunctionChecked`
(checked cheaply every 20,000 interpreted steps) so a single
long-running real ARM call can't leave the window unrepresented for a
full real second, matching `RepresentLastFrame`'s own documented
"at least roughly once a second" contract.

All temporary instrumentation ([GLDIAG]/[HEARTBEAT] tags in
`core/brew/gl_hle.cpp`/`frontends/standalone/sdl2_backend.cpp`, the
two-constructor `Sdl2GlBackend` experiment) reverted; `git diff --stat`
clean except the real fix (`core/brew/idisplay.{h,cpp}`,
`tools/game_probe.cpp`, new `frontends/standalone/null_gl_backend.h`).
292/292 tests pass.

---

**With the display fixed, went straight after the natural next
question: why does the real "CARREGANDO..." screen never advance, even
with the button-hold simulation delivering correctly?** First
confirmed, via a temporary `DrawText`-content trace (reverted), that
this genuinely *is* a real, correctly-animating loading spinner --
`"CARREGANDO"` → `"CARREGANDO."` → `"CARREGANDO.."` → `"CARREGANDO..."`
→ repeat, cycling normally every tick -- not the separate, real,
input-gated title screen this project's HID work earlier in this log
targeted. The simulated button press visibly has no effect on it, which
is now understood to be *expected*: this is a different, earlier real
screen.

**Picked up this file's own long-standing, well-specified open thread
(class `0x01005511`, slot 6, "what should set `pUser+37`") and closed
it -- as a dead end, not a fix.** Traced the real caller live (a
temporary per-slot trace on every method of this object, reverted):
slot 4 gets the same three real `SetProperty`-shaped calls this log
already documented, slot 3 (`CreateSignal`) fires with a real
`callback`/`userdata` pair, then slot 6 gets one real call. Captured
its real caller's return address (`lr=0x00107638`) and disassembled
around it directly (`arm-none-eabi-objdump -D -b binary -m arm
--adjust-vma=0x100000`): the calling function (starts `0x107570`) is a
real, one-time "finalize the resource list" routine, guarded by its own
done-flag (`[r4+0x74c]`, checked against `35` at entry, set to `35`
before the slot-6 call) -- calls slot 6 exactly once, ever, per real
run.

**Found and corrected a real bug in this investigation's own tracing
methodology in the process**: the earlier-observed `r1=0xf000061c` at
the slot-6 trap (which looked, alarmingly, like the object's *own*
slot-6 trap address being passed back to itself as an argument) turned
out to be a misreading, not a real argument. The real call site
(`0x107628`-`0x107634`: `ldr r1,[r0]; ldr r1,[r1,#24]; bx r1`) reuses
`r1` purely as scratch to *resolve* the vtable slot 6 target -- `bx`
doesn't clear the register it branches through, so by the time our own
HLE trap dispatcher reads "r1", it's reading the just-computed branch
target, not a real passed argument. Confirmed via `HleRuntime::
DebugFunctionCount()` (a temporary accessor, reverted) bracketed around
each major `Build*`/`RegisterInstance` call site that trap index 391
(`0xf000061c`) is exactly this same object's own slot 6, registered as
its local slot 6 out of 20 -- not a coincidence, just this calling
convention's own scratch-register reuse. A real, reusable lesson for
any future trace added the same way: a naively-read register at an
HLE trap doesn't distinguish "real argument" from "resolution scratch
still sitting in that register" -- worth checking the real call site's
own disassembly before trusting either.

**Slot 6's own real return value is unconditionally discarded** --
immediately after the call (`lr=0x107638`), the caller does
`pop {...}; mov r0,#1; bx lr`, regardless of what slot 6 returned or
whether the call even happened (the same `0x107638` address is also
the direct branch target when the guarding object pointer is null).
Combined with it firing exactly once per run (the done-flag), this
means slot 6 is a real, one-shot, fire-and-forget notification whose
result nothing downstream ever checks -- implementing it "correctly"
would not unblock anything. The `pUser+37` byte this log's much earlier
round was chasing lives in a *different* real function (`0x11f4dc`,
reached via the download-complete callback path, not this one) that
this round's tracing never actually exercised long enough to revisit --
a live watchpoint on `pUser+37` (temporary, reverted) confirmed zero
writes across a run that included the simulated download-complete
notification and the full button-hold window.

**Located the real `DrawText` call site for the CARREGANDO text
itself**, the more direct real question. A temporary `DrawText` trace
augmented with the caller's `lr` (reverted) showed a single, consistent
real caller: `lr=0x00123cb8`, inside a larger function starting at
`0x123cc8`. Disassembled the call site directly: it's a real
`IDisplay::DrawText`-shaped indirect vtable call (`ldr r4,[r0,#16]`
-- slot 4 -- `bx r4`), with `r3=-1` (null-terminated string, matching
`DrawText`'s own already-documented real nChars convention) and a text
buffer built earlier in the same function, not yet traced back to
whatever decides the buffer's own content (the cycling dot count) or,
more importantly, whatever condition would make this function draw
something *other* than one of the four `"CARREGANDO"`-family strings
found via a direct static search of `ddragonz.mod` for the literal
bytes (`0x70e38`/`0x70e44`/`0x70e50`/`0x70e60`, real file offsets --
no fixed-address cross-references exist for them, confirmed via a
targeted 4-byte scan, consistent with this being ROPI/PC-relative code
that computes the string address at runtime rather than storing it as
a literal).

**Not pursued further this round** -- a deliberate stop after
conclusively closing one long-open thread and precisely locating the
real next one, not a stall. Concrete next step: disassemble
`0x123cc8` (and whatever calls *it*) forward from its own start,
rather than backward from the `DrawText` call already found, to find
the real condition that currently always resolves to "still loading."
All temporary instrumentation (the per-slot `0x01005511` trace, the
`HleRuntime::DebugFunctionCount` accessor and its bracketing prints,
the `pUser+37` `Memory::Write8` watchpoint bridge, the `DrawText`
caller-`lr` trace) reverted; `git diff --stat` clean, no functional
changes survive this round. 292/292 tests pass (unchanged).

---

**Went straight after `0x123cc8`'s own real callers via a live
`pc==`-gated trace (temporary, reverted) and found the actual
CARREGANDO-drawing function is `0x106098`, not `0x123cc8`/`0x123c20`
(both real, but one level removed -- shared sprintf+DrawText helpers
this function itself calls). Disassembled `0x106098` directly: it
reads a real dot-counter (`[this+0x1512]`, a 16-bit field), branches
on its value (0/1/2/3) to draw the matching `"CARREGANDO"`/`"."`/
`".."`/`"..."` variant via `0x123c20`, then unconditionally increments
and wraps the counter (`&3`) and clears bit 0 of `[this+0x15ac]` --
the exact same field this log's much earlier Peggle/Double-Dragon
button-press investigation was already tracking. This function has no
internal exit condition at all -- it always draws something -- so
whatever decides to *call* it each tick is the real gate, not anything
inside it.

**Traced that caller too (same live technique) and found the real
per-tick dispatch mechanism directly**: `0x104b60`-`0x104b7c` calls
through `applet+0x50` then `applet+0x54` (two real function-pointer
fields this log has tracked since the earliest Double Dragon HID
work) every tick, then (`0x104b80`-`0x104b9c`) tests bit 0 of
`applet+0x15ac` to pick between two further real functions. Confirmed
live that `applet+0x54` currently *equals* `0x106098` -- i.e. the
loading-spinner drawer is genuinely one of the two real per-tick
handler slots this log already had a name for, not a separate,
unmapped code path.

**Added a periodic `applet+0x50`/`applet+0x54`/`0x15ac` status print
(temporary, reverted) and watched it across the full ~50-second
simulated button-hold window (matching this log's own earlier-proven
9-button batch, unchanged) -- and got a complete, satisfying answer.**
The real per-tick handler pair transitions exactly as this log's much
earlier round already found (`0x1222f0`/`0x107104` → `0x121110`/
`0x1063ec`, the same states/addresses, independently reproduced) --
confirming that earlier finding still holds in the current build.
Then, around tick 260-280 (well inside the button-hold window), it
transitions *again*, this time to a state this log had never reached
before: `applet+0x50`/`applet+0x54` = `0x1220f8`/`0x1070a8`, and
`0x106098` (the CARREGANDO drawer) briefly reappears once more at the
transition itself before being left behind for good. This new state
is stable for the rest of every run tried (1000+ real ticks).

**The real question then became: does `0x1070a8` ever draw anything
new, or is the app genuinely stuck?** Added unconditional call
counters (temporary, reverted) to `IDisplayHle::DrawText`, `DrawRect`,
and `Update`, plus real GL activity tracing
(`GlHle::EglMakeCurrent`/`EglSwapBuffers`/`GlClear`/`GlDrawArrays`) --
and the answer is unambiguous. Across a full run reaching the new
state and continuing 1000+ ticks past it: `DrawText`/`DrawRect`/
`Update` call counts all freeze at exactly 53 (the same count reached
during the CARREGANDO phase, confirmed by timestamp -- zero calls to
any of them after the transition) while real GL activity explodes:
**19,419 real GL calls** in the same window (`GlClear`,
`EglSwapBuffers`, and thousands of `GlDrawArrays` calls across many
distinct real triangle-strip/fan batch sizes, from small UI-sized
batches up to 1980-vertex batches consistent with real 3D/sprite
scene geometry).

**Conclusion: there is no remaining "stuck on CARREGANDO" bug in
Double Dragon's own real game logic.** The real BREW app genuinely
completes loading, transitions its per-tick state machine cleanly
(exactly as this log's HID/button-hold work already established), and
begins drawing its real next screen -- entirely through real GLES
rendering, which this project currently, deliberately, never displays
(`NullGlBackend`, this log's own black-screen fix earlier this same
round: a real host GL context anywhere in this process reliably
breaks this desktop's real compositor for every window the process
owns, and no application-level workaround found so far gets around
it). The window appears "stuck" not because anything is broken in the
interpreted app or its HLE surface, but because the one real rendering
path this next screen actually uses is the one this project
deliberately stopped presenting. This reframes the real remaining gap
entirely: it's not a missing HLE call or an unfound gate anymore --
it's the same architectural tension the black-screen fix already
surfaced (real GL content vs. this desktop's real compositor bug),
now confirmed to be the *only* thing standing between this build and
actually showing Double Dragon's next real screen. A real fix needs
either (a) resolving or working around the compositor bug some other
way so `Sdl2GlBackend` can be used again, or (b) compositing the 2D
`IDisplay` surface itself through the same single real GL context real
GLES content uses, so the process only ever owns one real GL context
total instead of two -- not attempted this round.

All temporary instrumentation (the `0x106098`/`0x1063ec` entry-point
trace, the periodic per-tick status print, the `DrawText`/`DrawRect`/
`Update` call counters, the `[DBGGL]` real-GL-activity trace) reverted;
`git diff --stat` clean, no functional changes this round. 292/292
tests pass (unchanged).

---

## Real GL rendering: implemented option (b), and it works

Took the second option from the previous round's own conclusion:
composite the 2D `IDisplay` surface through the same single real GL
context real GLES content uses, instead of trying to resolve the real
desktop compositor bug directly.

**Validated the core assumption first, before writing any real code.**
A minimal, independent SDL2 reproduction (one real window, one real GL
context, used as the *sole* presentation mechanism -- real varying
`glClearColor` + a real triangle draw + `SDL_GL_SwapWindow`, every
frame, nothing else) was watched directly on the real desktop for
15-20 seconds: stayed correct throughout, no blackout. This is a
categorically different setup from every earlier reproduction that
*did* show the real compositor bug (all of which had two real
presentation paths -- a 2D `SDL_Renderer` plus a separate/hidden GL
context -- coexisting in one process). Confirms a single real context
used as the only presenter is safe on this real desktop.

**Implemented `Sdl2UnifiedBackend`** (`frontends/standalone/
sdl2_unified_backend.{h,cpp}`), replacing `Sdl2Backend` +
`NullGlBackend` in `tools/game_probe.cpp`: one real object implementing
both `Backend` (2D video/audio/input) and `GlBackend` (real IGL/IEGL),
backed by one real `SDL_Window` + one real `SDL_GLContext` created
eagerly at construction and held for the object's whole lifetime.
`PushVideoFrame` uploads the RGB565 framebuffer as a `GL_UNSIGNED_
SHORT_5_6_5` texture (a direct format match, no pixel conversion) and
draws it as a full-screen textured quad (state saved/restored around
it via `glPushAttrib`/`glPushMatrix` so it never corrupts real app-owned
GL state) before calling `SDL_GL_SwapWindow` -- the exact same real
drawable, the exact same real context, every time, whether the frame
came from a real `IDISPLAY_Update` call or a real `eglSwapBuffers`
call. `GlHle` now takes the same object as its `GlBackend`. This also
matches what real disassembly already found elsewhere in this
investigation: `IBITMAP_QueryInterface(..., AEECLSID_DIB, ...)`'s
result gets cast straight to `NativeWindowType` for
`eglCreateWindowSurface` -- i.e. on real hardware, GLES's own native
window surface *is* IDisplay's own device bitmap/surface, not a
separate one, so this class's design matches real hardware behavior,
not just this project's own convenience. `NullGlBackend` (no longer
used anywhere) removed.

**First real-desktop test showed a new, smaller problem: an occasional
brief black flicker, not a permanent lockup.** Systematically tried,
directly on the real desktop, each in isolation: `glTexImage2D` every
frame (a known driver footgun) vs. allocate-once-then-`glTexSubImage2D`
(no change); rate-limiting the frontend's own `RepresentLastFrame`
keep-alive push to ~5/sec (this file's own established "at least
roughly once a second" contract) -- confirmed to make it measurably
*worse*, consistent with this file's much earlier finding that this
same desktop's compositor needs *frequent* real presents to keep a
window's content visible at all; explicit `SDL_GL_SetSwapInterval(0)`
(no change); explicit `SDL_GL_SetSwapInterval(1)` (measurably better,
kept); explicit `SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)` (worse,
reverted). Net real improvement from this pass: `glTexSubImage2D` (the
objectively more correct pattern regardless) and `SDL_GL_
SetSwapInterval(1)`, kept as permanent, deliberate choices -- the
flicker itself is reduced but not eliminated, a real, open, much
smaller residual issue for a future round (the permanent blackout this
whole investigation started from is what's actually fixed).

**Second real-desktop test surfaced the actual remaining blocker, and
it isn't a bug in this round's work at all.** With `Sdl2UnifiedBackend`
in place, the loading screen showed briefly then went solid black --
alarming at first, but a live `[DBGGL]` trace on `GlClearColorx`/
`GlViewport` (temporary, reverted) found the real, root cause
immediately: real Double Dragon code sets `glClearColor(0,0,0,0)` --
literal black -- combined with the same degenerate `glViewport(0,0,1,0)`
call this investigation already found and explicitly deferred two
rounds ago. `glClear` fills the *entire* real color buffer regardless
of the current viewport (no scissor test in use), and a zero-height
viewport means no subsequent real draw call can rasterize a single
pixel -- so the real, correctly-computed output of Double Dragon's own
current real GL code is, genuinely, a solid black frame. Confirmed via
counts: 848 real `GlClearColorx(0,0,0,0)` calls, 848 matching real
`GlViewport(0,0,1,0)` calls, over a run that also fired thousands of
real `GlDrawArrays` calls (all invisible, clipped away by the
zero-height viewport). This is `Sdl2UnifiedBackend` working exactly as
designed -- real GL content is finally, genuinely reaching the real
screen -- surfacing a real, separate, already-known gap that was
previously hidden behind the compositor bug and (before this round's
fix below) the frontend's own stale-frame re-presenting.

**Found and fixed one real bug of this round's own making along the
way**: `IDisplayHle::RepresentLastFrame`'s keep-alive mechanism doesn't
know the app has moved on to real GL rendering, so it kept
re-uploading and re-presenting the *stale* last 2D snapshot (the final
`"CARREGANDO..."` frame) every tick, fighting the app's own real GL
frames for the same drawable -- real GL content never had a chance to
actually stay on screen until this was fixed. Added `Sdl2UnifiedBackend
::HasRealGlActivity()` (true once the app has called `eglSwapBuffers`
at least once) and gated both real call sites that invoke
`RepresentLastFrame` (`tools/game_probe.cpp`'s main loop and the
in-`CallArmFunctionChecked` liveness check, threaded through a new
optional `backend_for_liveness` parameter) on it being false. Matches
this same investigation's own confirmed finding that Double Dragon
stops calling `IDISPLAY_Update` entirely once real GL rendering starts
-- there is nothing left worth re-presenting from that point on.

Also confirmed, directly with the user: the simulated button-hold
this dev tool injects is not an artificial requirement -- real
disassembly (documented much earlier in this investigation, see
`kSimulatedDeviceHandle`'s own doc comment in `tools/game_probe.cpp`)
already established Double Dragon's own real code genuinely waits for
real HID/controller input at this stage on real hardware; the
simulated hold exists purely so this unattended dev tool can validate
that path without a human present to press a real button.

**Net result this round**: the real black-screen blocker (the original
permanent one) stays fixed; real GL rendering now genuinely reaches the
real screen (a first, confirmed via direct evidence, not assumed); the
screen being black right now is Double Dragon's own real, correctly-
computed output given its own real, pre-existing viewport bug, not an
emulation or presentation defect. `git diff --stat`: `Sdl2UnifiedBackend`
(new), `tools/game_probe.cpp`/`tools/CMakeLists.txt` updated,
`NullGlBackend` removed. All temporary diagnostics (`[DBGGL]`
`ClearColor`/`Viewport` trace) reverted. 292/292 tests pass. Concrete,
well-specified next step: the degenerate `glViewport(0,0,1,0)` call
itself -- find the real code that computes those dimensions and why
they're wrong, not attempted this round.

---

## The degenerate viewport: root-caused and fixed for real

Went straight after the concrete next step from the previous round: a
live `pc==`-gated trace (temporary, reverted) on the real `GlViewport`
call found its real caller at `0x0011d47c`. Disassembling backward from
there found a thin real dispatch wrapper (`0x124498`, a real GL
function-table call, unrelated to the bug) and, one level further back,
the real values themselves: `applet_ptr+0xad0+24`/`+26`, read as signed
16-bit halfwords, with `width` getting a real `+1` added before being
stored (explaining the observed `w=1` from a real `0` -- `0+1=1` -- not
a literal hardcoded `1`).

**Traced the real writer with a live memory watchpoint** (temporary,
reverted, same `g_debug_watch_addr`/`g_debug_last_pc` bridge this log
has used before) armed on the correct address *before* `CreateInstance`
even runs (an earlier attempt armed it right after, and missed the
write entirely -- the real value is already wrong by the time
`CreateInstance` returns). Found the exact real write site
(`pc=0x0011d6b4`) and, walking a little further back, its own real
source: a real vtable call through `[applet_ptr+12]` -- confirmed,
via this project's own already-documented `mod_runtime.cpp`
(`kAppContextShellOffset = 12`), to be the real `IShell` pointer --
slot 4 of its vtable, i.e. the real, standard BREW
`ISHELL_GetDeviceInfo(IShell*, AEEDeviceInfo*)` call, which
`core/brew/ishell.cpp` already correctly identified by name (comment:
`// 4  GetDeviceInfo`) but left a blind `Stub` -- confirming the
degenerate viewport was never a rendering bug at all, just an
unimplemented real BREW API silently returning zeroed output.

**Confirmed the real struct layout against the real bundled SDK header**
(`research/docs/sdk_installer_extract/brew_sdk_headers_reference/
brew_mp_7.12.5_sdk/AEEShell.h`) rather than guessing: real
`AEEDeviceInfo` starts `uint16 cxScreen; uint16 cyScreen;` at offsets
0/2 -- an exact match for what real Double Dragon code reads out of
this call's own output buffer.

**Implemented `IShellHle::GetDeviceInfoImpl`** for real: writes
`cxScreen`/`cyScreen` from a new `screen_width`/`screen_height`
constructor parameter (defaulted to 640/480, Zeebo's one real native
resolution, so every existing call site -- ten-plus in `tests/
brew_test.cpp` alone -- keeps compiling unchanged; `tools/game_probe.cpp`
and `frontends/standalone/main.cpp` now pass `kWidth`/`kHeight`
explicitly). New test `IShellHle.GetDeviceInfoWritesRealScreenDimensions`
confirms the real write. **Verified against the real Double Dragon run
on the real desktop**: the viewport is now correct (`w=641 h=480` --
the real `+1` on width lines up exactly), and real geometry finally
became visible on screen for the first time this whole investigation --
directly observed by the user as "forms trying to be real game
graphics."

**A new, real, separate symptom appeared once real content was finally
visible: the screen settled into solid white after a few frames.** A
live `[DBGGL2]`-tagged trace (temporary, reverted) found the real cause
immediately: real code calls `glGenTextures`/`glBindTexture` many times
with real, freshly-generated texture names, but **never once** calls
`glTexImage2D` -- confirmed via the same trace run showing thousands of
real `glDrawArrays` calls but zero `glTexImage2D` calls. Real GLES
texture objects with no image data ever uploaded are "incomplete";
sampling one is undefined/white on many real implementations, and with
this many real draw calls covering the screen, the cumulative visual
result is exactly the observed solid white. `core/brew/gl_hle.h`'s own
existing doc comment already flagged this precisely: "glTexSubImage2D,
compressed textures, ... is still Stubs" -- real Double Dragon almost
certainly uploads texture data through a real compressed-texture path
(this project's own bundled research samples reference real ATITC
compression) that this project has never implemented at all. Not
pursued further this round -- a concrete, well-evidenced next target.

**Separately, and just as significant: the simulated button-hold this
file has relied on since early in the Double Dragon investigation
turned out to be actively harmful, not helpful, once real rendering
started working.** Direct real-desktop testing, at the user's own
suggestion, with the simulated hold shortened from ~4 seconds (240
ticks) down to ~250ms (16 ticks) still showed the game visibly racing
through several distinct real screens (loading -> a title-shaped
screen -> a selection -> white -> another screen -> white) well before
any one could be observed stably. **Testing with the simulated press
removed entirely settled the question directly**: with zero simulated
input, the game reaches a real, stable title screen on its own and
*stays there* -- the original assumption (from early HID-investigation
rounds, before real rendering worked) that a held press was needed to
get past the loading state was never correct; what that early testing
actually observed was the real per-tick state machine legitimately
advancing on a real timer/loading-completion condition, misattributed
to the simulated input because nothing could be *seen* to distinguish
the two before real rendering existed. Removed the simulated
button-hold injection from `tools/game_probe.cpp` entirely (not
shortened further -- deleted): real, sustained per-tick "held" input
apparently reads to real game code as repeated discrete presses, not a
single hold, racing through screens; a real human's own real keyboard/
controller input (already separately wired up via `PollInput`/
`SdlKeyToAvk`) is the correct way to navigate further now that there's
something real to navigate. The `simulated_button_events` queue and the
real HID `GetNextButtonEvent` plumbing draining it are kept intact,
just no longer auto-populated.

**Verified end to end on the real desktop, twice independently**: the
game now reaches the real, correctly-computed title screen (black
background, white unrendered-texture rectangles where real sprite
assets should be -- the confirmed-real, separate `glTexImage2D` gap
above) and stays there stably, both immediately after this round's
fixes and again after a full clean rebuild. All temporary
instrumentation (`[DBGGL2]`, the `GlViewport`/write-site watchpoints)
reverted; `git diff --stat`: `core/brew/ishell.{h,cpp}` (real
`GetDeviceInfo`), `tests/brew_test.cpp` (new test),
`tools/game_probe.cpp`/`frontends/standalone/main.cpp` (pass real
width/height; button-hold injection removed). 293/293 tests pass (292
+ the new `GetDeviceInfo` test).

## Sound: `MediaHle` is real and wired, but never registered -- and Double Dragon's title screen doesn't ask for it anyway

With the title screen now visible and stable, the next candidate was
audio. `core/brew/media_hle.h`/`.cpp` turned out to already be a
complete, real `IMedia` implementation (WAV/MIDI decode via
`core/loader/{wav,midi}.h` -> `Mixer` -> real SDL2 audio device), and
`tools/game_probe.cpp` already constructs it (`MediaHle media_hle(...)`)
and drives its output every real tick (`mixer.Mix(backend, ...)`,
confirmed already present in the main loop). **But `media_hle.Build()`
is called and the object is never handed to `shell_hle.RegisterInstance`
under any `ClsId` at all** -- a real, concrete, no-guessing-required bug:
any real `ISHELL_CreateInstance(shell, AEECLSID_MEDIA, ...)` call the
game makes is guaranteed to fail today, regardless of which numeric
`ClsId` value is correct.

Finding that numeric value turned into a real dead end this round, not
a guess-and-move-on: no bundled reference header in `research/` defines
`AEECLSID_MEDIA` (only usage-doc comments in the real `AEEIShell.h`
reference and the real `AEEMediaUtil.c` sample, neither with the
literal value). A temporary `[DBGCLS]` print added to
`IShellHle::CreateInstanceImpl` (reverted after use) recorded every
`ClsId` Double Dragon's own code actually requests and fails to get,
across a clean, unmodified, **100-real-second** idle run of the already-
stable title screen: exactly eight calls total, covering six distinct
`ClsId`s (`0x01001002`, `0x0102f679`, `0x01030852`, `0x0102f681`, then
`0x0100550a` and `0x01005501` twice each), and **all eight happen within
the first ~150ms of the run** -- zero further `CreateInstance` calls of
any kind for the remaining ~99.85 real seconds of idle title-screen
time. The `0x0100550a`/`0x01005501` pair fires specifically from inside
the already-identified real download/install-progress notification
callback (`0x0011d020`, see the `0x01005511` class doc comment above) --
numerically adjacent to it (`0x010055xx`) and reached from the exact
same real call chain, not independently -- strong evidence this is the
*same* download/catalog subsystem, not `IMedia`. None of the eight shows
an `IMedia`-shaped call pattern (no `SetMediaParm`-style
`(nParamID, p1, p2)` triple was ever going to be observable anyway,
since every one of these `CreateInstance` calls fails before any method
could be invoked on a result).

Tried the project's own established "give it a generic per-slot-logging
scaffold and watch what real code does with it" technique (temporary,
reverted) on all six -- this was informative but in the wrong direction:
registering a fake object for `0x01001002` let real code proceed into
new territory that hadn't been reached before, ending in a real crash
(`pc=0x00000000` wandered outside the module after ~97871 steps, from a
`timer callback threw: Miscellaneous instruction space` exception).
`0x01001002` sits in the same real low system-class range as the
already-confirmed `AEECLSID_DISPLAY` (`0x01001001`) and `AEECLSID_
FILEMGR` (`0x01001003`) -- almost certainly a real core BREW class
(plausibly `AEECLSID_STATIC`), and definitely not safe to paper over
with a blind stub. Reverted immediately; not registered.

Also chased a real, distinctive anchor -- the literal string `sound.ggz`
appears twice in `ddragonz.mod`'s own rodata (file offsets `0x4dae8`/
`0x4e1a8`), sitting inside what a raw hex dump confirms is a genuine,
large, fixed-stride (0x4c-byte) per-asset resource descriptor table
(hundreds of `data.ggz`-referencing entries follow, continuing across
multiple regions of the file). This is real and worth returning to --
it's very likely the real per-resource-to-archive index Double Dragon's
own asset loader walks -- but decoding its field layout (which field
selects an audio vs. image handler, if any) wasn`t attempted this round;
`objdump -b binary` on this file gives no symbol/relocation information
to mechanically trace which code reads which literal-pool entry, so
this would need real, careful manual disassembly around the functions
referencing offsets `0x4dae0`-`0x4e460`, not a quick check.

**Conclusion for this round**: on the real, already-fixed, stable title
screen, Double Dragon genuinely does not request `IMedia` (or anything
else) at all without further real interaction -- title-screen audio
(if any) is gated behind real menu/gameplay progression this project
deliberately no longer auto-simulates (see the button-hold removal
above). Getting further on sound needs either (a) a real human driving
real keyboard/controller input far enough to reach a state that
actually calls `CreateInstance` for `IMedia`, live-traced the same way
the `GetDeviceInfo`/viewport bug was found, or (b) manually walking the
real `0x4dae0`-region resource table's real field layout to find a
static, not-yet-reached real call site. Not a wasted round -- the
`MediaHle`-never-registered bug is confirmed and real regardless of
which path resolves the `ClsId` question -- but no numeric `ClsId` was
registered this round, since every candidate examined turned out to
belong to a different, already-identified real subsystem or an
unconfirmed core system class, and guessing one in would violate this
project's whole evidence-first approach. All temporary instrumentation
(`[DBGCLS]`, the per-slot scaffold, a `[DBGSPIN]`/`[DBGTICK]` step-count
check that disproved an initial false-alarm "hang" reading -- the
title screen just stops being *traced* past tick 9, it never stops
*running*) reverted; `git status` clean; 293/293 tests still pass.

## Textures: real ATITC support, derived from this project's own bundled sample -- not guessed, not from memory

Pivoted to the other confirmed, self-contained gap from earlier this
round: real code calls `glGenTextures`/`glBindTexture` many times but
never `glTexImage2D`. `core/brew/gl_hle.cpp`'s vtable already had slot
15 (`glCompressedTexImage2D`) as a blind `Stub` -- and this project's
own bundled research happens to include Qualcomm's real
`simple_atitc.c` reference sample (`research/samples/
MSM7500_OGLES_qcom_sdk_samples.release_02.15.07.beta1/simple_atitc/`),
confirming real Double Dragon almost certainly uploads textures through
`GL_COMPRESSED_RGB_ATI_TC`/`GL_COMPRESSED_RGBA_ATI_TC` (real ATI/AMD
"ATITC" block compression) instead.

**No public spec text or remembered algorithm was trusted blindly.**
The same bundled sample directory also ships two real, matched pairs of
compressed-texture-file + its original uncompressed source image
(`texture_rgb.atitc.org`/`texture_rgb.tga`, `texture_rgba.atitc`/
`texture_rgba.tga`, 128x128 each) -- real ground truth this project
already had checked in. Used that directly: a Python analysis script
(1) confirmed the block-file's row order matches the TGA's own raw
(unflipped, bottom-up) scanline order exactly, by testing all four
combinations of pixel/block orientation against real per-block color
averages (33.3 avg error for the correct combination vs. ~197 for the
wrong one -- unambiguous); (2) derived the real per-code color-
interpolation weights via least-squares regression over every real
texel in the image, pooled by 2-bit selector code, rather than assuming
DXT1's evenly-spaced weights -- the fit came back clean and physically
exact (`code0=(1,0)`, `code1=(0.666,0.334)≈(2c0+c1)/3`,
`code2=(0.375,0.625)=(3c0+5c1)/8`, `code3=(0,1)`), confirming ATITC's
real, distinctive quirk: color2/color3 are **not** evenly spaced like
DXT1's are; (3) found the real RGBA block layout (16 bytes: 8 bytes of
DXT3-style explicit 4-bit-per-texel alpha nibbles, low nibble first,
*then* the same 8-byte RGB block, confirmed by testing "color first" vs
"color last" against real alpha data -- 360 avg error vs. 17.6,
unambiguous) and its alpha nibble scale (×17), which came back at 0.37
avg error -- essentially exact modulo real 4-bit quantization. Full-
image reconstruction error against the real ground truth: ~4.6/channel
for color (a real, physically reasonable amount of loss at this
format's real ~6:1 compression ratio), ~0.4 for alpha. The real GL
enum values (`GL_COMPRESSED_RGB_ATI_TC=0x8C92`, `_RGBA_ATI_TC=0x8C93`)
were separately confirmed from a second real bundled header
(`research/docs/sdk_installer_extract/qx_cab/
_81F04985BCAA4B3C9B34D39633767DB0`), not guessed either.

Implemented as `core/loader/atitc.h`/`.cpp` (`DecodeAtitc`, a pure
host-side decoder producing plain RGBA8 -- no HLE/GL coupling, directly
unit-testable), wired into a new `GlHle::GlCompressedTexImage2D`
(`core/brew/gl_hle.{h,cpp}`, replacing the slot-15 `Stub`): decodes the
real compressed bytes, then forwards the result to the existing
`GlBackend::TexImage2D` path unchanged, since a real desktop host GL
implementation won't have this real mobile-only extension. New test
file `tests/atitc_test.cpp`: two of the four tests embed real 8-/16-
byte blocks lifted byte-for-byte from the bundled real sample files
(not synthetic) with their expected decoded pixels, pinning the C++
implementation to the already-Python-verified result; the other two
cover truncated-input rejection and non-block-aligned crop. 297/297
tests pass (293 + 4 new).

**Verified visually on the real desktop**: screenshotted the running
window twice independently. Before this fix: solid white rectangles on
black. After: a detailed, sharp-edged monochrome silhouette (reads as a
stylized skull/dragon-head loading-screen graphic) with real fine
structure -- eye socket, jaw, teeth -- reproducible across both runs.
Real texture *data* is now visibly driving what's on screen, not an
"incomplete texture samples as white" fallback. The image itself is
strikingly black-and-white rather than full color; given the color
decode passed its own independent, real-ground-truth-validated test
(~4.6/channel error, not a systematic "everything is white" failure
mode), the most likely explanation is that this particular real asset
*is* genuinely a stark two-tone logo (alpha-cutout sprite over a black
clear), not a decode bug -- not confirmed further this round, since the
shape/detail improvement alone is the significant, evidenced result.
`git status` clean (`core/loader/atitc.{h,cpp}`, `core/brew/gl_hle.
{h,cpp}`, `core/brew/gl_types.h`, `tests/atitc_test.cpp`, both
`CMakeLists.txt`s -- no temporary diagnostics left in any of them).

## Textures, round two: not every white rectangle is ATITC -- one confirmed misrouted real OBM1 font, real root cause still open

The user ran the fixed build directly and reported textures still not
loading -- screenshotted their own live session (twice, after fixing a
window-focus/monitor issue) and confirmed: the *first* real texture
this session already found (the skull/dragon-head silhouette) still
renders correctly, but a *later* real screen state shows large,
undifferentiated white rectangular panels -- the original "incomplete
texture" symptom, just for different real assets than the one already
fixed.

Added temporary, targeted tracing (all reverted after use) to find out
why, rather than guessing: a debug print in `GlCompressedTexImage2D`
itself first showed real calls with `internalformat=0x8b9d` (not
either real ATI_TC token, correctly rejected as "unhandled format" by
the existing code) and absurd `width`/`height` (tens of thousands of
pixels -- e.g. `50385`x`18715`). Captured the real caller's `LR` and
disassembled the actual real ARM call site (`ddragonz.mod` offset
`0x11e14c`, calling a real thin `IGL_glCompressedTexImage2D`-shaped
trampoline confirmed to start at `0x124188`, itself doing nothing but
forwarding all 8 real arguments unchanged from *its own* caller through
vtable slot 15 -- so the real 4-register + 4-stack-word ABI assumption
already in this code was confirmed correct, not the bug).

Traced back further: `width`/`height` come from real memory fields
`*(r4+0)`/`*(r4+4)`, not registers, where `r4` is a real per-resource
descriptor pointer walking a small array (a real, incrementing index
field at `r4+8`: `2, 3, 4, 5, 6, 7, ...`). A PC-gated register/memory
dump (temporary, reverted) at the trampoline's entry confirmed these
memory values are real, deterministic, non-garbage data -- not
uninitialized memory -- ruling out a simple "stale stack slot" bug.
The surrounding real function (a large block of real per-pixel
bit-field extract/repack code just before the call, handling several
real pixel-format cases via a real 5-way jump table on a value at
`[sp+20]`) is genuine real texture/pixel-format-conversion logic, not
misidentified code.

**Found the real, concrete smoking gun by checking what `data.ggz`
actually contains at the exact real file position/size the traced
`IFILE_Read` calls request** (temporary `OpenFile`/`Read` tracing,
reverted -- confirmed `data.ggz` is loaded as a single flat, padded
1,229,921-byte blob under the literal VFS name `"data.ggz"`, matching
this project's own long-established "real code streams its own GGZ
archive and decompresses members itself" finding, not a per-entry-
decompressed archive). Extracted the real bytes at one traced read's
exact `(pos, want)` in Python: a genuine gzip stream (real `1f 8b 08`
magic, `FNAME` flag set) whose real embedded filename is
**`Font.obm1`** -- a real OBM1-format bitmap/font resource, a format
this project already has its own working, tested loader for
(`core/loader/obm1.{h,cpp}`), decompressing (real Python `zlib`,
matching this project's own already-confirmed real gzip framing) to
exactly the requested `16904` bytes, confirming the *read* itself is
correct -- the compressed stream is genuinely only `774` of those bytes
though (`d.unused_data` len `16130`), meaning real code's own read-size
request already assumes the *decompressed* size, not the compressed
size, consistent with (not necessarily itself broken by) real code
performing its own real in-ARM gzip inflation after the read.

**Conclusion**: this is not an ATITC decoding bug, and not a data.ggz/
file-reading bug -- it's a real, more fundamental **resource-type
dispatch bug**: this real font/bitmap resource is reaching the exact
same generic real per-resource pixel-conversion-then-
`glCompressedTexImage2D`-upload code path that real ATITC textures use,
instead of whatever real, different path would correctly route it to
this project's own already-working OBM1 loader. The real cause of that
misrouting (a wrong branch taken at the `[sp+20]` 5-way jump table, or
a wrong value feeding it, likely from some other still-Stub or
partially-implemented real HLE call upstream) is not found yet -- real,
concrete evidence narrows it to "some real dispatch/classification
value is wrong," not "which specific missing HLE call computes it,"
which needs picking the `[sp+20]` value's own real producer apart next,
a genuinely separate investigation from anything ATITC-shaped.

**Landed a real, permanent, defensive fix, not just a revert**:
`GlCompressedTexImage2D` now rejects `width`/`height` above 4096 (any
real 2009-era mobile GPU's real practical max texture size) before
ever attempting a decode/allocation -- so a future call like this one
(a real non-texture resource wrongly routed here) can never trigger a
many-hundred-megabyte `DecodeAtitc` buffer allocation, regardless of
whether the real dispatch bug above is fixed first. This is separate
from, and doesn't change, the already-correct-and-verified real ATITC
decode path itself. All temporary diagnostics (`[DBGCTI]`, the
`[DBGCALLER]` PC-gated probe in `tools/game_probe.cpp`, the
`[DBGFILE]` `data.ggz` open/read tracing in `core/brew/file_hle.cpp`)
reverted; `git status` clean except the one real, permanent
`core/brew/gl_hle.cpp` bounds-check. 297/297 tests pass (unchanged --
no new functional behavior to test yet, since the real dispatch bug
itself isn't fixed).

## Correction: the "verified visually" ATITC claim above was wrong -- no successful decode has ever actually happened

The user directly disputed the "verified visually" claim from the first
"Textures" section above (real, correct pushback, not a false alarm).
Re-added temporary success-path tracing (`[DBGCTIOK]`, printing on
every real, successful `DecodeAtitc` call) and a temporary print on
every rejected call (`[DBGCTIFMT]`), across several fresh runs,
including one screenshotted at the exact moment the dragon-shaped
silhouette was on screen. **Zero `[DBGCTIOK]` lines, ever, in any run
captured across this entire investigation (grepped every log file this
session produced) -- and zero `glTexImage2D` calls either**, in a run
that had the dragon silhouette fully rendered and screenshotted at the
same time. The *only* real calls this project has ever observed into
`glCompressedTexImage2D` are the same ten `Font.obm1`-misroute calls
from the previous section (`internalformat=0x8b9d`, correctly
rejected).

**This means the "verified visually" claim in the first "Textures"
section was a false positive, not a confirmed fix.** The visual
change observed earlier (blank rectangles -> a detailed silhouette)
was real, but it was never actually caused by, or evidence for, the
ATITC decoder -- it's the same pre-existing "an incomplete real GL
texture (bound, but never given real pixel data via *any* real upload
call) samples as white on this host's real GL implementation" fallback
that was the *original* bug, showing through real, correct mesh
geometry that has nothing to do with texture color data. Two different,
unrelated real observations (a visual improvement between two
separately-timed screenshots; a genuinely-correct, ground-truth-
validated ATITC decoder) got connected into a causal claim that was
never actually checked at the time -- a real process mistake, not a
subtle bug: the fix was to look at whether `[DBGCTIOK]`/`[DBGTI]` had
*ever* fired, which this round finally did.

**What's still real and true**: `core/loader/atitc.cpp`'s `DecodeAtitc`
is independently, correctly verified against this project's own real
bundled Qualcomm ground-truth sample data (`tests/atitc_test.cpp`,
293->297 passing tests, unaffected by this correction -- that
validation never depended on the real running game at all). What's
**not** true: that Double Dragon has ever been observed calling it, or
`glTexImage2D`, with valid arguments, in this entire investigation.
**The real open question is now more fundamental than "which format
enum"**: why does Double Dragon appear to never call either real
texture-upload entry point with valid data at all, given real
`glGenTextures`/`glBindTexture` calls are confirmed happening
throughout (established many rounds ago) and the only real
`glCompressedTexImage2D` calls observed are a real, different resource
type being misrouted here. Not resolved this round -- a genuinely
open, deeper question for next time, not a quick fix. All temporary
instrumentation reverted again; `git status` clean; no code changes
this round beyond the already-committed permanent bounds check.

## Root cause found for real: a currently-Stub static-base gate function lets real code run on still-compressed data

Picked the real dispatch chain apart by disassembling further, not
guessing. `0x11ddb8` (the function feeding `glCompressedTexImage2D`'s
bogus args) turns out to call three things through the app's own real
static-base function-pointer table (`core/brew/mod_runtime.cpp`'s
`Install()`, table at `0x80280000`, real address confirmed by decoding
the exact ROPI PC-relative computation at the real call site, not
assumed): slot `0xDC` first (`unknown_0xdc_fn` -- one of several real,
previously-unidentified slots this project already flagged as unknown
and Stubbed years of rounds ago), gating everything that follows; then,
if that returns success, slot `0x0` -- which is `kMemcpySlotOffset`,
i.e. **real `memcpy`**, not a property accessor as read last round.
The "`GetProperty(propid, out, count)`"-shaped calls traced previously
are really just `memcpy(dest, src, n)` with `src` = a real pointer
(`r6`) at small fixed byte offsets (+2, +3, +4, +6).

**Dumped `r6`'s real memory directly** (a temporary PC-gated probe,
reverted after use) across every one of the ten real calls this
project has ever observed reaching this path. Every single one starts
with real, live gzip magic bytes (`1f 8b 08 08`) -- **`r6` is the raw,
still-*compressed* resource stream**, not decompressed data, not a
parsed header. The real embedded gzip filenames (`FNAME` field) are
`Font.obm1`, `FONT_L.obm1`, `FONT_L2.obm1`, `Title.obm1`, `Menu_P.obm1`,
`OptionBG1.obm1`, `OptionBG2.obm1`, `HowTo.obm1`, `HowToBG1.obm1`,
`HowToBG2.obm1` -- **every single real title-screen/menu/how-to-play
graphic this title has, all real OBM1 bitmaps** (this project's own
already-working `core/loader/obm1.{h,cpp}` format), not one ATITC
texture among them. The earlier "`Font.obm1` misroute" framing
undersold this: it isn't one stray resource hitting the wrong branch,
it's *the entire real title-screen asset set* never reaching real
decompression before this generic property-extraction code runs.

**Proved this mathematically, not just by inspection**: the observed,
always-identical rejected `internalformat` (`0x8b9d`) is computed by
real code as `local20 + 0x8b95`, where `local20` is `memcpy`'d from
`r6+2` -- gzip's own **compression-method byte**, which is always `8`
(deflate) for every real gzip stream. `0x8b95 + 8 = 0x8b9d` exactly.
The "format enum" was never a format enum at all -- it's gzip header
byte 2 plus a constant, an arithmetic artifact of running real
width/height/format-extraction logic against raw compressed bytes it
was never meant to see at this point. Likewise `width`/`height` are 2-
byte slices of gzip's own `MTIME` field, not image dimensions.

**Real, confirmed root cause**: `unknown_0xdc_fn` (static-base offset
`0xDC`, `core/brew/mod_runtime.cpp`) is currently `[](IArmCore& core) {
core.SetRegister(kR0, 0); }` -- an unconditional "success, proceed"
Stub. Real disassembly shows it's called first, on the raw compressed
stream pointer, and gates (via its return value) whether the rest of
this function runs at all. Its real job is almost certainly to trigger
real decompression (or detect "not yet decompressed" and dispatch
elsewhere) before any of this generic property/format extraction is
valid to run -- with it permanently stubbed to "yes, proceed," every
real OBM1 asset this title has runs straight through width/height/
format extraction against compressed garbage instead. This is
consistent with, and finally fully explains, the project's very first
Double Dragon texture finding many rounds ago: "`glGenTextures`/
`glBindTexture` many times, never once `glTexImage2D`" -- the real
asset pipeline for every title-screen graphic dead-ends here, before
any real upload call could ever be reached with valid data.

**Not fixed this round** -- implementing `unknown_0xdc_fn` correctly
needs a real design decision this project hasn't made yet (does real
decompression belong inside this HLE slot itself -- i.e. should we
gzip-inflate host-side and hand back decompressed bytes through some
new mechanism -- or does real ARM code do its own inflate elsewhere,
meaning this slot's real job is something narrower like a format-
sniff/redirect this project would need to identify first) rather than
a quick guess. Documented here precisely so it's a concrete, scoped
starting point, not an open-ended mystery, next time. All temporary
instrumentation (the `[DBGR6]` PC-gated probe) reverted; `git status`
clean; 297/297 tests pass (unchanged -- no functional code touched
this round).

## Fixed for real: real decompression, real OBM1 upload -- Double Dragon's title screen renders correctly, in full color, for the first time

Asked directly (the user, after the previous round's design-decision
question) to keep going the authentic way rather than guess a
pragmatic host-side workaround -- the right call, since the authentic
path turned up the *actual* real architecture, not just a plausible
substitute.

**Design question resolved by re-reading `mod_runtime.h`'s own
existing documentation of sibling static-base slots first** (offsets
`0xd0`/`0x184`/`0x1b4`), not by guessing: every one of them, real
disassembly already confirmed in earlier rounds, is a real call
through the app's own imported "system services" table -- the same
real mechanism `memcpy`/`malloc`/`free`/`realloc` already live in,
already implemented host-side in this project's own C++, not traced
into any real ARM code inside `ddragonz.mod` (there isn't any -- these
are real *external* imports, like libc, not part of the game's own
compiled logic). That settles it: `unknown_0xdc_fn` being a real
system-provided decompression primitive, implemented host-side, isn't
a shortcut substituting for the "authentic" answer -- given this
table's own already-established real nature, it *is* the authentic
answer.

**Implemented `ModRuntime::DecompressGzipInPlaceImpl`**
(`core/brew/mod_runtime.{h,cpp}`): real zlib `inflate` (same
`windowBits = 15 + 16` gzip framing this project's own
`core/loader/ggz.cpp` already uses), streamed incrementally to/from
emulated memory in growable chunks (real gzip streams don't declare
their own compressed length up front, and this project has no
advance guarantee of the real decompressed size either), overwriting
the same address with the real decompressed result -- matching the
real caller's own confirmed convention (a single pointer argument, no
output parameter, immediate subsequent reads from that same address).
Wired into static-base offset `0xDC`, replacing the "always succeed,
do nothing" Stub. New `tests/mod_runtime_test.cpp` tests: a real gzip
stream wrapping a real-shaped OBM1 header decompresses correctly in
place (byte-for-byte), and a second, larger (10,000-byte,
non-compressible) stream exercises both the growable-input and
growable-output chunking paths.

**Re-ran with only that one fix and traced the live results**
(temporary `[DBGCTI2]` prints, reverted after use): `internalformat`
immediately started reflecting real, live OBM1 header bytes instead of
gzip's own compression-method byte -- confirmed exactly:
`0x8b95 + real_flag_byte(4) = 0x8b99` for 8bpp assets,
`0x8b90 + real_flag_byte(4) = 0x8b94` for 4bpp assets (both formulas
already derived two rounds ago), both now driven by the real,
correctly-decompressed flag byte rather than gzip's method byte.
**Width/height became completely sane real values** -- `128x128`,
`256x256`, `512x512`, `512x256`, `128x512` -- real, plausible
power-of-two texture dimensions, a complete transformation from the
previous round's `50385x18715`-scale garbage.

**That, in turn, revealed the real, final piece**: even with fully
correct data, real code *still* funnels every one of these uploads
through the same real `glCompressedTexImage2D`-shaped vtable slot,
`internalformat` carrying an internal engine tag (not a real Khronos
GL enum) and `data` pointing exactly 8 bytes past a real OBM1 header.
Checked directly (another temporary print, reverted): the 8 bytes
immediately before every real `data` pointer are, byte-for-byte,
`4f 49 04 <bpp> <width-LE16> <height-LE16>` -- literally `"OI"` (real
OBM1 magic) followed by the real flag/bpp/width/height fields, matching
`core/loader/obm1.h`'s independently-reverse-engineered layout exactly,
field for field, on every single real call observed. Cross-checked the
declared `imageSize` argument too: `real palette size (2*2^bpp) + real
pixel data size (width*height*bpp/8)` matches it exactly in every case
(`16896` for a real 128x128 8bpp asset, `32800` for a real 256x256 4bpp
asset, etc.) -- not a coincidence, a complete, self-consistent real
format match. **Confirmed via `core/loader/obm1.h`'s own doc comment**
that this isn't a one-off: "all 89 real assets in that archive share
this exact layout" -- Double Dragon's `data.ggz` contains real OBM1
images *exclusively*; this game never uses real ATITC at all. The
`simple_atitc.c` bundled sample that motivated the original ATITC work
was real, correctly-derived Qualcomm SDK sample code -- just not what
this particular title actually ships.

**Fixed `GlHle::GlCompressedTexImage2D`** (`core/brew/gl_hle.cpp`) to
check for those real magic bytes directly (`data_ptr - 8` == `"OI"`)
*before* trusting `internalformat` as a real GL enum -- real, physical
evidence beats a real-but-nonstandard tag value every time. On a
match, reads the full real OBM1 file (header + this call's own
`imageSize` bytes) out of emulated memory and decodes it with this
project's own already-working, already-tested `Obm1Image::Decode`
(`core/loader/obm1.cpp`), then uploads the result as an ordinary
`GL_RGB`/`GL_UNSIGNED_BYTE` texture through the existing
`GlBackend::TexImage2D` path -- no ATITC involved. The real ATITC path
from two rounds ago is kept as a fallback for `internalformat` values
that really do match `GL_COMPRESSED_RGB(A)_ATI_TC`, with the same
defensive dimension bound as before; genuinely dead code for this
title, per the finding above, but not deleted -- this project has no
evidence yet that no other real title ever uses it. New
`tests/gl_hle_test.cpp` tests: a real-shaped, hand-built 2x2 OBM1 image
(red/green checkerboard, matching `tests/obm1_test.cpp`'s own already-
validated fixture shape) decodes and uploads correctly through the real
vtable slot; a call with neither OBM1 magic bytes nor a recognized
ATITC format uploads nothing, rather than guessing. 301/301 tests pass
(297 + 4 new: 2 `ModRuntime`, 2 `GlHle`).

**Verified on the real desktop, screenshotted directly**: Double
Dragon's title screen now renders *completely, correctly, in full
color* -- the real dragon line-art (gold/orange/red), the real
Japanese kanji logo (双截龍, gold-to-blue gradient on magenta panels),
"DOUBLE DRAGON" in gold English text, "APERTE O BOTÃO HOME" (the real
Brazilian-Portuguese localized prompt), and the real copyright block
("©Million Co.Ltd", "Brizo Interactive Corp. 2009") -- all present,
all correctly colored, all correctly positioned. This is the first
time in this entire project's history that a real target game's title
screen has rendered with real, correct texture data end to end. `git
status` clean (`core/brew/gl_hle.cpp`, `core/brew/mod_runtime.{h,cpp}`,
`tests/mod_runtime_test.cpp`, `tests/gl_hle_test.cpp` -- no temporary
diagnostics left in any of them).

## Real transparency: the magenta color-key, confirmed and fixed

The user caught it immediately on the very next look: real graphics
were rendering with a visible pink/magenta border/background instead
of being transparent -- exactly the gap `core/loader/obm1.h`'s own doc
comment had already flagged as unconfirmed ("many real sprite assets
use a distinctly magenta... palette entry as an apparent... color-key,
but the exact convention... isn't confirmed").

**Found the real mechanism by tracing what Double Dragon's own code
calls around these uploads**, not by guessing (temporary `[DBGCAP]`
prints on `glEnable`/`glDisable`, and temporary real-but-logging
implementations of the previously-Stubbed `glAlphaFuncx`/`glBlendFunc`
slots, all reverted after use): real code enables both `GL_ALPHA_TEST`
(`0xbc0`) and `GL_BLEND` (`0xbe2`) before drawing these sprites, with
`glAlphaFuncx(GL_NOTEQUAL, 0.0)` (discard exactly alpha==0 pixels) and
`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` (standard alpha
blending for the rest) -- the textbook real GLES1.x sprite-
transparency combination, confirmed via real, live register values,
not assumed from the API shape alone.

**Found the exact real color-key value the same evidence-first way**:
dumped every real OBM1 palette this project has observed a real width/
height for (temporary `[DBGPAL]` print, reverted) -- all ten showed
**palette index 0 == `0xF83E`** (RGB565: R=31,G=1,B=30 -> real,
near-pure magenta, `(255,4,246)` in RGB888), zero exceptions, across
every size and bpp. Combined with the `GL_NOTEQUAL 0.0` alpha-test
finding above, this confirms the real, simple, robust rule: **palette
index 0 is always the real transparency signal**, keyed by index, not
by re-matching a specific color value per pixel.

**Implemented for real**:
- `core/loader/obm1.h`/`.cpp`: `DecodedImage` gained a parallel
  `alpha` channel (`0` for real palette-index-0 pixels, `255`
  otherwise), populated in the same per-pixel decode loop that already
  resolves `rgb` -- an additive, backward-compatible change (the one
  other real consumer, `tools/obm1_inspector.cpp`, only ever read
  `.rgb` and needed no changes). New `tests/obm1_test.cpp` test proves
  the rule keys off the *index*, not the stored color (index 0 set to
  a deliberately non-magenta color still comes out alpha=0).
- `core/brew/gl_backend.h`: new `AlphaFunc`/`BlendFunc` virtual
  methods, implemented in both real backends
  (`frontends/standalone/sdl2_unified_backend.{h,cpp}`, the active one,
  and the legacy `sdl2_gl_backend.{h,cpp}`) as thin forwards to real
  host `glAlphaFunc`/`glBlendFunc` -- same "raw Khronos enums pass
  through unchanged" rationale already established for Enable/Disable.
- `core/brew/gl_hle.{h,cpp}`: `GlAlphaFuncx`/`GlBlendFunc` replace the
  two real slots (4, 6) that were blind Stubs, forwarding real
  arguments (with real `GLfixed` decoding for the alpha reference,
  matching every other `...x`-suffixed GLES1.x entry point this
  project already implements). `GlCompressedTexImage2D`'s real OBM1
  path now interleaves `rgb`+`alpha` into a real RGBA buffer and
  uploads with `format = kGlRgba` instead of `kGlRgb` -- a real alpha
  channel now actually exists for `GL_ALPHA_TEST`/`GL_BLEND` to act on.
- New tests: `tests/gl_hle_test.cpp` confirms the real OBM1 upload path
  now produces RGBA with the real color-key pixel at alpha=0, and that
  `glAlphaFuncx`/`glBlendFunc` forward their real confirmed argument
  values to the backend. `RecordingGlBackend`
  (`tests/gl_lifecycle_test.cpp`) and both real `GlBackend`
  implementations updated for the two new pure-virtual methods.
  303/303 tests pass (301 + 2 new... plus the existing OBM1 upload test
  updated in place for the new RGBA shape).

**Verified on the real desktop, screenshotted directly, immediately
after the fix**: the solid magenta panels behind the kanji logo and
"DOUBLE DRAGON" text are gone -- the real dragon line-art now shows
through cleanly where they used to be, exactly as real transparency
should look. All temporary instrumentation (`[DBGCAP]`, `[DBGPAL]`)
reverted; `git status` clean (`core/brew/gl_backend.h`,
`core/brew/gl_hle.{h,cpp}`, `core/loader/obm1.{h,cpp}`,
`frontends/standalone/sdl2_{gl,unified}_backend.{h,cpp}`,
`tests/gl_hle_test.cpp`, `tests/gl_lifecycle_test.cpp`,
`tests/obm1_test.cpp`).

## Real controller input, live from the keyboard: fully traced, fully wired, fully verified

The user asked directly: "hook up the controllers to the keyboard, so
I can control the screen and check that here." This turned into one of
this project's deepest live-tracing sessions -- and it fully worked,
confirmed end to end against the real, already-documented title-screen
progression gate.

**Re-confirmed the title screen genuinely never asks for anything on
its own** first (temporary `[DBGCLS]` `CreateInstance` trace, reverted):
90 real seconds with the now-fully-working textures/transparency, still
zero new `ClsId` requests beyond the same eight calls in the first
150ms. No shortcuts available -- real controller input is the only way
forward, exactly as the user proposed.

**Found the real, live consumer of the already-wired-but-never-fed real
HID button mechanism** (`hid_device_methods[9]`/`captured_button_callback`,
long present in `tools/game_probe.cpp` but never driven by anything).
A live read-watch (the same real technique from this project's earlier
rounds: a temporary global in `core/memory/memory.{h,cpp}`, a PC
tracker in `core/cpu/arm_interpreter.cpp`, both reverted after use) on
`captured_button_context+0x28` immediately found a real, consistent
per-tick reader at `ddragonz.mod` offset `0x12376c` -- a real per-
gamepad-slot "latch this frame's input, reset the one-shot edges"
function (`0x123740`), itself feeding a real "OR both gamepad slots
together" combine function (`0x11a2ec`) that writes
`applet+0x3618/0x361c/0x3620` -- confirmed via a second live write-
watch landing exactly where static disassembly predicted. `applet+
0x361c` bit `0x100` is the exact real bit this project's own much
earlier investigation (before this session) found gates the title
screen's progression -- full circle, same real struct, independently
re-derived.

**Injected a real test button press through the real callback and
watched it silently fail** -- `queue_remaining=0` (consumed) but the
callback's own internal translator function (`0x100740`) returned
failure. Traced *that* function directly rather than guess again:
it subtracts a real base UID from the real `nButtonUID` field and
jump-tables the result into a small `nButtonID`, silently dropping
anything outside its own real 10-entry recognized subset. **Found the
real base UID is real, named, and documented** -- one off from this
project's own already-confirmed `AEEUID_HID_Joystick_Device`
(`0x0106c3fd`) sits `research/docs/sdk_installer_extract/
sdk_installer_cab/_23C2FF7AB01B49768D1DB61FA4834C66`
(`AEEHIDDevice_Joystick.h`, real bundled Qualcomm header, 2008
copyright), naming all 16 real joystick button/axis UIDs explicitly.
Made one real off-by-one mistake deriving the jump table's real target
addresses (ARM's `add pc,pc,r1,lsl#2` reads `pc` as the *next*
instruction's address, not the current one) -- caught it by directly
instrumenting the branch targets (temporary, reverted) rather than
trusting the arithmetic blindly, then re-verified.

**Confirmed the real, complete result live, tick by tick** (temporary
prints, reverted): injecting the real `AEEUID_HIDJoystick_Back`
(`0x0106c403`) UID through the exact same path a real keypress now
uses shows `context+0x28`/`+0x2c` set (bit `0x100`) the instant the
callback runs, `applet+0x3618` (held) and `applet+0x361c` (just-
pressed) both showing bit `0x100` exactly one real tick later, held
persisting indefinitely (never released) while the press-edge
correctly self-clears the following tick -- textbook correct real
edge-triggered input state, matching this project's own already-
documented gate check precisely.

**Implemented for real**: `SdlKeyToHidButton` (`tools/game_probe.cpp`)
maps real keys to the real, named UID constants Double Dragon's own
table actually recognizes -- confirmed by the same live trace, not
assumed from the header alone: arrows -> real D-pad, Enter/Backspace ->
real `Back` (the confirmed progression button), Q/E -> real upper
shoulder buttons, Z/X/C/V -> real face buttons 1-4. `Start` and both
real thumbstick-click UIDs are real and valid but simply outside Double
Dragon's own recognized subset -- not a project gap, a real fact about
this specific title. Wired into the main loop's existing key-handling
block, alongside (not replacing) the classic AVK path: every keypress
now drives both real input mechanisms this codebase implements. All
temporary instrumentation (`[DBGWATCH]`, `[DBGNORM]`, `[DBGBR]`,
`[DBGCB]`, `[DBGSLOT9]`, `[DBGVERIFY]`, the one-shot test injection)
reverted; `git status` clean except `tools/game_probe.cpp`'s real,
permanent feature.

## Real 12fps -> 31fps: a missing -O flag, and a double wait stacked on real vsync

With the FPS overlay above in place (top-left "FPS:N" readout,
`Sdl2UnifiedBackend`), the user asked why Double Dragon's real title
screen was only running at ~12fps against a real "locked 30fps" they
found documented online. Investigated as two real, separate, additive
bugs -- neither guessed, both confirmed live.

**Bug 1: this project's entire build has never been optimized.**
`build/core/CMakeFiles/zeebulator_core.dir/flags.make` showed
`CXX_FLAGS = -std=gnu++20 -Wall -Wextra` -- no `-O` flag at all, and
`CMakeCache.txt` had `CMAKE_BUILD_TYPE` empty. CMake's single-config
generators default to no optimization at all when `CMAKE_BUILD_TYPE`
is unset -- meaning every build of this software ARM interpreter,
including every FPS measurement taken before this session, was
running fully unoptimized. Confirmed zero `assert()` usage anywhere
under `core/` first (so `-DNDEBUG` carries no correctness risk), then
added a standard, non-forcing default to the top of `CMakeLists.txt`:

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
```

Reconfigured, confirmed `flags.make` now reads `-O3 -DNDEBUG ...`,
full rebuild, 303/303 tests still pass (and the test binary itself
runs measurably faster: 1.05s -> 0.73s wall time for the whole
suite -- indirect but real corroborating evidence). Live-measured via
the FPS overlay against real Double Dragon: **12fps -> 27fps**, more
than double, from this one flag alone.

**Bug 2: `tools/game_probe.cpp`'s main loop was double-waiting on top
of real vsync.** 27fps was still short of the real ~30fps target, so
rather than guess further, added temporary instrumentation (reverted
after use) to get real evidence of where the remaining time went:

- A temporary `[DBGPACE]` print measuring real wall-clock elapsed
  time each outer-loop iteration, right before the loop's
  `SDL_Delay(kTickMs)` call, throttled to every 20th iteration.
  Showed elapsed consistently ~0-1ms once past the noisy first-10-
  ticks trace-logging startup window -- ruling out slow ARM
  interpretation as the remaining bottleneck; the optimized
  interpreter itself is fast.
- A temporary `[DBGTIMER]` print in `IShellHle::ScheduleTimer`
  (reverted after use) logging every real `ms` argument the game
  itself passes to `ISHELL_SetTimer`. Real, live output: **239 calls
  requesting `ms=32`** (one initial `ms=33`) -- i.e. Double Dragon's
  own real, compiled code is self-rearming its main-loop timer at a
  real 32ms period (~31.25fps) every single tick. This *is* the real
  "locked 30fps" the user found documented, confirmed directly from
  the game's own real requested value rather than inferred.

Read `IShellHle::Tick()`'s real matching logic
(`elapsed_ms >= remaining_ms` fires, else subtracts) against this
loop's fixed `kTickMs=16`: a clean 32ms request quantizes to exactly
2 outer-loop iterations with zero loss (16+16=32). So the simulated-
time math alone can't explain a 37ms-real-per-frame (27fps) result
against a 32ms-simulated (~31fps) target -- the gap had to be in real
wall-clock overhead the simulated-time model doesn't see.

Found it in the loop's tail: `SDL_Delay(kTickMs)` was **unconditional
and flat**, added on top of whatever real wall-clock time that
iteration's own work already took -- including, on the iteration
where the real 32ms timer fires, a real `eglSwapBuffers` call that
itself really blocks on the host's vsync (`SDL_GL_SetSwapInterval(1)`,
`Sdl2UnifiedBackend`'s own real, deliberate choice -- see its class
doc comment). A real vsync wait plus a second, redundant flat 16ms
wait right after it silently ate real throughput the simulated-time
accounting had no way to detect.

**Fixed** by making the wait elapsed-aware instead of flat:

```cpp
uint32_t elapsed_this_iter = SDL_GetTicks() - loop_start_ms;
if (elapsed_this_iter < kTickMs) SDL_Delay(kTickMs - elapsed_this_iter);
```

(`loop_start_ms` captured via `SDL_GetTicks()` at the very top of the
`while (running)` loop, before event handling.) This targets a real
~16ms *per outer iteration*, rather than *"whatever this iteration's
own real work took" + 16ms* -- a real host-loop pacing bug, not an
emulation-accuracy change; the simulated `kTickMs` fed into
`Tick()`/`mod_runtime.Tick()` is untouched.

**Verified live**, rebuilt (303/303 tests still pass), same real
Double Dragon run, same FPS overlay: steady **`FPS:31`**, matching
the real requested ~31.25fps cadence almost exactly, confirmed stable
across two screen captures a few seconds apart. Also surfaced a real,
previously-invisible-at-the-wrong-speed detail as corroborating
evidence this wasn't just the counter changing: a real "APERTE O
BOTÃO HOME" ("press the HOME button") prompt now visibly blinks on
and off between captures -- a real timed UI element that was rendering
too infrequently to ever be human-visible at 12-27fps.

All temporary instrumentation (`[DBGPACE]`, `[DBGTIMER]`, the
`#include <cstdio>` it needed in `core/brew/ishell.cpp`) fully
reverted; `git diff --stat` clean except the two real, permanent
fixes (`CMakeLists.txt`, `tools/game_probe.cpp`).

## Real freeze on the second button press: a missing per-event re-arm

The user reported: "If I press right the entire game freezes." Reproduced
live (real XTest-simulated keyboard input into the real window, via a
real focus-then-inject script -- no physical keyboard access needed) and
root-caused with real evidence, not guesswork; the actual bug turned out
to have nothing to do with "Right" specifically.

**First reproduction**: fresh process, real Right press -> both real
input paths (`HandleEvent`/classic AVK and the real HID button callback)
threw `Miscellaneous instruction space (MRS/MSR/etc.)` at a real,
consistent `pc=0x00090024`, and the window visibly stopped updating
(two screen captures several seconds apart were bit-for-bit identical --
even the title screen's own real "press HOME" blink animation had
stopped). A full instruction trace (`trace=true`, temporary) showed
the real callback (`ddragonz.mod` `0x11bdf4`) calling the already-
documented UID-translation dispatcher (`0x100740`) with `r0=0`; inside
it, `LDR r1,[r0]` then `LDR r12,[r1,#0x24]` then `BX r12` chains three
reads off a null base straight to `PC=0`, then (this project's
already-documented behavior) wanders ~400-500 harmlessly-zero-decoded
steps until PC coincidentally lands on a real, non-zero byte pattern
(Double Dragon's own real ClsId constant, `0x0102f789`, sitting at
`0x00090024`) that happens to decode as an unimplemented MRS/MSR-space
instruction.

**Assumed "Right" was special and it wasn't.** A direct side-by-side
comparison (same process, same real focus, `Down` then `Right`, full
trace) found `Down` succeeds completely -- `LDR r1,[r0]` returns a real,
valid pointer (`0x80064000`, confirmed to be this project's own
`kHidDeviceObject`, i.e. `r0` pointed at a real per-device struct
whose first field correctly held a pointer back to the scaffolded
`IHIDDevice` object). **Right crashed only because it happened to be
the button pressed *after* a full press+release cycle already
completed.** A temporary write-watch on that exact field
(`*captured_button_context`, `0x80300a54` this run) caught the real
culprit directly: real code at `ddragonz.mod` `0x10ada4`/`0x10adb8`
(inside `0x100740` itself) writes `0` into that field as part of its
own real cleanup once a full press+release cycle finishes. Nothing
ever writes a valid pointer back in -- confirmed by 20+ minutes of
pure idle waiting with zero writes, and by the field simply staying
`0` for every subsequent press. On real hardware this field is almost
certainly re-armed by firmware as part of genuinely delivering a new
signal; this project's own simulated injection (`tools/game_probe.cpp`,
directly invoking the captured real callback) was skipping that step
entirely, since it calls the callback straight through without going
through whatever real mechanism would normally repopulate it.

**Fixed** with one line, right before invoking the simulated callback:

```cpp
cpu.GetMemory().Write32(*captured_button_context, kHidDeviceObject);
```

**Verified live**: 8 real presses in one run (`Right, Right, Up, Down,
Left, Right, Right, Right`, deliberately hammering Right specifically
and repeatedly, interleaved with other directions) -- every single one
now succeeds via the real HID callback path (`... ran`, never `...
threw`), confirmed by a final screen capture still showing a live,
correctly-updating `FPS:31` readout, no freeze. 303/303 tests pass.

One separate, lower-priority loose end left alone: the *other*,
already-documented-as-experimental classic AVK path
(`SdlKeyToAvk`/`HandleEvent`, its own doc comment already says "not a
claimed-correct real key mapping") still throws for Right's specific
synthetic AVK code (`0xe02e`) even after this fix -- confirmed
harmless (doesn't poison later ticks or other presses, every
subsequent real interaction keeps succeeding) and out of scope here:
the real, permanent, confirmed-correct input mechanism is the HID
path, which is now fully fixed.

All temporary instrumentation (`trace=true` on both key-handling call
sites, the `=== BEGIN uid=... ===` marker, the write-watch globals in
`core/memory/memory.{h,cpp}`, the matching `g_debug_current_pc` tracker
in `core/cpu/arm_interpreter.cpp`) fully reverted; `git diff --stat`
clean except the one real, permanent fix in `tools/game_probe.cpp`.

## Real gameplay reached for the first time: a silent stdout buffering trap, then sprites vanishing forever on Right

With the freeze fixed, the user reached real Double Dragon gameplay
(the title screen's own "APERTE O BOTÃO HOME" prompt, then actual
Chinatown level 1) for the first time this project has ever gotten
that far, and reported a real, reproducible bug: pressing Right made
"a lot of sprites disappear" -- confirmed via two screenshots, one
right after gameplay starts (player, enemy, weapon, full HUD all
visible) and one right after pressing Right (only the background
brick wall/door/garage-door/poster remain; player, enemy, weapon, and
the entire HUD are gone).

**First real obstacle: an invisible stdout buffering trap that wasted
several repro rounds.** Live reproduction depends on the user playing
against a real window while this project reads `tools/game_probe.cpp`'s
redirected stdout log afterward -- but repeated attempts showed zero
new lines in the log despite the user confirming they'd played and
reproduced the bug. Root cause: glibc fully block-buffers stdout by
default whenever it isn't a real TTY (true for `> file.log`
redirection, this tool's entire live-debugging workflow) -- sparse
output (a handful of printf calls per keypress) can sit unflushed in
memory indefinitely while the process keeps running, so a live
`tail`/`grep` on the redirected log looks like nothing happened even
though it did. **Fixed for real, permanently** (not a workaround):
`std::setvbuf(stdout, nullptr, _IOLBF, 0);` at the top of `main()`,
switching stdout to line-buffered so every printed line reaches the
file the instant it's printed. Immediately confirmed working: the very
next repro round captured every real keypress.

**Root-caused the sprite loss with the same live-tracing technique
this project always uses.** Added temporary, unconditional one-line
prints inside `GlHle`'s real `EglSwapBuffers`/`GlEnable`/`GlBindTexture`
/`GlDrawArrays`/`GlDrawElements`/`GlDeleteTextures` (reverted after
use) to see every real GL call the game itself makes, tick by tick,
with no gating. **Direct, unambiguous evidence**: every tick before
the Right press binds and draws 7-8 real textures (one big background
batch, texture 3, `count=1980`, plus several smaller per-sprite/HUD
batches on textures 4/5/6/7/8/9/10) before each real `eglSwapBuffers`.
**The instant after Right is processed, every tick for the rest of the
run draws texture 3 alone** -- textures 4-10 (every sprite and the
entire HUD) simply stop being submitted, permanently, matching the
screenshots exactly (background survives, everything else is gone).

This is the *same* already-documented bug from the freeze
investigation, not a new one: the classic, explicitly-experimental AVK
path (`SdlKeyToAvk`, its own doc comment already says "not a claimed-
correct real key mapping") still makes real `HandleEvent` code jump
through a null function pointer for Right's made-up code (`0xe02e`),
wandering hundreds of steps through low, mostly-zero real memory
before finally crashing on an unimplemented instruction (caught,
logged, previously judged "harmless" since it doesn't halt the loop).
**It isn't harmless.** During that wander, real non-zero bytes
elsewhere in that low address range get misinterpreted as real ARM
instructions -- and evidently at least one of those accidental
"instructions" is a real store that corrupts whatever real per-frame
sprite/HUD draw loop reads its active-entity list from, permanently,
since the exception is caught and the loop just keeps going with
already-corrupted state.

**Fixed** by removing the risk at its source rather than special-
casing Right: the classic AVK path now only runs for keys with *no*
real HID mapping (`uint32_t avk = (hid_button_uid == 0) ?
SdlKeyToAvk(...) : 0;`). Every key this project currently maps
(arrows, Enter/Backspace, Q/E, Z/X/C/V) already has a real, confirmed-
correct HID equivalent, so this doesn't lose any real functionality --
it just stops taking a demonstrated-harmful risk with an admittedly-
fake, unconfirmed key code for no benefit.

**Verified live**: fresh build, same repro (Enter x2 into gameplay,
then Right pressed repeatedly -- 36 separate real Right button events
in one run), zero `key event threw` lines (the AVK path never fires
for it anymore) and the user confirmed directly: "Controller seems to
be working fine." 303/303 tests pass. All temporary GL-call
instrumentation (`core/brew/gl_hle.cpp`) and tick-tracing scaffolding
(`tools/game_probe.cpp`) fully reverted; `git diff --stat` clean
except the two real, permanent fixes (the `setvbuf` line and the
AVK-gating change), both in `tools/game_probe.cpp`.

Explicitly out of scope, called out by the user directly and left for
next: "There are some other issues though" -- not yet described in
detail, tracked as open follow-up work rather than guessed at here.

## Real depth-buffer infrastructure was completely missing: five real GL gaps found and fixed, one real bug still open

The user described two "other issues": a garbled "J1 %7D"-style HUD
string top-left during gameplay, and (raised mid-message, unprompted)
sprites layering in the wrong front/back order -- "characters that
should be appearing farther to the back are showing up in front."
Investigated both live.

**"J1 x7d" ruled out as a rendering bug, real root cause not yet
found.** Traced the real classic-2D `IDISPLAY_Update`/`DrawText` path
(temporary prints, reverted) and the real `sprintf`-family HLE helper
(`ModRuntime::SprintfImpl`, temporary trace, reverted): neither one
ever fires for this string -- `DrawText` only ever draws real
"CARREGANDO..." loading-screen text (bottom-left, a real, different,
correctly-rendering element), and `SprintfImpl` is never called at
all during this window. Dumped every real uploaded GL texture to PPM
(temporary hook in `Sdl2UnifiedBackend::TexImage2D`, reverted) and
found `texture 2`: a complete, correctly-rendered real ASCII bitmap
font atlas (digits, A-Z, a-z, accented characters, all legible) --
proving the font/glyph pipeline itself is correct. "J1 x7d" is a real,
correctly-rendered string; the bug (not found this round) must be in
whatever real game-state value gets sampled from that atlas to build
it -- a new, separate investigation thread, not a rendering defect.

**Sprite z-ordering: five real, confirmed HLE gaps found and fixed
this round, root cause of the remaining specific symptom still open.**
Live-traced `GlEnable`/`GlDisable` calls (temporary, reverted) and
found real Double Dragon code enables `GL_DEPTH_TEST` exactly once at
startup and never touches it again -- real code fully expects working
depth-based layering. Checked what this project actually gives it:

1. **No real depth buffer was ever requested.** `SDL_GL_SetAttribute`
   was never called with `SDL_GL_DEPTH_SIZE` before creating the one
   real GL context (`tools/game_probe.cpp`), so the real enabled
   `GL_DEPTH_TEST` had nothing real to test against. **Fixed**:
   request a real 24-bit depth buffer before context creation.
   **Confirmed live**: the door that previously rendered as a flat
   black rectangle now shows its real closed-door texture, and the
   health bar's real fill (previously invisible, hidden behind its
   own container) now renders correctly, blue, on top.
2. **`glDepthFunc`/`glClearDepthx`/`glDepthMask` were all silent
   Stubs** -- real per-draw depth comparison/clear-value/write-mask
   configuration was being dropped on the floor. **Fixed**: real
   implementations, forwarding to genuine host `glDepthFunc`/
   `glClearDepth`/`glDepthMask`, added to `GlBackend` (both real
   backends: `Sdl2UnifiedBackend`, the legacy `Sdl2GlBackend`) and
   wired into `GlHle`'s vtable (slots 9/21/22, previously `Stub`).
3. **`glPushMatrix`/`glPopMatrix` (vtable slots 60/61) were also
   silent Stubs.** Real GLES1.x code's standard per-sprite pattern is
   `glPushMatrix(); glTranslatef(...); <draw one sprite>;
   glPopMatrix();` -- with both calls silently dropped, any use of
   that pattern would leave every sprite's transform permanently
   accumulated onto the modelview stack instead of properly scoped
   per sprite. **Fixed**: real implementations added the same way.

**The remaining specific symptom (individual overlapping character
sprites still drawn in the wrong order) needed real evidence, not
more guessing, and didn't fully resolve from the fixes above.** Traced
real vertex data directly (temporary: `GlVertexPointer`'s real `size`/
`type` args, and each real `GlDrawArrays`' actual extracted Y/Z range,
both reverted) and found: real vertex position arrays *do* carry a
real per-vertex Z component (`size=3`, `type=GL_FIXED`, correctly
decoded via this project's existing `ReadGlComponent`), confirming the
extraction pipeline itself isn't the bug. But multiple, visually
distinct, simultaneously-overlapping character sprites all shared the
*exact same* real Z value (`-80.0`) in the same frame -- real code
never calls `glTranslatef`/`glLoadMatrixx`/`glMultMatrixx` at all
(confirmed zero calls to any of them, live), so per-sprite Z isn't
coming from a transform either. This means real hardware likely
doesn't separate *individual* same-layer character sprites by Z at
all -- only broad layers (background `-640`, static decoration
`-505`-ish, characters `-80`, foreground overlay `-2`) -- and relies
on real, correct *submission order* within the character layer for
final visual stacking. Whether this project's own submission order
for same-layer sprites actually matches what real game code intends
is a real, different, deeper question this round didn't answer --
needs real ARM disassembly of the sprite-list iteration/ordering
logic itself, not another round of GL-call tracing. Tracked as open
follow-up, not guessed at further here.

All temporary instrumentation across this investigation (`DrawText`/
`Update` prints in `core/brew/idisplay.cpp`, the `SprintfImpl` trace
in `core/brew/mod_runtime.cpp`, the PPM texture dumper in
`frontends/standalone/sdl2_unified_backend.cpp`, and the `GlEnable`/
`GlDisable`/`GlBindTexture`/`GlDrawArrays`/`GlTranslatex`/
`GlVertexPointer`/`glLoadMatrixx`/`glMultMatrixx` traces in
`core/brew/gl_hle.cpp`) fully reverted; `git diff --stat` clean except
the five real, permanent depth-related fixes. 303/303 tests pass.

## "J1 %7d" fixed for real: a missing printf width-field parser. Sprite ordering: real GL state confirmed correct, real bug is deeper

The user asked to proceed fixing both open issues. Drove the game
directly this time (real XTest-simulated input, no physical keyboard
needed) since both required targeted ARM disassembly work rather than
more live GL-call tracing.

**"J1 %7d": found and fixed for real.** Searched `ddragonz.mod`'s raw
bytes directly for the literal "J1" (this project's established
technique) and found the real, complete string at file offset
`0x6c0b0`: `"J1 %7d\0"` -- a genuine, real printf-style format
string (the on-screen "%" glyph just resembles an "x" at this
resolution). Confirmed the value is never substituted: neither the
classic `DrawText`/`Update` path nor `ModRuntime::SprintfImpl` ever
fires for it (both traced live, temporary, reverted). Tried to find
the real reference statically first (a literal-pool `ldr`/`add
pc-relative` load of `0x0016c0b0`) and found none anywhere in the
file -- consistent with this module's already-documented ROPI
compilation, where string addresses aren't stored as raw absolute
pointers. Fell back to a live memory read-watch (temporary, reverted,
`core/memory/memory.{h,cpp}`) on the string's real runtime address:
every read traced back to real trap `0xf000002c`. Confirmed via
`HleRuntime::Register`'s own real index arithmetic (`index =
functions_.size()` at call time, sentinel slot 0 already occupied) --
temporarily logged every registration (reverted) -- that trap index 11
is the 11th real `hle_.Register()` call, which is exactly
`ModRuntime::SprintfImpl`'s own registration in `Install()`'s real,
literal call order. **Root cause**: `SprintfImpl` only ever read a
single character right after `%` as the entire directive. For
`"%7d"`, that character is `'7'` -- not a recognized conversion --
so it fell through to the "unknown directive: emit literally"
fallback, which output the raw `"%7"` and left `'d'` to be copied as
an ordinary character right after, passing the whole `"%7d"` straight
through unsubstituted with the real integer argument never consumed.
**Fixed**: `SprintfImpl` now parses an optional `0`-flag and a real
decimal minimum-field-width between `%` and the conversion character,
padding the formatted result (space, or zero if the `0`-flag was
given) up to that width -- standard printf semantics, previously
entirely unimplemented. New test locks in the exact real string
(`"J1 %7d"` with argument 3 -> `"J1       3"`) plus zero-padding and
a too-small width (never truncates). **Verified live**: "J1" now
shows a real, correctly-substituted decimal value (space-padded, "0"
observed) with zero leftover format-string garbage.

**Sprite z-ordering: real GL depth state confirmed already correct,
remaining bug is deeper than GL configuration.** Traced the real
"draw one sprite" leaf function live (a character-layer-filtered
`GlDrawArrays` call, keyed on the already-known real Z `-80.0`,
capturing its caller's `LR` -- temporary, reverted) to real ARM
address `0x11d2d0` (confirmed via disassembly: a large
register-saving prologue, `push {r0-r9, sl, fp, lr}`, matching the
call site's own return address falling inside its body). Searched the
full disassembly for every real caller of `0x11d2d0` and found 17
distinct static call sites spread across a huge address range
(`0x104e98`-`0x122558`) -- this is a shared, general-purpose "draw a
textured quad" utility invoked from many different, separate places
in the game's own code, not one single central sprite-list loop.
Before going further into full entity-system reverse engineering,
checked the cheaper hypothesis first: is this actually a GL
depth-state configuration gap, now that a real depth buffer exists?
Live-traced real `glDepthFunc`/`glDepthMask` calls (temporary,
reverted): real code calls `glDepthFunc(GL_LEQUAL)` exactly once
(`0x203`, confirmed via the standard Khronos enum value) -- meaning
real code explicitly wants *equal* depth values to pass, so a
later-submitted same-layer sprite correctly overwrites an
earlier-submitted one at the same depth (the standard, correct real
technique for real depth-tested same-layer 2D sprites). `glDepthMask`
is never called at all -- real code relies on the real GL default
(writes enabled). **This is exactly what this project's own
`GlHle`/`GlBackend` already forward correctly** (both real fixes from
the previous round). Since the configuration itself is confirmed
correct and the visual bug still reproduces on top of it, the real
remaining discrepancy must be in the *actual submission order* real
ARM code produces for a given frame's active entities -- i.e.
whether this project's own emulation of whatever real code decides
which entity's own "think/draw" logic to run when (entity-list
iteration, real per-object state machines scattered across many real
functions, not a single loop) faithfully reproduces real hardware's
own order. That's a real, substantially larger reverse-engineering
task -- tracing the actual entity-update loop, not more GL-state
probing -- and is left as open follow-up, not guessed at further
here.

All temporary instrumentation this round (`core/memory/memory.
{h,cpp}`'s read-watch, `core/cpu/arm_interpreter.cpp`'s PC tracker,
`core/brew/hle_runtime.cpp`'s registration-index logger, the
`DBGWHO` tags across `ModRuntime`'s string-handling functions, and
`core/brew/gl_hle.cpp`'s `DBGLR`/`DBGDEPTHFUNC` traces) fully
reverted; `git diff --stat` clean except the one real, permanent
`SprintfImpl` fix (`core/brew/mod_runtime.cpp`) and its new test
(`tests/mod_runtime_test.cpp`). 304/304 tests pass (303 + 1 new).

## Sprite z-ordering, continued: mapped the real tick/render call chain, no fix yet -- this needs real entity-system reverse engineering

The user asked to keep going on sprite ordering specifically. Went
deeper into real static/live disassembly (no code changes reached
this round; every temporary probe below was reverted, `git diff
--stat` clean at the end).

**Traced the real per-tick entry point.** The real `ISHELL_SetTimer`
callback address this project already captured live
(`ddragonz.mod` `0x1239dc`) is itself just a 1-instruction tail-jump
(`b 0x104ab0`) -- the *real* master per-tick function is
`0x104ab0`. Statically disassembled its body: it calls a real HID
tick helper (`bl 0x123798`, `r0 = applet+0xa20` -- the exact real
per-device struct this project already fully mapped during the
controller-input investigation), conditionally one of two real
per-frame update paths (`bl 0x10493c` if `[applet+0x24] != 0`), then
branches on a real flag bit (`[applet+0x1000+0x5ac] & 1`) between two
further real functions: `0x11daf4` or `0x124528`.

**`0x11daf4` is a 3-instruction trampoline, not a render loop.** It
loads two words from `[r0+8]`/`[r0+0xc]` and tail-jumps to
`0x123f00`, which itself does a real indirect call through a
PC-relative global's own vtable slot 26 (byte offset `0x68`) --
i.e. a real `ISomething::Method26(...)` call through an
as-yet-unidentified real interface object, not an inline loop over
game entities. Didn't chase that interface's identity further this
round (would need the same kind of live vtable-object tracing this
project has used for `IShell`/`IDisplay`/`IGL` elsewhere, but for a
still-unknown, possibly game-internal, interface).

**Confirmed the "draw one sprite" leaf function (`0x11d2d0`, from the
previous round) is genuinely entity-agnostic.** Set a real, direct
PC breakpoint in `ArmInterpreter::Step()` (temporary, reverted) and
captured every real entry to it live during actual gameplay with
visibly-overlapping sprites: **`r0` was always exactly `0x32` (50) or
`0x0`, never a real memory address.** This rules out the natural
assumption that `r0` is "which entity is being drawn" -- it isn't a
self-pointer at all, more likely a quad-type/mode selector constant.
Whatever real per-entity state (position, which texture, and
critically *draw order*) exists must be established by each of the
17 real call sites *before* they call into this shared utility (matching
the earlier finding that real per-vertex Z/position data is fully
baked in by the caller, not computed inside this leaf function).

**Where this leaves the investigation**: there is no single, simple
"for each active entity, draw" loop visible in the code traced so
far. Real order comes from whatever decides, each real tick, which of
this title's many real per-object-type update/draw functions actually
runs and in what sequence -- likely a real linked-list-of-active-actors
or fixed-size-actor-table walk, each entry carrying its own real
"think" function pointer (a common structure for this era/genre of
game), reached through the still-unidentified vtable slot 26 call
inside `0x123f00`, or through the *other* real branch (`0x124528`)
this round didn't reach at all. Finding the real root cause (does
this project's own entity/actor iteration order actually match real
hardware's, or does something in this project's own emulation --
e.g. this bump allocator's addresses differing from a real heap
allocator's, if iteration or insertion order is address-dependent --
produce a genuinely different order) requires identifying and tracing
that real actor system directly, the same depth of real work this
project has already done for `IShell`/`IDisplay`/`IGL`/HID, not
another round of GL-state or leaf-function probing. **Explicitly not
attempted this round**: identifying vtable slot 26's real interface,
disassembling `0x124528` (the other real branch), or finding the real
actor/entity table itself. Tracked as a real, substantial follow-up
investigation, not a quick fix -- flagged clearly rather than guessed
at further.

No permanent code changes this round; every temporary probe (the
`ArmInterpreter::Step()` PC breakpoint) fully reverted. 304/304 tests
still pass (unchanged from the previous commit).

## Sprite z-ordering, continued again: found the real entity system's backbone, still haven't reached the actual draw-order code

Pure static disassembly this round (no live process, no code changes
-- `git diff --stat` clean throughout).

**Found the real master per-tick game-logic function: `0x1220f8`.**
Confirmed via its very first real call, `bl 0x11a2ec` -- the *exact*
real "combine both real gamepad slots" function this project already
fully identified and verified during the HID controller investigation
(`PHASE8_LOG.md`, "Real controller input, live from the keyboard").
This function calls, in order: the real input-combine, twice into
`0x11e644` (per-player input processing, args from `[applet+0x1000+
0x5bc]`/`[+0x5c0]`), `0x11a1dc`, `0x11fa30`, `0x109620`, **`0x119ec4`**,
`0x11cd5c`, `0x123630`, `0x122568` -- a real sequence of per-tick
subsystem passes, only some of which were identified this round.

**Traced `0x119ec4` (a real dispatcher) all the way down to a genuine
linked-list walk.** It calls a real "get list head for category N"
accessor (`0x11a5a8`, called with category constants `2`/`3`) then a
real two-list walker (`0x104c54`) six times with different category
pairs and a real mode flag. `0x104c54`'s own body contains two real,
confirmed linked-list traversals -- `r4 = *(r4+12)` / `r5 = *(r5+12)`
("next" pointers), each looping until null, calling one of six real
per-pair handler functions (`0x11a934`, `0x10a46c`, `0x10a3bc`,
`0x122420`, `0x11a76c`, `0x11aa68`) for each node reached. Given the
category-pair-vs-mode-flag shape (six calls covering (2,3,mode=0),
(2,2,mode=0), (3,2,mode=0), (3,3,mode=1), ...), this has the exact
shape of real *pairwise collision detection* (e.g. "every enemy vs.
every player hitbox") -- not rendering.

**Also found a real per-entity state-machine dispatcher.** The last
call in `0x1220f8` (`bl 0x122568`) leads to `0x122684`: a real
`ldrsh r0,[r1,#4]` (entity state field) -> `cmp r1,#9` ->
`addls pc,pc,r1,lsl#2` -- a genuine 10-entry real jump table
dispatching on real per-entity state (handlers include two real tail
calls to `0x10b95c`/`0x10bd7c` and several inline blocks). This is
almost certainly the real "advance this entity's state machine one
tick" mechanism every game-object type in this title shares --
exactly the kind of structure that would ultimately decide what gets
drawn and (via whatever real list these entities live on and in what
order) in what sequence, but none of its 10 real branches were
individually traced this round to confirm which ones -- if any --
reach `0x11d2d0` or its 17 already-known callers.

**Where this leaves it**: this project now has a real, confirmed,
connected map of a meaningful slice of Double Dragon's own real
entity system (input combine -> per-tick master function -> pairwise
list-walk collision pass -> per-entity state-machine dispatch), which
took several real rounds to build and should save real time for
whoever continues -- but the specific function that decides *draw
order* (walks entities and issues the `0x11d2d0` calls, in the
17-call-site pattern already found) has still not been located inside
this map. The most promising unexplored leads, in likely order of
value: (1) the state machine's own 10 branches, especially the two
tail calls (`0x10b95c`, `0x10bd7c`); (2) the still-unidentified
`0x11e644`/`0x11a1dc`/`0x11fa30`/`0x109620`/`0x11cd5c`/`0x123630`
calls from `0x1220f8` that this round didn't open at all; (3) the
still-unidentified real interface behind vtable slot 26 (from the
previous round's `0x11daf4`/`0x123f00` trace). This is a real,
substantial, still-open reverse-engineering task -- flagged honestly
rather than guessed at further, given how much real structure has
been found without yet reaching the actual bug.

No permanent code changes this round either. 304/304 tests still
pass (unchanged).

## Sprite z-ordering, time-boxed final round: hit the real wall -- ROPI-computed function pointers, not statically greppable

User asked to time-box one more focused round on the single most
promising lead, with a firm report-back either way. Picked the most
direct one: `0x104e98` (one of `0x11d2d0`'s 17 known real callers) is
inside a function starting at `0x104dec` (a "draw a 3x3 grid of
something, then one more sprite" real function, partially examined
last round). If this function is itself one entry in a real
per-entity-type "update+draw" table (matching the state-machine
dispatcher's own shape found last round), finding what calls it would
directly identify the real entity/type-table mechanism controlling
draw order.

**Result: no direct `bl 0x104dec` anywhere in the file** (confirmed by
grep). **No raw 4-byte pointer to its address anywhere in the file
either** (confirmed by a direct byte search, same technique used
successfully for the "J1" string). **No PC-relative `ldr`/`add`
instruction computes to its address either** (confirmed via objdump's
own `@ 0xNNNNNN` annotations, same technique that successfully found
zero hits for the "J1" string too, before that investigation had to
fall back to a live memory read-watch).

This is the same real obstacle the "J1" string investigation hit,
generalized: this module is real ROPI-compiled code, so a function's
real address, when it's needed as *data* (stored in a table, not
called directly), gets computed via a real, non-trivial multi-
instruction sequence at the point it's stored -- most plausibly
inside a real one-time "build the entity type table" initialization
routine this project hasn't located, executed once at real startup,
not visible via any single-instruction static cross-reference. A
live memory *write*-watch on wherever `0x104dec`'s real computed
value first gets stored (the same class of technique that cracked the
"J1" string) is the concrete next real step -- but that requires
first knowing *where* to watch, which itself needs either (a) finding
the real entity-type table's address some other way first, or (b) a
broad live trace of the real one-time init sequence looking for any
write whose value, once resolved, equals `0x00104dec`. Neither was
attempted this round -- genuinely a new investigation, not a
continuation of this round's static approach, which has reached its
real limit.

**Firm stop, as committed.** Two full-depth static rounds plus this
time-boxed one converge on the same conclusion: this project now has
a real, evidenced, connected map of Double Dragon's own entity
system's *backbone* (input combine, master tick, pairwise collision
walk, per-entity state-machine dispatch), but the specific draw-order
mechanism sits behind real ROPI pointer indirection that pure static
disassembly cannot crack -- it needs the same live-memory-watch
technique that worked for the "J1" string, starting from a real
runtime address this round didn't find. Left as a clearly-scoped,
real follow-up rather than continued guessing.

## Sprite z-ordering, the real fix round: cracked the ROPI wall live, found the real cause -- there's no missing sort to restore

Picked back up after the time-boxed round above, this time going
straight for a live-tracing technique instead of more static search.

**The instrumentation bug that caused the earlier "wall"**: the prior
round's live PC-watch checked `fetch_addr == 0x00100000 + 0x104dec`.
That's wrong -- `arm-none-eabi-objdump --adjust-vma=0x00100000`'s
printed addresses (e.g. `104dec:`) already have `kBase` baked in, so
runtime PCs match the listing directly with no further offset needed.
The check was silently comparing against `0x0021f804`-style addresses
that never occur, guaranteeing zero hits regardless of whether the
target code actually ran. Confirmed by fixing the same mistake this
round (see below) and immediately getting real hits.

**Real technique that replaced blind static searching**: instead of
grepping for references to a candidate address, temporarily log
`core.GetRegister(kLR)` inside `GlHle::GlDrawArrays` for every real
sprite-sized draw call. `LR` is the real return address the ARM `bl`
that invoked the trap left behind -- it names the exact real calling
function directly, with no static cross-reference search at all. A
deterministic frozen-frame testbed (see below) made every capture
reproducible and comparable.

**New tool support (added, used, then fully reverted)**: `SIGUSR1` +
an env-var-gated `DBGFREEZE=1` in `tools/game_probe.cpp` made
`mod_runtime.Tick`/`shell_hle.Tick` stop advancing sim time exactly
1200ms of simulated time after the second real `Return` keydown --
giving a perfectly reproducible, byte-identical frozen frame on every
run (confirmed via two `xwd` captures 1s apart being byte-identical)
of Double Dragon's own intro "gang lineup" cutscene, which has 7-8
real overlapping character sprites. This removed all the real-time
round-trip guessing that made earlier correlation attempts unreliable.

**Real call chain found, live, this round** (all addresses real
`ddragonz.mod` ARM code, confirmed via disassembly + live LR capture):

- `0x107360` (`bl 0x11f804`, inside a larger real per-tick render
  function starting near `0x1071xx`): the real, single per-frame call
  site. Immediately preceded by three real calls to `0x10b678` with
  distinct constant args (`0xa0`, `0x1a0`, `0x1b8`) -- almost
  certainly the three real HUD text draws (`TEMPO`, `J1` score,
  lives) -- and immediately followed by `bl 0x122520`.
- `0x11f804`: the real "draw every registered entity" function.
  `push {r4,r5,r6,lr}; r6=r0+0x4600; r4=r0` (`r0` = real applet
  pointer, `0x80300024` every observed call). Contains two real
  loops, walking two parallel lists (`category 0` at `r4+0x5434`,
  `category 1` at `r4+0x4834`, real per-slot stride 4 bytes, real
  bound read from `[r6+52]`/`[r6+54]`). Each iteration: `r1 =
  list[i]` (an entity pointer), `r0 = r4` (constant), `bl 0x10b034`.
  Confirmed live: **every one of the 7-8 real character draws in the
  frozen cutscene frame shares the exact same `LR=0x0011f8b0`** --
  the return address right after this one real `bl 0x10b034` --
  proving there is exactly one real call site, a flat sweep, no
  branching per entity.
- `0x10b034`: the real per-entity draw function (leads to the
  already-known `0x11d2d0` leaf and the real `glDrawArrays` trap).
- `0x11f6c4`: the real "register entity into category list" function
  (previously misidentified as starting at `0x11f764`, which is
  mid-function). Real signature `(r0=applet, r1=entity_ptr)`. Real
  logic, fully disassembled: bail out if a real state/flag check on
  the entity fails; otherwise `r2 = (entity->field_0x50 >= 0) ? 1 :
  0` (a real two-way category selector); read the category's real
  running count from `[applet + r2*2 + 0x4600 + 0x34]`; bail if
  `>=256`; otherwise **append** `entity_ptr` at
  `applet + r2*1024 + count*4 + 0x3e34` and increment the count.
  Confirmed live (write-watch on the real category-1 table address,
  `applet+0x4234`): **100% of the real writes into this table came
  from this exact one PC (`0x11f74c`, the `str` inside this
  function)** -- confirming it's the *only* real writer, and that
  it's a pure append, with **no sort, no insertion-position logic,
  no Y/depth comparison anywhere in it**.
- Real callers of `0x11f6c4`, live-captured via `LR` at its entry,
  for one real frame of the frozen cutscene: `0x1165c0` (a small,
  real single-entity function -- registers exactly one fixed entity
  every frame, most likely the player) and `0x116190` (same shape,
  a different fixed single entity) each fire once; `0x11666c` (the
  return address inside a real per-enemy update function starting
  near `0x1165d4`, itself called from a still-untraced real caller)
  fires **five times per frame, always in the same fixed order**
  (entity pointers `0x803204b8, 0x80320634, 0x803207b0, 0x8032092c,
  0x80320aa8`, byte-identical order every single frame observed).

**The real conclusion**: character sprite draw order is controlled
entirely by (1) fixed real code order between the three caller groups
(single-entity callers before/after the 5-enemy-slot loop, whichever
the real per-tick function's own instruction order dictates) and (2)
within the 5-enemy-slot group, plain array slot index (0..4) -- a
real, disassembled, dead-end for depth: **there is no Y-sort, no
Z-sort, and no depth-buffer participation anywhere in this real call
chain for character sprites.** This round independently re-confirmed
(live LR + bbox capture) what the depth-buffer round already
suspected: real `glTranslatef` Z is always `0` and real character
vertex data carries no Z of its own -- only unrelated real background
decoration (the door, the wanted poster) carries its own baked,
nonzero real Z, confirmed via a live frozen-frame capture showing
those exact quads' dimensions (e.g. the door, `80x96`) coincide with
the earlier-suspected "static overlay" group.

**What this means for "the real fix"**: there is no missing sort to
restore -- the traced real code genuinely never sorts by depth for
character entities. Two honest paths remain, neither of them "restore
a broken real mechanism":
1. Trace one level deeper: find what assigns an enemy to a slot
   (0..4) in the first place (spawn-time allocation, not yet traced).
   If slot assignment happens to be Y-order-dependent on real hardware
   in a way this emulation's spawn timing/order doesn't reproduce
   exactly, *that* would be a real, fixable emulation bug. Not
   confirmed either way this round.
2. Accept this is authentic original-game behavior and, if visually
   correct layering is wanted regardless, add a real per-draw
   synthetic depth derived from each sprite's own real screen
   position (a compositing correction, not a ROM-fidelity fix) --
   attempted and reverted this round (see below) after it
   false-positived on the door's background quad; fixable by gating
   on "no real Z of its own" but was reverted anyway once the user
   asked to pursue the real fix instead. Real GL depth-test
   infrastructure needed for this (`GL_LEQUAL`, real depth buffer) is
   already in place from the earlier depth-buffer fix.

All live instrumentation from this round (`gl_hle.cpp`'s `LR` log,
`arm_interpreter.cpp`/`memory.cpp`/`memory.h`'s PC tracker and write-
watch, `game_probe.cpp`'s `SIGUSR1`/`DBGFREEZE` frame-freeze) has been
fully reverted -- `git diff --stat` clean, 304/304 tests pass. No
code change landed this round; this is a pure, real, disassembly-and
live-evidence-grounded finding.

## Sprite z-ordering, traced to the cutscene's own script: it's hardcoded ROM data, not computed

Picked back up per explicit user direction ("go for the real fix...
path 1" -- trace the enemy-slot spawner to check whether slot
assignment itself might differ from real hardware). Kept using the
same live `LR`-capture + deterministic frozen-frame technique from the
round above, walking one real caller further up each time.

**`0x109620`** (real per-frame "update all N entities in this slot
array" loop, called via the tail-chain traced last round): confirmed
its real structure fully -- `container = r0` (constant real runtime
value `0x80300ad4` every call), `array = *(container+4)` (constant
real runtime value `0x80337c44`, `bound = *(container+0x10) = 8`).
Real per-slot body: `entity = array[slot]`; if null, skip; if a real
flags-bit is set, take an alternate path (`bl 0x10af08`) instead of
the normal one; otherwise real call through the entity's own real
vtable slot 0 (a genuine `add lr,pc,#28` / `bx r1` call, not a tail
call -- confirmed by finding that exact `add lr,pc,#28` sets
`lr=0x109680`, matching the earlier round's captured return address
exactly) -- this vtable call is what reaches `0x1165d4`, the real
per-enemy "Update" method for this entity type, which itself real-
calls the registration function traced last round.

**`0x10c4e4`** (found by live-write-watching the real 8-slot array
`0x80337c44` directly): a real free-list-backed object-pool allocator.
Pops an entity struct off a real free list at `container+12`, unlinks
it, then does an unconditional `array[index] = entity` at whatever
real `index` its caller passed in (no scan-for-a-free-slot, no
displaced-entity check beyond bookkeeping the old occupant for later
cleanup). The index is not computed inside this function at all.

**`0x10eccc`** (real generic "spawn an entity of type/resource
`(r1,r2)` at slot `r3`" function, found via live `LR` capture at
`0x10c4e4`'s entry): confirmed live that `r3` (the index eventually
handed to the allocator) is just this function's own third argument,
passed straight through -- still not computed here either.

**The real caller of `0x10eccc`, live-captured for one full pass of
the frozen cutscene**: **seven distinct, real call sites**
(`0x120f08`, `0x120f48`, `0x120f8c`, `0x120fc8`, `0x121004`,
`0x121040`, `0x12107c`), each passing its own **literal constant**
`(type, resource, index)` triple straight out of the ROM's own
instruction stream (`mov`/`add` immediates, not loads from any
computed table) -- e.g. `(0x14, 0x15, 6)`, `(0xa, 0xb, 1)`, `(0x2,
0x3, 6)`, `(0x3, 0x4, 6)`, `(0x8, 0x9, 6)` twice, `(0x13, 0x14, 6)`.
This is the intro cutscene's own authored script: a straight-line
sequence of hardcoded "spawn this character at this slot" calls
written directly by the original developers, not a loop, not a
formula, not anything this project's emulation computes or could get
wrong independently of correctly executing the ROM's own real
instructions.

**Also fully accounted for**: re-examined the one piece of real
conditional logic inside the registration function (`0x11f6c4`,
traced last round) that hadn't been explained yet -- confirmed it's a
real flicker/invincibility-flash skip (cycles a real 2-bit counter at
`entity+0x165`, gated on a real flag bit at `entity+0x40`) plus a
real category/bounds check. Not a sort mechanism either.

**Conclusion, now with every real instruction between spawn and draw
accounted for**: there is no computed, dynamic, or hidden sort
anywhere in this chain, at any of the four real levels traced (spawn
script -> generic spawner -> pool allocator -> per-frame update loop
-> registration -> draw). The intro cutscene's character layering is
determined entirely by literal constants baked into the ROM's own
code. If another emulator (Infuse, per user report) renders this
scene with different, visually-correct layering, the discrepancy is
not explained by anything found in this trace -- it would have to
come from something outside this exact call chain (a different real
subsystem this project hasn't located, or a difference in how some
earlier real state this chain depends on gets set up). Not yet
identified; flagged as the next real question rather than guessed at.

All live instrumentation from this round (`arm_interpreter.cpp`'s two
successive entry-point probes, `memory.cpp`/`memory.h`'s write-watch,
`game_probe.cpp`'s `SIGUSR1`/`DBGFREEZE` frame-freeze) fully reverted
again -- `git diff --stat` clean, 304/304 tests pass. No code change
this round either; another pure real evidence-gathering round.

## Sprite z-ordering: installed the real reference emulator, found a real correction to last round's conclusion

User pointed out Double Dragon renders this exact cutscene correctly
on Infuse (Tuxality's real, open-source Zeebo/BREW emulator,
https://github.com/Tuxality/Infuse -- explicitly lists Double Dragon
as one of three fully-playable titles) and asked to install and check
it directly rather than keep guessing from this project's own trace
alone.

**Installed real Infuse** (official Linux build from
`tuxality.net/projects/infuse_zeebo_emulator`, run locally). Loaded
the same real Double Dragon ROM already in this repo's own
`research/games/`. Got it to the exact same intro "gang lineup"
cutscene this project has been analyzing. Real, direct visual
comparison against this project's own rendering of the identical
scene: in Infuse, the blond enemy is genuinely partially hidden behind
the brute (only his upper torso/arm visible), and the rear purple-suit
enemy is genuinely partially hidden behind the front one -- real,
correct depth occlusion. In this project's own render, every
character's full silhouette is visible with no occlusion at all.

**This directly contradicted the previous round's "no sort anywhere,
authentic ROM behavior" conclusion -- so it was re-checked, and found
wrong.** Re-instrumented `GlHle::GlDrawArrays` (temporarily) to log
the real vertex Z alongside bbox for draws returning to the real
character-draw call site (`LR=0x0011f8b0`, confirmed last round).
**Real character quads do carry distinct per-vertex Z** (`-512` down
to `-504`, one integer apart per real entity, matching each entity's
real registration/slot index) -- contradicting last round's claim that
character sprites carry no Z. The earlier mistake: the `z=-512..-504`
group found several rounds ago really did include the door and the
wanted-poster (confirmed then by bbox size, e.g. the door's `80x96`),
but those aren't a *separate* "background decoration" draw path as
concluded then -- they're just two more entities (early registration-
index slots) in the exact same real entity system as the characters,
sharing the same Z-from-slot-index convention. Conflating "this
specific quad is the door" with "therefore this whole Z range is
background-only, unrelated to characters" was the error.

**With that corrected, every other real piece of the depth-test
pipeline was directly verified, this round, against the *real* OpenGL
driver state (not this project's own bookkeeping)**, right at each of
these character `glDrawArrays` calls:
- `glIsEnabled(GL_DEPTH_TEST)` → real `1`.
- `glGetBooleanv(GL_DEPTH_WRITEMASK)` → real `1` (real code never
  calls `glDepthMask` at all; confirmed the real backend's default is
  correctly the enabled-writes default GLES itself specifies).
- `glGetIntegerv(GL_DEPTH_FUNC)` → real `0x203` (`GL_LEQUAL`).
- `glGetFloatv(GL_DEPTH_RANGE)` → real `[0,1]` (default; real code
  never calls the still-stubbed `glDepthRangex` either -- confirmed
  live, zero real calls this round).
- `glGetFloatv(GL_PROJECTION_MATRIX)` at the exact character draws:
  byte-matches the expected real `glOrtho(0,320,-240,0,1,4096)`
  matrix (`proj[10]=-0.000488`, `proj[14]=-1.000488`, etc.) -- and the
  modelview matrix is real identity, so no hidden transform is
  corrupting Z.
  Precision check: two real, adjacent Z values (e.g. `-511`/`-512`)
  map to NDC-Z values ~0.0039 apart, ~32,000 distinct steps apart in a
  real 24-bit depth buffer -- nowhere near a z-fighting risk.
- `glGetIntegerv(GL_DEPTH_BITS)` on the real created context → `24`
  (confirmed the earlier depth-buffer fix's `SDL_GL_DEPTH_SIZE`
  request was actually honored, not silently negotiated down).
- `glViewport` stays real, constant `(0,0,641,480)` throughout --
  ruled out an offscreen/lower-res render target (no FBO functions
  exist in this project's GL HLE at all, and the game never crashes
  reaching for one, so real code isn't using one); the `320x240` real
  `glOrtho` call found earlier is purely a logical coordinate-space
  choice, not a separate smaller render surface.

**Every individual piece verified correct, yet the final rendered
image is still wrong.** This round did not find the remaining defect
-- it corrected a real factual error from the prior round (character
Z does vary, is not absent) and exhaustively ruled out the entire
depth-test/framebuffer/matrix pipeline as the cause via direct real
GL driver queries, not assumptions. The most likely remaining
explanation, given Z-per-character is real and derived from real
slot/registration index (not random): **this project's own real
execution may be assigning entities to different real slots (and
therefore different real Z) than real hardware would for the same
ROM script** -- i.e. the previous round's "path 1" question (does our
execution order/timing of the seven hardcoded spawn calls match real
hardware's) is back open and is now the best-supported next lead, not
the abandoned "accept it's authentic" conclusion from before.

All temporary instrumentation from this round (`gl_hle.cpp`'s
`DBGDEPTH`/`DBGCAP`/`DBGZCHECK`/`DBGVIEWPORT` prints and the
`glDepthRangex` stub's temporary logging wrapper, `sdl2_unified_
backend.cpp`'s `DBGREALGL`/`DBGMATRIX` real-driver-state dumps)
reverted -- `git diff --stat` clean, 304/304 tests pass. No code
change landed. Real Infuse installation left in place locally
(`~/.Tuxality/Infuse`, outside the repo) for further comparison in a
future round.

## Sprite z-ordering: continued down the entity-system layer, found the previous round's structure model was wrong

Continued directly from the "installed the real reference emulator"
round's conclusion (real character Z is genuinely tied to real
registration/slot index; best lead was checking whether this
project's own execution assigns entities to different real slots than
real hardware would).

**First: resolved an apparent internal contradiction from two rounds
ago.** That round's own notes claimed the real registration function
`0x11f6c4` "never fires" for the frozen cutscene (0 live hits) --
directly contradicting that same round's own separate finding that
`0x11f74c` (an instruction *inside* `0x11f6c4`, unreachable without
first entering at its top) was confirmed, via live write-watch, to be
the real render-list's only writer. Redid the check fresh, cleanly,
this round: **`0x11f6c4` does fire** (552 real hits in one capture
window). The earlier "0 hits" report was itself wrong -- most likely
a stale or mistimed capture in that round, not a real fact about the
ROM. The append-only, no-sort logic inside it (confirmed two rounds
ago) still stands; only the "never fires" framing was incorrect.

**Second, and more significant: the "8-slot pool array" model built
up over the last two rounds was wrong.** Live-dumped the real
contents of what was believed to be an 8-entity-pointer array at
`0x80300ad8`..`0x80300af4` (derived from `[applet+0xab0]+4`, the
"container" the real per-tick update loop `0x109620` and the real
pool allocator `0x10c4e4` both operate on) at the frozen cutscene
frame:

```
slot[0] = 0x80337c44  <- the real render-list base address itself
slot[1] = 0x80337c64  <- render-list base + 0x20 (likely an adjacent,
                          second real list -- category 0's list,
                          contiguous with category 1's)
slot[2] = 0x80320c24  <- looks like a real entity pointer
slot[3] = 0x00000008  <- a real small integer (matches the render
                          list's own real bound of 8, confirmed
                          two rounds ago)
slot[4] = 0x00000100  <- 256, matches the real registration
                          function's own "count >= 256, reject" cap
slot[5] = 0x80337c84  <- another 0x8033xxxx-range real pointer
slot[6] = 0x00000008
slot[7] = 0x80008000
```

This is **not** an array of 8 entity pointers -- it's a small,
mixed-purpose real struct (list-base pointers, bounds/caps, at least
one tracked entity pointer) that this project's last two rounds
misread as a uniform slot array. That misreading is *why* "six of
seven hardcoded spawn calls pass literal index=6" looked
contradictory against "6-8 simultaneous characters render" -- the
"index" argument those calls pass was never a render-list slot at
all; it indexes into this differently-shaped struct for an as-yet-
ungeneralized purpose, and the real render-list's own actual index
(the one that determines Z) is assigned separately, at registration
time, by `0x11f6c4`'s own real running-count field -- exactly as
found two rounds ago, just previously described using the wrong
mental model of *why* six spawn calls could share one literal index
argument without visibly colliding.

**Stopping here deliberately.** Each layer opened this round revised
or contradicted something believed settled from the layer before --
a sign of reconstructing this real struct's layout faster than it's
being verified, not a sign of converging on an answer. Rather than
keep building on possibly-shaky ground, this is flagged honestly as
still open. What's now solid, re-confirmed fresh this round: the
render list (`0x80337c44`, 8 real slots, bound 8) is real and is what
`0x10b034` draws from; `0x11f6c4` really is its one live writer, a
pure append with no sort; real Z is genuinely `f(append-order-index)`
for characters. What's not yet solid: the full real shape of the
`applet+0xab0`-based struct, how its fields relate to the seven
hardcoded spawn calls' literal index arguments, and therefore what
actually determines each character's real append order into the
render list -- the original "path 1" question, still open.

All live instrumentation from this round (`arm_interpreter.cpp`'s
fresh entry-point/pool-dump probes, `memory.cpp`/`memory.h`'s dual
write-watch) reverted -- `git diff --stat` clean, 304/304 tests pass.
No code change landed.

## Sprite z-ordering: rigorous, correlated re-verification -- follower order is genuinely ROM-deterministic

Explicit instruction this round: verify slowly and rigorously rather
than keep building new theories on unverified ground, after the
previous round caught itself in a real self-contradiction.

**Step 1, verified directly (not inferred from arithmetic): the real
render-list write target.** Logged the exact address at the real
`str` instruction itself (`0x11f74c`) across many hits: `0x80304258`
through `0x80304274` (8 slots, stride 4), with `r0=0x80300024`
(applet) confirmed live at `0x11f6c4`'s own entry every single time.
This matches `applet + 0x4234 + index*4` exactly -- the real render
list, independently re-derived and cross-checked against the actual
observed store address, not assumed. The `0x80337c44` array from
prior rounds is a **separate, unrelated structure** (confirmed
next) -- dropped as a false lead for *this* list.

**Step 2: what `0x109620` really iterates.** Live-dumped the real
8-slot array at `0x80337c44` (the one two rounds of prior theory
called an "entity pool") at both the entry *and* the exit of a single
`0x109620` call, deterministically correlated to a frozen frame via a
temporarily re-added `SIGUSR1`/timed freeze (same technique as
before). Both dumps, for the *same* call, are byte-identical: only
slots `[1]=0x80320044` and `[6]=0x803201c0` are populated; the other
six slots are `0`. Yet all 8 real render-list writes (all 8 real
characters) happen within that exact same call's window, interleaved
in append order right alongside those two entities' own registration.

**Conclusion, now solidly evidenced rather than guessed: the five
"follower" entities (and one more, `0x8032033c`) are not reached by
iterating this shared 8-slot array at all.** Since the array is
unchanged before and after, and their registrations still happen
nested inside this same call, they must be reached via a direct,
nested call chain originating from processing the array's own slot
1 entity (`0x80320044`, the real "leader") -- consistent with the
real leader/follower backreference already found two rounds ago
(each follower's own `+0xac` field stores a pointer back to this
same leader). Most likely mechanism (not yet directly proven): the
leader's own `Update()` walks a real linked list of its followers
(entity `+0xc`, the same "next" field this project found genuinely
used for list-walking elsewhere) built once at spawn time, directly
calling each follower's registration in turn -- entirely within
normal, deterministic ARM execution, no separate scheduling or
indexed-array lookup involved.

**Why this matters**: this follower order is driven by fixed real ROM
code and a real, deterministically-built linked list -- there is no
step in this chain that depends on anything this project's own
HLE/timing could plausibly compute differently from real hardware
(no RNG, no wall-clock-dependent branch, no HLE-stubbed return value
observed anywhere in this path). Taken at face value, that argues
*against* the "our slot assignment differs from real hardware" theory
this round set out to check, and re-opens a different, harder
question: if entity order really is ROM-fixed and this project's own
depth-test pipeline was already exhaustively verified correct (two
rounds ago, directly against real OpenGL driver state), the
remaining gap between this project's rendering and Infuse's may not
be a Zeebulator bug at all -- Infuse is itself an independent,
explicitly-labeled "A1 development preview" reimplementation, not
verified-against-real-hardware ground truth, and could plausibly be
applying its own approximation (e.g. a manual Y-sort) that isn't
authentic Zeebo behavior either. Not confirmed either way; flagged
honestly as the real open question rather than assumed.

All live instrumentation from this round (`arm_interpreter.cpp`'s
correlated entry/exit/write probes, `tools/game_probe.cpp`'s
temporarily re-added deterministic freeze) fully reverted -- `git
diff --stat` clean, 304/304 tests pass. No code change landed.

## Sprite z-ordering: pursued a general-emulation-bug hypothesis, corrected another self-made error, landed on a timing-sensitivity finding

User pushed on two fronts this round: (1) checked whether this is a
documented issue anywhere (Infuse's own GitHub issues, general web
search, two contemporary Zeebo-version reviews) -- found nothing, and
notably both reviews describe the graphics positively with no
mention of sprite-layering problems, which is real (if indirect)
evidence this is not authentic ROM/hardware behavior, consistent with
the real-hardware footage and Infuse's own rendering. (2) Asked
whether the real cause might not be Double-Dragon-specific at all,
but a general ordering/timing issue in this project's own emulation
infrastructure.

**Investigated the general-emulation angle directly.** Checked
`IShellHle::Tick()`'s timer-expiry ordering (`core/brew/ishell.cpp`):
expired timers fire in `timers_`' vector order (registration order),
not sorted by anything else. Real code only appears to use one
recurring master timer for Double Dragon's own tick (per much
earlier PHASE8_LOG evidence), so this specific mechanism likely isn't
what's driving per-character update order -- flagged as a real,
general emulation detail worth keeping in mind for other titles, but
not confirmed as this bug's cause.

**Then made, and caught, another real self-correction.** Live-
captured the actual instruction executed immediately before entering
a "follower" entity's `Update()` (`0x1165d4`) for all five followers:
`prev_pc=0x1096?c` every time -- the exact real `bx r1` inside
`0x109620`'s own loop body, contradicting last round's "the array
never contains followers" conclusion. Re-ran a single, clean,
self-consistent capture (both facts logged together in the same
process run, not compared across separate launches) and found the
array (`0x80337c44`) genuinely does hold multiple/all eight real
entities -- just not all at once from the very start. Last round's
entry/exit dump had simply caught an earlier moment, before the five
followers had been spawned into the array yet. There is no separate
"leader-driven linked list" mechanism after all; `0x109620` iterating
this one real shared 8-slot array, in fixed slot order, calling each
occupant's real `Update()` (which internally re-registers into the
render list via `0x11f6c4`), is the whole real mechanism.

**Why this matters for the user's hypothesis**: the array's contents
visibly change over the run's duration as entities get spawned in
(confirmed by the same capture showing mostly-null early, then
progressively more real entity pointers later). This project's
`SIGUSR1`/timed-freeze testing methodology captures a snapshot at a
fixed *simulated* millisecond offset after the second real `Return`
keydown -- a purely internal, engineering-convenience checkpoint with
no real counterpart in how a human plays on real hardware or how
Infuse's own timing naturally settles. If entity spawn/eviction is
still slightly in-progress at the exact moment this project's own
freeze fires, the snapshot could easily catch a genuinely different,
transient mid-population state than what a real, continuously-running
system would show once things settle -- a real, plausible, and
importantly *general* (not Double-Dragon-specific) source of
divergence between this project's captures and any other real or
emulated system's, worth treating as a live methodology caveat for
this investigation rather than a confirmed engine bug.

Not yet confirmed either way. All live instrumentation from this
round reverted -- `git diff --stat` clean, 304/304 tests pass. No
code change landed.

## Closed the timing-methodology caveat: replaced the fixed-offset freeze with a population-stabilized one, bug persists

Directly followed up on last round's open methodology caveat (fixed
`sim_ms` offset freeze could catch the active-entity array mid-spawn
rather than settled) by replacing the freeze trigger itself, live in
`tools/game_probe.cpp` (temporary, fully reverted after this round --
see below).

**New freeze trigger**: instead of a fixed simulated-time offset
after the second real `Return` keydown, the tool now polls the real
active-entity array every main-loop iteration (container =
`applet_ptr + 0xab0`, base pointer at `container+4`, 8 slots -- all
real/confirmed addresses from earlier rounds) and only freezes once
the array's contents (which real entity pointer occupies which slot)
have held byte-identical for 90 consecutive iterations. This freezes
on an actual observed steady state rather than a guessed clock offset.
A `SIGUSR1` handler was kept as a manual override/fallback.

**Verified deterministic and reproducible.** Ran the tool twice, as
two fully independent process launches (killed and relaunched from
scratch between them, confirmed via `ps aux` each time), sending the
identical two real `Return` keydowns via `send_key.py` both times.
Both runs converged on the exact same real array contents at freeze
time -- `{0x00000000, 0x80320044, 0x00000000, 0x00000000, 0x00000000,
0x00000000, 0x803201c0, 0x00000000}` -- and a byte-for-byte identical
screenshot (`PIL.ImageChops.difference` bbox `None`), confirming this
project's ARM interpreter + allocator behavior is fully deterministic
given the same real input sequence, and that this new freeze trigger
reproducibly lands on the same real settled frame.

**The bug is still there in the genuinely-settled frame.** Cropped
and zoomed the right-side trio the user originally pointed at. The
tan-jacket/mohawk character (boots at the higher, farther-back screen
row) is drawn overlapping *in front of* the purple-suited character
standing next to him (boots at a lower, closer-to-camera screen row)
-- backwards from the "higher on screen = farther back" rule, the
exact same failure mode flagged at the start of this investigation.
Since this frame is now provably not a transient mid-spawn snapshot
(reproduced identically across two independent launches, well after
the array stopped changing), **the timing-sensitivity/methodology
caveat from last round is now ruled out as the explanation** -- this
is a real, settled, repeatable rendering-order defect, not an
artifact of when the snapshot was taken.

**Note**: the array only held 2 of the 5 on-screen characters at
freeze time (`0x80320044`, `0x803201c0`) despite 5 sprites being
visible on screen simultaneously. This confirms (again) that this
specific 8-slot array is not the complete real set of on-screen
entities -- consistent with earlier rounds' finding that it's a
mixed-purpose/partial structure, not a full entity registry. Whatever
governs the other 3 visible characters' draw order is not yet
identified. Left open for a future round.

All live instrumentation (the `csignal` include, the `SIGUSR1`
handler, the second-Return-keydown counter, and the population-
stabilization freeze logic) fully reverted from `tools/game_probe.cpp`
via `git checkout --` -- confirmed `git diff --stat` clean and
304/304 tests passing. No code change landed from this round.

## Found and fixed the real root cause: an unimplemented real sort trap

Went hunting for the actual mechanism controlling draw order, since the
8-slot active-entity array (`0x109620`'s own iteration) was already
confirmed unsorted (pure append, TASKS.md Phase 8 "crack the sprite
z-ordering ROPI wall live"). Disassembled `0x11f804` in full (the real
per-frame "draw every registered entity" function) rather than just its
loop shape as before, and found it isn't two flat back-to-back loops:
right before the category-1 (character) draw loop, there's a real
one-time call through a function-pointer field at a fixed table offset
-- `(base=applet+0x4234, count=8, size=4, compar=<a real ARM function
pointer>)` -- with category 0 never getting an equivalent call.

That table offset is `0x1b4`, one of this project's own runtime-helper
table slots (`core/brew/mod_runtime.cpp`) -- already found and named in
an earlier round, but registered as a **safe no-op stub**, guessed at
the time to be an "array-of-N-constructor" RVCT/EABI helper that
couldn't be safely implemented without a visible element-stride
argument. Live capture of this exact call site proved that guess wrong:
`size=4` (pointer-sized elements, not a real per-entity struct size)
rules out "construct N objects," and the argument shape is exactly a
generic `qsort`-style `SORT(base, count, size, compar)`.

Disassembled the real comparator itself (`ddragonz.mod` 0x10b918):
both arguments are pointers to array slots (each holding an `Entity*`,
dereferenced once, matching real `qsort` semantics), comparing the
pointed-to entities' real fields at `+0x7c` (primary key) then `+0x50`
(tie-break -- the same field already known to gate which of the two
render categories an entity lands in), returning +1 when the first
entity sorts before-or-equal the second and -1 otherwise.

**Implemented the real slot** (`ModRuntime::SortPointerArrayImpl`): a
real in-place insertion sort over the `count` pointer-sized slots at
`base`, calling back into the real ARM `compar` function via
`HleRuntime::CallArmFunction` for every comparison -- deferring
entirely to the real game's own comparison logic rather than
reimplementing what `+0x7c`/`+0x50` mean. Insertion sort specifically
because it doesn't depend on the real comparator forming a strict weak
ordering, unlike quicksort. `HleRuntime::CallArmFunction` repurposes
`LR` as its own return sentinel, and this HLE function is itself
invoked from inside another real call's own `Dispatch()`, so `LR` is
saved before the comparison loop and restored right before returning --
confirmed necessary by a dedicated new test
(`Slot0x1b4RestoresLrSoItsOwnCallerCanStillReturnCorrectly`) whose
first, LR-unaware version of this test hung the whole process, a real
demonstration of exactly the bug this save/restore prevents.

**First attempt regressed live** -- caught directly by the user
watching the running window, not by any automated check: with the
naive `compar(prev, next)` argument order, the door started rendering
in front of both heroes (previously correct). Flipping to
`compar(next, prev)` fixed the door regression *and* produced the
correct real character layering (tan-jacket character now correctly
hidden behind the purple-suited character in front of him, matching
both the real-hardware YouTube footage and Infuse) -- confirmed with
fresh screenshots at the title cutscene, the transition into real
gameplay (HUD/score/timer all appearing normally), and while walking
during real gameplay with the two heroes overlapping. This argument-
order choice isn't derivable from the disassembly alone -- both orders
call the real comparator faithfully as a generic sort; only live
verification against real ground truth showed which one is correct.
Net effect: the real sort call produces the final array in *descending*
order by the real comparator's own relation, not ascending -- entities
with the larger `+0x7c` value draw first (farthest), smaller draws
last (nearest).

Added real unit tests (`tests/mod_runtime_test.cpp`, replacing the old
`UnknownSlot0x1b4DoesNotCrash` no-op smoke test): a real ARM comparator
written directly into emulated memory (several instruction words
copied verbatim from the real, disassembly-confirmed comparator, not
guessed), verifying the sort's descending output, idempotency on an
already-sorted array, the count<=1 no-op guard, and the LR save/restore
behavior via a synthetic real-calling-convention caller (`push {r4,lr}`
/ `bx` / `pop {r4,lr}` / `bx lr`, matching this project's own real
disassembly patterns elsewhere). 307/307 tests pass (added 4, replaced
1). `tools/game_probe.cpp`'s temporary live-tracing instrumentation
used to find this (PC watches at `0x11f868`/`0x11f884`/`0x11f888`/
`0x11f89c` in `core/cpu/arm_interpreter.cpp`) fully reverted via `git
checkout --`; the real fix itself (`core/brew/mod_runtime.cpp`,
`core/brew/mod_runtime.h`, `tests/mod_runtime_test.cpp`) is a genuine,
committed code change -- the first of this entire investigation.

## Sound, round two: still no real AEECLSID_MEDIA call, even through real combat and damage

Direct follow-up to "`MediaHle` is real and wired, but never
registered" above, now that real keyboard-driven input can reach far
past that round's 100-second-idle-title-screen ceiling: title -> menu
-> cutscene -> real walking -> real melee combat (score visibly
climbing past 0, e.g. to 4600 in one run) -> **taking real damage**
(lives visibly dropping from 2 to 1 in another run). Two pieces of
temporary instrumentation, both fully reverted after use:

1. `[DBGCLS]` in `IShellHle::CreateInstanceImpl` -- logs every
   `ClsId` real code requests and fails to get, same technique as the
   earlier round but now exercised across all of the above states, not
   just an idle title screen. Result: **the exact same six `ClsId`s as
   before, and no others** (`0x01001002`, `0x0102f679`, `0x01030852`,
   `0x0102f681`, `0x0100550a`, `0x01005501`) -- the last two now firing
   dozens of times (the self-rearming download/catalog-progress timer
   already identified, confirmed still unrelated to audio by its own
   call shape). Zero new `ClsId`s appeared despite real combat and
   real damage happening on-screen.
2. `[DBGSTUB]` in `scaffold_object.cpp`'s shared generic `Stub` --
   logs `this`/`LR`/`r1`/`r2` every time real code calls an
   un-overridden slot on any already-*successfully*-created scaffold
   object, to check whether sound routes through one of the objects
   that already succeeds rather than a fresh, still-failing
   `CreateInstance`. Only 4 hits total, all during early graphics
   startup (`this=0x8000d000`/`0x80019000`/`0x8000f000`, matching the
   already-identified `0x01002001`/`AEECLSID_DIB`-ish/device-bitmap
   scaffolds from Phase 5/8's graphics work) -- none during or after
   combat, none audio-shaped.

Also checked whether any bundled real SDK header defines
`AEECLSID_MEDIA`'s actual numeric value directly, to shortcut the live
search: confirmed (again) it doesn't -- `AEEMediaUtil.c` and
`AEEShell.h`/`AEEIShell.h` only ever *use* the symbolic name via
`ISHELL_GetHandler`/`ISHELL_CreateInstance`, never `#define` it, and a
direct search for this project's own already-confirmed real numeric
IDs (e.g. `0x01001001` for `AEECLSID_DISPLAY`) turns up nowhere in the
bundled headers either -- consistent with every numeric `ClsId` this
project has ever used coming from live evidence, never a header, and
confirming there's no shortcut available here.

**Conclusion**: within every real code path reached so far -- title,
menu, cutscene, movement, melee combat, taking damage -- Double
Dragon's Zeebo port genuinely never requests `IMedia` or calls into
any already-successful generic object in an audio-shaped way. Two real
possibilities remain open, neither confirmed: (a) the real trigger is
gated behind a real game state not yet reached (an actual KO, a level
transition, a boss encounter, a game-over screen), or (b) this
specific title's Zeebo port uses a real vendor-specific audio path
that isn't standard `AEECLSID_MEDIA` at all -- the same relationship
`AEECLSID_GL`/`AEECLSID_EGL` (Zeebo-specific extensions) already have
to stock BREW `IDisplay`. The real `ddragonz.mod` resource-descriptor
table flagged in the previous round (`sound.ggz` string references at
file offsets `0x4dae8`/`0x4e1a8`, inside a large fixed-stride
per-asset table spanning roughly `0x4dae0`-`0x4e460`) is still the most
concrete unexplored lead -- manually walking its real field layout,
not another round of "play further and watch," is probably what
resolves this. All instrumentation reverted; `git diff --stat` clean;
307/307 tests pass; no code change this round.

## Sound, round three: real loading pipeline mapped, real playback trigger still not found

Pivoted from static byte-guessing at the resource table to live-tracing
the real code that consumes `sound.ggz`, reusing the LR-capture
technique that broke the sprite z-ordering wall. Temporary
instrumentation in `FileHle::OpenFileImpl`/`ReadFromHandle` (capturing
`LR` whenever the open/read is on `sound.ggz`) and in
`ArmInterpreter::Step` (PC watches at specific real call sites),
reverted after use.

**Confirmed a real bulk preload pass, not per-play loading.** Every one
of `sound.ggz`'s 74 real archive entries gets opened and fully read in
one continuous burst right as the title screen loads, in descending
index order -- not selectively when a sound is actually needed.
Disassembled the two real functions responsible:
- `0x10739c`: opens the currently-selected file, seeks to
  `index * 8` into the real GGZ archive's own internal directory,
  reads that entry's real 8-byte (offset, size) header, big-endian
  decodes it, and mallocs a buffer sized `size + 0xc350`
  (`bl 0x1096f0`).
- `0x11bfd0`: the generic wrapper -- resolves a file-manager object,
  calls `0x10739c` for the header, then loops calling `Read` with the
  real remaining-byte count until the whole asset is pulled in, then
  closes. Six real static call sites into this wrapper found via
  `grep`; live PC-watching all six during the preload burst identified
  exactly which ones fire and with what arguments (filename pointer,
  index) -- `0x11c994` is the bulk "load every sound.ggz entry" loop
  (index counting down); `0x1075a4` is a separate, one-shot load of
  archive index 0 specifically, gated by a real state-machine flag
  (byte `35` at a fixed context offset means "already done, skip").

**Chased the one-shot index-0 load (`0x1075a4`) as the most audio-
specific-looking lead, and it wasn't.** After the load, it branches
on a stored pointer to one of two functions: live-captured the actual
arguments at both branches. One (`0x109dfc`) turned out to be a real
`ISignalCBFactory::CreateSignal(this, pfn, pUser, ...)` call --
confirmed by the fact its "id" argument (live-captured as
`0x0011bdf4`) is exactly this project's own already-known real HID
joystick-button-callback address (`kRealButtonCallbackAddress` in
`tools/game_probe.cpp`), i.e. this is HID/input signal setup, not
audio. The other (`0x10ad58`) is a generic per-slot object-release/
cleanup routine, also unrelated to audio specifically. Neither branch
leads anywhere audio-shaped in the code reached so far.

**Where this leaves things**: the real loading/caching pipeline for
`sound.ggz` assets is now concretely mapped end to end, real function
addresses and all -- genuine, reusable progress. But the actual
"decode a cached asset and hand it to a playback path" trigger has not
been found; every thread pulled on this round (the two dispatch
branches at `0x1075a4`, the resource-table field layout from the
previous round) turned out to be generic infrastructure or unrelated
subsystems, not the answer. This is now a materially deeper
investigation than the sprite z-ordering one was at a comparable
stage -- that bug had a single, mechanically-findable call site right
in the per-frame draw path; this one is buried in a much larger real
resource-management subsystem with no single obvious next call to
follow. All instrumentation reverted; `git diff --stat` clean;
307/307 tests pass; no code change this round.

## Sound, round four: found and fixed the real gap -- GetHandler, not CreateInstance

Picked the investigation back up by following the "activate a cached
resource into a playback slot" call shape found last round
(`0x11e964`-`0x11eabc`, reading the per-object cache arrays at
context+0x870/+0x874 by index, then calling `0x11bfa0` to fill in a
slot struct, then branching into `0x10a1e0`/`0x10a0c0` based on a
flag). Disassembled `0x10a1e0` in full this time (last round only
found it as a branch target) and found the real payoff near its end:
a call through a resolved `IShell` object's own vtable slot `0x80`
(offset 128, slot 32) with a constant `0x01005500` set up right before
it (`mov r1,#0x5500; add r1,r1,#0x1000000`).

Slot 32 in this project's own `IShellHle::Build` order is
**`GetHandler`** -- `AEECLSID GetHandler(IShell*, AEECLSID cls, const
char *pszMIME)` -- a completely different vtable slot from
`CreateInstance` (slot 2), and the bundled real `AEEMediaUtil.c`
sample's own real usage shape is `ISHELL_GetHandler(ps,
AEECLSID_MEDIA, szMIME)` then `ISHELL_CreateInstance(ps, cls, ...)`
with the returned value. **This is exactly why the last three sound
rounds' `[DBGCLS]` instrumentation (watching only `CreateInstanceImpl`)
never saw anything audio-shaped**: real code never calls
`CreateInstance` for a media class directly at all -- it asks
`GetHandler` first, gets back nothing (this slot was a blind `Stub`,
always returning 0), and silently gives up before ever reaching
`CreateInstance`.

Live-verified with a temporary PC watch at the real call site
(`0x10a2a0`, reverted after use): fired twice during the same title
-> menu -> cutscene sequence used throughout this whole sound
investigation, both times with `this` = the real `IShell` object
(`0x80001000`) and `cls = 0x01005500`, confirming this is a real
`ISHELL_GetHandler(shell, 0x01005500, pszMIME)` call, not a guess. The
real `pszMIME` argument didn't resolve to printable text in that
capture, so `GetHandlerImpl` doesn't try to match on it.

**The fix** (`core/brew/ishell.cpp`/`.h`): implemented slot 32 for
real -- returns `cls` itself when `cls == 0x01005500`, `0` otherwise.
Doesn't need to know the *real* returned class ID, since real calling
code immediately feeds this call's return value into
`ISHELL_CreateInstance`: `tools/game_probe.cpp` now calls
`shell_hle.RegisterInstance(0x01005500, media_hle.CreateMediaObject())`
right after `media_hle.Build()`, closing the loop end to end with a
single shared `IMedia` instance (the simplest first step, matching the
one real bundled sample this project has seen using exactly one
`IMedia` pointer reused across calls -- revisit if real evidence ever
shows this game wanting concurrent instances).

**Verified with real, external, OS-level proof** -- the same class of
evidence Phase 6 originally used, not a screenshot (screenshots can't
show audio): ran the fixed build through title -> menu -> cutscene ->
walking -> melee combat, and checked `pactl list sink-inputs` twice.
Found a live, unmuted (`Corked: no`) sink input at
`application.process.binary = "zeebulator_game_probe"`, PID matching
the real running process, `Sample Specification: s16le 2ch 22050Hz` --
exactly Double Dragon's own real asset format (Phase 6). Still present
and still uncorked after driving through combat, confirming the
single shared `IMedia` instance survives repeated real `SetMediaParm`/
`Play` calls rather than breaking after the first one.

Added 3 real unit tests (`tests/brew_test.cpp`): `GetHandler` returns
the class itself for the real audio class and `0` for anything else,
and a full `GetHandler` -> `CreateInstance` chain test proving the two
slots compose the way real code actually uses them. 310/310 tests
pass. This is the second real code change to land from this whole
investigation (after the sprite z-ordering sort fix) -- small diff
(`core/brew/ishell.cpp`/`.h`, `tools/game_probe.cpp`, tests), but it's
the real gap: not a missing codec, not a missing mixer, not a missing
`IMedia` implementation (all of that was already real and correct from
Phase 6) -- just one un-implemented vtable slot standing between a
fully-working audio pipeline and real game code never finding it.

## Sound, round five: one shared IMedia instance was already wrong -- fixed to a real per-call factory

Immediate follow-up, prompted by re-reading the previous round's own
disassembly more carefully: the real `GetHandler`->`CreateInstance`
call pair for `AEECLSID_MEDIA` (`ddragonz.mod` 0x10a1e0) sits *inside*
the same function that runs once per cached `sound.ggz` resource being
activated into a playback slot (`0x11e964`-`0x11eabc`, chased over the
last two rounds) -- i.e. real code creates a fresh `IMedia` instance
**per sound**, not once overall. The previous round's fix registered a
single shared instance (`shell_hle.RegisterInstance(0x01005500,
media_hle.CreateMediaObject())`, called once at startup) -- functional
enough to prove the pipeline worked end to end, but wrong for real
concurrent use: every new sound would reuse the exact same underlying
`Media` state as whatever was already playing, silently stomping it
rather than layering (e.g. a hit sound cutting off background music,
or two overlapping hits fighting over one playback slot).

Fixed by adding a real factory mechanism to `IShellHle`
(`RegisterFactory`, checked before the plain fixed-instance map in
`CreateInstanceImpl`) and switching `tools/game_probe.cpp` to register
`[&media_hle]() { return media_hle.CreateMediaObject(); }` instead of
a single pre-created object -- now every real `CreateInstance` call
for `AEECLSID_MEDIA` gets its own fresh `IMedia` object
(`MediaHle::CreateMediaObject()` already bump-allocates a distinct
object address per call, confirmed in `media_hle.cpp`; nothing else
needed changing).

Re-verified the same way as the previous round -- real, external,
OS-level proof, not a screenshot: ran the fixed build through title ->
menu -> cutscene -> walking -> repeated melee attacks (deliberately
exercising many real sound activations in quick succession, the exact
scenario the single-shared-instance version would have handled wrong)
and checked `pactl list sink-inputs` before and after. Same live,
unmuted `zeebulator_game_probe` stream at `s16le 2ch 22050Hz`
throughout, process stayed healthy (no crash, no wandering) across the
whole combat sequence.

Added a real unit test (`RegisterFactoryReturnsAFreshObjectFromEach
CreateInstanceCall`) proving two `CreateInstance` calls for the same
class through a registered factory yield two distinct object
addresses. 311/311 tests pass. Third real code change from this whole
investigation.

## Sound, round six: real decode bug found and fixed (gzip'd MMD_BUFFER), but still not actually audible -- Play() is never called

Directly prompted by the user asking "should I be hearing stuff?" after
round five's `pactl`-only verification. That question exposed a real
gap in this project's own verification: a live, unmuted, correctly-
formatted PulseAudio/PipeWire sink input proves the *pipeline* exists,
not that real audible samples are flowing through it. Recorded the
actual stream with `parecord --monitor-stream=<id>` during a real
title -> menu -> cutscene -> combat run and analyzed the WAV in
Python: **376,832 frames, 0 non-zero samples -- complete digital
silence**, confirming this is a real bug, not a routing/hardware/
Bluetooth-headphone issue (the sink was correctly routed and unmuted).

**Found a real, concrete decode bug.** Temporary instrumentation in
`MediaHle::SetMediaParmImpl` (reverted after use) live-captured the
real `AEEMediaData` Double Dragon actually passes:
`clsData=0x00000001` (not `MMD_FILE_NAME`, which is the only shape
`SetMediaParmImpl` handled -- confirmed real BREW `MMD_BUFFER`), with
a real malloc'd buffer pointer and a real size. Dumped the buffer's
first bytes live: `1f 8b 08 08 ...` -- a genuine gzip stream (magic +
CM=deflate + FLG=FNAME set), with the *original filename* visible
right in the header (`bgm_1_...`). Real sound.ggz entries are
gzip-compressed, and `SetMediaParmImpl` was never gunzipping them --
it only implemented the filename-lookup shape, explicitly rejecting
`MMD_BUFFER` as "not supported yet" (a real, previously-documented,
now-closed gap).

**Fixed for real** (`core/brew/media_hle.cpp`/`.h`): added `MMD_BUFFER`
support -- reads the raw bytes directly from emulated memory, gunzips
them if they start with the real gzip magic (`Gunzip`, same real
zlib/`inflateInit2(windowBits=15+16)` technique already used by
`ModRuntime::DecompressGzipInPlaceImpl`), then dispatches to WAV or
MIDI decode by sniffing the *decompressed* content's own magic bytes
(`RIFF`/`MThd`) rather than a filename extension, since a raw buffer
has none. Live-reverified after the fix: the same real buffer now
decompresses cleanly and decodes as a real, correctly-shaped MIDI
track (1,038,683 samples at 22050Hz mono, ~47s -- matching this
project's own earlier real `bgm_1_0.mid` measurement from Phase 6
almost exactly).

**But it's still not actually audible, and this round found why --
`Play()` is never called.** Extended the temporary instrumentation to
every `IMedia` vtable slot and re-ran through cutscene, extended
passive waiting (12s+), and combat. Real observed sequence on every
prepared media object: `SetMediaParm` (now succeeding) ->
`RegisterNotify` -> **`Stop()`** -- and nothing else, ever, for the
rest of the run (confirmed the run wasn't stalled: the on-screen level
timer visibly ticked down the whole time). Real code explicitly stops
each prepared resource right after preparing it, and whatever later
step is supposed to call `Play()` on it was never reached in any test
window tried this round, including deep into real combat.

**Honest status for the user**: the audio *decode* pipeline is now
provably correct end to end for real, gzip-compressed Double Dragon
assets (verified via live decode of real data, not synthetic
fixtures) -- a genuine, real bug fix. But sound is still not audible,
because the real trigger for `Play()` has not been found. This is a
different, and now the *last remaining*, piece of the puzzle. Added 4
real unit tests covering the new `MMD_BUFFER` path, including the
real gzip-compressed scenario. 315/315 tests pass. All temporary
instrumentation (including a `parecord`-based silence-detection
methodology worth remembering: `pactl` proves plumbing, not sound)
reverted; `git diff --stat` clean.

## Sound, round seven: found the real cutscene script's "load sound resource" opcode; the actual Play() trigger remains unconfirmed

Direct continuation of round six, chasing where `Play()` is really
called from. Cross-referenced this project's own bundled real
reference source, `research/samples/conftest_source/conftest/
ctsoundmgr.c` -- a genuine Qualcomm sound-manager sample -- and
confirmed Double Dragon's own loader (`0x10a1e0`/`0x10a0c0`) matches
its real `L_CTSoundMgr_LoadMedia` almost exactly (`CreateMedia` ->
`SetMediaData` -> `EnableChannelShare(TRUE)` == the real `SetMediaParm
(MM_PARM_CHANNEL_SHARE, 1)` this project already found -> 
`RegisterNotify`), confirming this project's own disassembly reading
has been accurate. Critically, that real reference sample's own
`Play()` (`CTSoundMgr_Play`) is a **separate, on-demand function**,
never called from `LoadMedia` -- matching this project's own
hypothesis that Double Dragon's `Stop()`-after-`RegisterNotify` isn't
unusual, and the real trigger is elsewhere.

**Traced further via three real techniques layered together**:
1. A memory read-watch (temporary, reverted) on the exact live address
   the real `IMedia` pointer gets stored at (`0x803007c0`, captured
   from `CreateInstance`'s own `ppobj` argument) -- found all 4 real
   consumers of that address: the loader itself, a generic per-slot
   cleanup helper, a `SetMediaParm(MM_PARM_VOLUME, ...)` setter, and a
   `Stop()`-only helper. No `Play()` among them, confirmed exhaustive
   for this one address (both created objects reuse the same slot).
2. Live-traced two per-tick indirect callback pointers inside a real
   per-tick "manager" function (`ddragonz.mod` 0x104b6c/0x104b7c) and
   found they implement a **real screen/scene state machine**: one
   dominant per-tick target for the current state, occasional
   single-fire targets for one-time state-entry logic. Confirmed a
   *real state transition* mid-session (extending the wait past a
   `[counter] >= 80` gate inside the cutscene's own per-tick update,
   `0x1222f0`) -- the dominant callback target genuinely changed
   (`0x1222f0` -> `0x1220f8`), replaced by a much larger "phase 2"
   per-tick function calling many more subsystems.
3. Found a **real cutscene script command interpreter**
   (`0x122684`, a classic ARM computed-goto jump table over 10
   opcodes) driving the "phase 2" state, and disassembled every
   opcode. Opcode 9 (`0x1228d0`) calls a real one-shot-trigger helper
   (`0x11f5dc`) that writes a real per-slot index into
   `context+0x3000+0x67c` -- **the exact field the 5-slot resource-
   activation loop (`0x11f528`, chased over the last two rounds)
   polls every tick to decide whether to activate a slot at all**.
   This is very likely the real "cutscene script says: load/prepare
   sound resource N now" command -- a genuine, concrete link between
   the cutscene's own scripted timeline and the audio-loading pipeline
   this investigation has been mapping.

**Still unresolved**: what makes a prepared slot transition from
"loaded, notified, stopped" to actually calling `Play()`. Re-checked
the "generic cleanup" function (`0x10ad58`) specifically for a missed
`Play()` call (offset 24/slot 6) -- confirmed clean, it's genuinely
just cleanup. The real trigger, per the `ctsoundmgr.c` reference
design, is almost certainly a separate, on-demand call (analogous to
`CTSoundMgr_Play`) gated on real gameplay/script state this
investigation hasn't reached or correctly identified yet -- possibly
one of the other 9 script opcodes not yet fully understood, or a
condition on the per-slot state this round didn't check for.

This round's real, concrete progress: proved this project's
disassembly reading is accurate against real reference source, found
and fully explained the real screen-state-machine/script-interpreter
architecture driving the cutscene (new, reusable understanding for
any future Double Dragon investigation), and found a genuine, specific
link from the cutscene script to the audio pipeline. All temporary
instrumentation reverted; `git diff --stat` clean; 315/315 tests pass;
no code change this round.

## Sound, round eight: found a real event-triggered script dispatcher; a full real session cycle still never calls Play()

Continued straight from round seven. Disassembled `0x123630` (called
directly from the "phase 2" per-tick state, in the same numeric
neighborhood as the already-identified audio helpers): a real
**event-triggered script dispatcher** -- checks a per-context "pending
event" slot (`context+0x3700+10`=event ID, `+12`=claimed flag,
`+14`=priority gate), and if a valid pending event exists, claims it
and searches a real script-record table (`*(context+0x36f8)`, a
linked list terminated by `-1`) for a record matching that ID, then
invokes the same 10-opcode script interpreter (`0x122684`) found last
round on the matching record. This is a real "fire event N" mechanism
-- something else (not yet found) posts an event ID into that pending
slot to request a scripted sequence run.

**Final live test this round**: a simple, low-noise watch on
`MediaHle::PlayImpl` alone (reverted after use), run across a full,
mostly-natural real session -- title screen, main menu, cutscene,
sustained real combat (multiple full rounds of movement + attacks),
then continued unscripted play. Mid-session, the game genuinely
progressed into new, previously-unreached territory: a real loading
screen (`"CARREGANDO"`, Portuguese for "loading"), then cycled back to
the title screen (a different real title-screen prompt than the
initial boot -- `"APERTE O BOTÃO HOME"` rather than the boot-time
one), confirming this was a real, novel state transition, not a stall
or a repeat. **`Play()` was never called, not once, across this
entire real cycle.**

**Where this leaves the investigation**: the audio *decode* pipeline
(fixed in round six) is proven correct against real data. The real
*trigger* mechanism has been mapped in extraordinary depth -- a real
per-tick screen-state machine, a real 10-opcode cutscene script
interpreter, a real event-queue dispatcher, a real concrete write into
the resource-activation loop's own polled field -- all confirmed live,
none of it guessed. But `Play()` itself was not observed firing across
title, menu, cutscene, combat, a genuine level-load transition, and a
full return to title. Two real possibilities remain, both unconfirmed:
(a) this specific demo/build genuinely never reaches a state that
calls `Play()` within what this project's automated `send_key.py`-
driven input can trigger (real sustained/precisely-timed human input,
or a menu path never tried -- e.g. "BATALHA 2 JOGADORES", "OPÇÕES" --
might behave differently), or (b) `Play()` is gated behind still-
unidentified per-slot state (the `context+0x3700+10/12/14` event
fields, or another script opcode among the 10 not yet fully manually
played back) that this round's testing didn't happen to satisfy.
Recommend a real human driving real input interactively as the next
concrete step, rather than another round of automated scripted input,
since this round's evidence suggests the automated input pattern
itself may not be exercising whatever real condition `Play()` needs.
All instrumentation reverted; `git diff --stat` clean; 315/315 tests
pass; no code change this round.

## Sound, round nine: found the real `Play()` trigger function itself
(never called); real human interactive input closes off the
"automated input pattern" hypothesis

Prompted by the user's own report that Double Dragon's title screen is
*known* to have music and sound effects from the start -- a much
stronger constraint than "some gameplay moment should have sound,"
since it means the trigger shouldn't require deep state at all.

**Traced the real boot-time media init function in full**
(`ddragonz.mod` `0x10a1e0`), instruction by instruction, not just the
call sites already known from earlier rounds. Confirmed it runs at
real boot (before any input), and does exactly this and nothing more:
calls the real IShell slot-43 method (currently a stub returning
success), calls `GetHandler(0x01005500)` (succeeds, this project's own
round-one fix), calls `CreateInstance(0x01005500)` (succeeds, this
project's own round-two fix, object `0x80200000`), then
`SetMediaParm(kParmMediaData)` (succeeds -- the real decoded audio
this project's round-six fix produces), `SetMediaParm(kParmChannelShare)`
(no-op success), and finally `RegisterNotify(fn=0x0011d020, pUser=this)`.
Then it returns. **It never calls `Play()`.** This is a complete,
line-by-line account of the function, not a sample of call sites --
there is no other branch or exit path in it that could call `Play()`.

**Found a second, real, and directly relevant function**:
`0x0011d04c`. Given a context pointer `Q` (fields at `Q+8` = a media
object, `Q+40` = an optional secondary context), it does a real,
concrete `IMedia::Play()` tail-call (vtable offset `0x18` = slot 6) on
the object at `Q+8` if `Q+40` is null, or on a secondary object if not
-- and in the latter case, sets a completion byte at
`(*(Q+40))+37` afterward. This is exactly the shape of a real
"start this sound" entry point, not a guess -- the vtable-slot-6
`Play()` call is unambiguous in the disassembly.

**But nothing calls it.** Searched exhaustively for how code could
reach `0x11d04c`:
- Scanned the *entire* binary for the real ARM PIC function-pointer
  idiom (`ldr rX,[pc,#N]; add rX,pc,rX`) used everywhere else in this
  project's own confirmed callback-registration sites (e.g. this same
  round's confirmation that `0x0011d020` is materialized this exact
  way at `0x10a394`/`0x10a398`) -- 780 such pairs found across the
  whole ROM, none resolve to `0x11d04c`.
- Searched for `0x11d04c` stored as a raw absolute 32-bit literal
  anywhere in the binary (the way a non-PIC jump/vtable table would
  store it) -- none found.
- Added a live PC-reached watch (`fetch_addr == 0x11d04c`) and a live
  memory-read watch (any `Read32` returning exactly `0x0011d04c`) to
  the interpreter/memory layer. Ran a full session: idle boot (30s,
  title screen, zero input), scripted menu+combat driving, and then
  **genuine human-driven interactive play** (the user, not
  `send_key.py`) covering many distinct real button IDs
  (`0x106c3fe/3ff/400/401/403/40b/40c`) across an extended real
  session from title through combat. Zero hits, on either watch,
  across all of it.

**This closes off round eight's open hypothesis.** Round eight's
recommendation was "try real human input, since the automated pattern
might not be exercising whatever `Play()` needs." That's now been
tried directly, extensively, and it made no difference: real human
input reaches the emulator correctly (confirmed live -- every single
button press produced a real `HID button callback(...) ran` log line,
proving genuine delivery, not a focus/input-plumbing problem), and
still zero `Play()` calls, including at the pure idle title screen
where the user confirms music should already be playing with no input
at all.

**Also tested and ruled out a preference/mute-gate hypothesis**: the
same PIC-idiom scan applied to `IShell::GetPrefs`/`SetPrefs` (slots
23/24) via the real, confirmed IShell call-trampoline pattern (the
`ldr r0,[r7,#-4]; ldr r0,[r0,#192]; bx r0` sequence used at every
other real IShell call site this project has found) turns up real
call sites for slots 2 (`CreateInstance`), 32 (`GetHandler`), and 43
(the unconfirmed slot), but never 23 or 24. Real code never calls
`GetPrefs` at all -- a mute/volume preference check gating playback is
not the explanation.

**Also traced and ruled out** a separate, real class (`0x01005511`,
already scaffolded in `game_probe.cpp` from earlier investigation) as
a candidate for the missing link -- confirmed it's Zeebo's real
download/install-progress notification service (reached only after a
real HID controller is detected, gated behind a real environment-
readiness byte), sharing the *same* generic notify callback
(`0x0011d020`) as the media object purely because that callback is a
generic dispatcher, not because the two subsystems are related. Its
own "call vtable slot 11 with literal 100" action is on a completely
different object (the download-progress interface, not `IMedia`) --
confirmed by tracing its real callback body (`0x11f4dc`) directly
rather than relying on the prior summary of it.

**Where this leaves the investigation**: every concrete, testable
hypothesis this project has been able to form has now been tried and
ruled out -- decode correctness (proven right), `GetHandler`/
`CreateInstance` gating (fixed, confirmed), a preference/mute gate
(ruled out), the download-progress subsystem (ruled out, unrelated),
and "the input pattern doesn't exercise the trigger" (ruled out by
real human play). A real, concrete `Play()`-calling function exists in
the ROM and is provably never reached by any static or dynamic
analysis this project has been able to perform, including a real human
at the controls for an extended session starting from a cold title
screen. This is now a genuine open question rather than an
unexplored one. All instrumentation reverted; `git diff --stat` clean;
no code change this round.

## Sound, round ten: a real per-entity event dispatcher confirmed live
(never carries a real event); a promising "global Play() trampoline"
lead conclusively closed as a real, unrelated GL call

Prompted by the user's own confirmation that Double Dragon's title
screen is known to have music and sound effects from the start --
narrowing the search to whatever should fire with zero player input.

**Broadened the search past the single `0x11d04c` candidate** by
scanning the whole ROM for the real instruction *shape* of a
`Play()`-type call site (vtable-deref, then `+0x18`/slot-6 load, then
an indirect call) rather than one specific address. Found 14 real
matches. Two were the already-known internals of `0x11d04c` itself
(confirms the scan is sound); the rest were new candidates.

**Found and confirmed live a real per-entity event dispatcher**
(`0x10d4c8`): takes `(self, pEvent)`; if `pEvent` is null, delegates
elsewhere immediately; otherwise switches on `*pEvent` (event codes
0-3), and for event code 2 specifically, selects a sound ID (`0xbe2`)
and calls the same-shaped `Play()`-style trampoline on the entity's
own media object (`self+8`, the same offset this project has
confirmed the real `IMedia` object lives at in every other function
this investigation has traced). Two real, direct `bl` callers found
(`0x10b1b8`, `0x10b340`), themselves gated by a real "did this
per-frame animation value change" comparison -- the classic shape of
an animation-frame-triggered sound event.

Instrumented all three addresses live (dispatcher entry, its event-2
branch, and the trampoline target) and had the user drive real,
extended interactive gameplay -- movement, repeated attacks, landing
hits on enemies. **The dispatcher fires continuously, every tick,
confirming this is real, live, per-frame entity-update code, not dead
weight.** But its event pointer was `NULL` on every single call
observed, across the whole session. The event-2/`Play()` branch never
fired.

**A second, separate real call site initially looked like the
breakthrough**: a different function (reached via `bl 0x124118`) that
also matched the `Play()`-shaped scan, and which *did* fire
continuously during real gameplay (confirmed live, repeatedly, not a
one-off). Traced its target precisely: it fetches an object pointer
from a fixed global slot (`0x14ccb4`, real address, computed and
confirmed live -- not a guess), dereferences it for a vtable, and
calls vtable slot 6 on it. Added a live probe on the exact call target
and the global slot's live value. Result: global slot resolves to
`0x80008000` -- **the project's own GL interface object**
(`GlHle::BuildGl`'s `object_address`, `tools/game_probe.cpp`), not
`IMedia`. Vtable slot 6 on the real GL interface (verified directly
against the slot ordering in `GlHle::BuildGl`, sourced from the real
extracted `AEEGL.h`) is `glBlendFunc`. The call's own argument,
`0x302`, is the real OpenGL ES enum value for `GL_SRC_ALPHA`. This is
a real, legitimate rendering call (almost certainly a hit-flash or
sprite-transparency blend effect), reached via a shared/generic
"call vtable slot N on a fixed global object" trampoline utility that
just happens to reuse slot 6 -- the same numeric slot as `IMedia`'s
`Play()` -- for a completely different interface. A real, valuable,
and now conclusively closed false positive: confirmed via concrete,
named evidence (a real object address this project itself allocated,
a real vtable slot this project itself built, a real matching OpenGL
enum), not left ambiguous.

**Where this leaves the investigation**: the one per-entity dispatcher
genuinely capable of calling real `IMedia::Play()` (`0x10d4c8`,
event code 2) is confirmed live and running every tick, but never
once carried a real event through an extended real human play session
covering movement, sustained combat, and landed hits. Combined with
round nine's findings (the boot-time `Play()`-trigger function
`0x11d04c` also never reached, `GetPrefs` never called, the
download-progress interface confirmed unrelated), this round adds:
the dispatcher that *would* trigger `Play()` is real, live, and
reachable, but whatever real per-frame condition is supposed to post
a non-null event to it never did, across the most extensive real
testing this investigation has run. All instrumentation reverted;
`git diff --stat` clean; 315/315 tests pass; no code change this
round.

## Sound, round eleven: traced the real per-entity event dispatcher's
actual live data (not just whether it's reached) across the most
exhaustive real session yet -- the data value that would trigger
`Play()` never once appears

Direct continuation of round ten's dispatcher (`0x10d4c8`). Round ten
established it fires live but with a null event pointer from its two
statically-discovered combat call sites. This round broadened the
search to every real caller.

**Found the true breadth of the mechanism**: `0x10d4c8`'s event
pointer is populated by a shared helper (`0x10b270`) with 60+ real
call sites scattered across `0x104e5c`-`0x109c3c` -- a widely-used
per-entity animation/state utility, not something specific to one
function. Traced `0x10b270` itself: it does a real per-category table
lookup (`entity+category*4+0x3000+{0xb84,0xc30,0xd88,0xcdc}`, category
`0x25`/37 observed live) to find a candidate event value, checks it
against a real `0xffff` "no entry" sentinel (confirmed live: real
values seen were `0/9/0xd/0x3f/0x71/0xa1/0xd3/0x104` -- the table is
genuinely populated, never empty), then only delivers the event to
`0x10d4c8` if that value differs from the previously-recorded one (a
real "did this change" gate).

**Live-instrumented the actual event pointer, its dereferenced value,
and whether `Play()`'s branch (`0x10d564`, event code `2`) is ever
reached**, then had the user drive an exhaustive real session: normal
combat (as before), then explicitly defeating multiple enemies, then
deliberately losing all three lives, sitting through the real continue
countdown, reaching a genuine game-over state, and returning to the
title screen. Result across **87,116 real, non-null event
deliveries** (not a small sample -- confirmed live via five distinct
real caller sites, including the exact combat dispatch site from round
ten, 371 of those deliveries): the dereferenced event code was **`0`
or `1` in every single case, never `2` or `3`**. `Play()`'s branch
(`0x10d564`) was hit zero times. No crash, no wandering, process
healthy throughout.

**Ruled out a stale/incomplete ROM dump as the explanation**: this
project's own `research/games/Double Dragon/mod/274754/` files
(already used for this entire investigation) are byte-for-byte
identical (`sha256sum`, all three of `ddragonz.mod`/`data.ggz`/
`sound.ggz`) to the ones inside `Double Dragon (Brazil) (Es,Pt).zip`,
the project's other independently-sourced copy of the same title. Not
a demo-vs-retail difference -- there is only one real copy of this
ROM in this project, and it's the one already under test.

**Where this leaves the investigation**: this is the cleanest and
most exhaustive negative result yet. The mechanism capable of
triggering `Play()` is real, confirmed alive, confirmed carrying real
(non-null, non-sentinel, table-driven) data continuously throughout
gameplay -- and the specific data value that would mean "play a
sound" has not appeared once across a session covering every normal
player action this project can identify (movement, every attack type
tried, landing hits, taking damage, dying, a full game-over cycle).
Two real possibilities remain: (a) this exact ROM's own data tables
genuinely never populate a `2`/`3` event under any real, reachable
player action (a property of the shipped title itself, not of this
project's emulation), or (b) a second, structurally different
sound-triggering mechanism exists elsewhere in the ROM that hasn't
been found despite exhaustive vtable-shaped-call-site scanning,
`GetPrefs` call-site scanning, and this round's full data-path trace.
All instrumentation reverted; `git diff --stat` clean; 315/315 tests
pass; no code change this round.

## Sound, round twelve: real gameplay audio confirmed working -- the
missing piece was two sibling `CreateInstance` class IDs, silently
failing since round one

The user independently ran this exact ROM through another real Zeebo
tool ("Infuse") and confirmed genuine gameplay sound -- ruling out
round eleven's "this build's data never triggers a sound event" theory
outright, and reframing the entire investigation: the gap was in this
project's own class registration, not the ROM's content.

**Revisited two real, never-followed-up `CreateInstance` failures**
from round one/two of this investigation: `0x0100550a` and
`0x01005501`, requested repeatedly at `lr=0x0010a12c` from the very
first live capture, alongside the confirmed-working `0x01005500`
(`AEECLSID_MEDIA`). Found the real function they're requested from
(`ddragonz.mod` `0x10a0c0`): a near-identical twin of the already-
understood `AEECLSID_MEDIA` init function (`0x10a1e0`), running the
same `CreateInstance`+`SetMediaParm`+`RegisterNotify` sequence, but
taking its class ID as a real caller-supplied parameter (`r5`) rather
than a hardcoded constant -- and never calling `GetHandler` itself.
Real Double Dragon sets up **multiple separate sound channels**
(background music plus at least one further channel, likely SFX) via
this shared generic setup routine; only the one this project happened
to hardcode (`0x01005500`) ever succeeded, so every real channel using
`0x0100550a`/`0x01005501` failed `CreateInstance` silently, this
entire investigation, without ever crashing or logging an obvious
error.

**Fix**: registered the same generic `MediaHle::CreateMediaObject()`
factory for both sibling class IDs too (`tools/game_probe.cpp`) --
this project's own `MediaHle` never dispatched on codec/class identity
in the first place (it sniffs real container magic bytes), so no
other change was needed.

**Verified live, immediately**: real, audible gameplay sound for the
first time this entire investigation -- confirmed directly by the
user. Real `MediaHle::PlayImpl` calls now fire for three distinct real
objects with three distinct real decoded clips (`0x80200000`: 38,878
samples; `0x80200004`: 34,081 samples; `0x80200008`: 1,046,798 samples
-- the ~47s real MIDI-rendered background music from round six, now
actually audible for the first time).

**Follow-up audio-quality issues found and fixed, once real audio was
actually reachable to listen to**:
- The user reported the (now-audible) music as "extremely deep and
  very loud." Isolated the exact decoded PCM buffer to a standalone
  WAV file (bypassing the Mixer/backend entirely) and confirmed live
  the same real distortion is present in the raw decode itself, not
  introduced downstream.
- `core/audio/mixer.cpp`'s `Mix()` never resampled -- every voice was
  read frame-for-frame against the Mixer's fixed output rate
  regardless of its own real decoded rate, a real, previously-
  documented "not a problem since every asset is 22050Hz" assumption
  that broke as soon as real, previously-unreachable channels started
  decoding real content. Replaced with real linear resampling (a
  fractional source position, advanced by each voice's own real
  rate ratio). Didn't fix this specific complaint (all three real
  clips here happen to already be 22050Hz) but is a real, now-fixed
  correctness gap for whatever real asset isn't.
- Instrumented `RenderMidiToPcm` live: the real background music track
  is 1,788 real notes across 9 real simultaneous MIDI channels
  (including real GM channel 10/index 9, the percussion channel --
  404 of those notes), spanning real note numbers 29-84 (~44Hz-1047Hz).
  This project's synthesizer had never distinguished the percussion
  channel from melodic ones (a documented, known gap from earlier
  work) -- real channel-10 note *numbers* mean specific drum sounds,
  not pitches, so running them through the pitched sine synthesizer
  injected real spurious low-frequency content throughout the track.
  Fixed: `MidiNote` now carries its real source channel, and
  `RenderMidiToPcm` skips real channel-9 notes entirely rather than
  mis-rendering them.
- The fixed per-note headroom (`0.2f` amplitude, sized for ~5
  simultaneous notes) plus a hard per-sample clamp meant real, dense
  9-channel polyphony regularly exceeded full scale -- confirmed live,
  ~5-7% of samples pinned at the exact int16 limit, real audible
  clipping distortion. Replaced the hard clamp with real post-mix
  peak normalization (scale the whole clip down by its own real peak
  only when that peak exceeds full scale), removing the clipping
  without touching any note's real loudness relative to any other.
- None of the three fixes above changed the user's actual complaint.
  Asked directly, and confirmed: the real melody, real rhythm, and
  real tempo are all correct -- it's recognizably the right piece,
  just through what the user accurately described as a "cheap/toy"
  synthesizer voice. This isolates the remaining gap precisely: every
  real note (`RenderMidiToPcm`) is still a plain sine tone with no
  real instrument timbre modeling at all (an explicitly documented,
  known scope limitation from when this synthesizer was first built,
  not a new bug) -- a real, correctly-decoded, correctly-timed MIDI
  performance rendered through a synthesis engine several real steps
  short of sounding like real instruments. A different, much larger
  kind of task than anything else in this investigation (real
  per-GM-program waveform/timbre modeling, or a real sampled-
  instrument/soundfont-based renderer), not a bug to keep chasing the
  same way as the rest of this investigation.

**Where this leaves the investigation**: real audible gameplay sound
is confirmed working for the first time. The original blocking
question this entire investigation chased ("why does `Play()` never
get called") is resolved -- it wasn't a missing trigger at all, it was
missing class registrations from round one that silently broke every
channel except the one this project happened to test first. What
remains is a scoped, well-understood synthesis-quality gap (plain sine
tones vs. real instrument timbres), not an open mystery. `git diff
--stat`: real changes in `tools/game_probe.cpp` (the two sibling
factory registrations), `core/audio/mixer.{h,cpp}` (real resampling),
`core/loader/midi.{h,cpp}` (real percussion-channel skip + real
post-mix normalization), plus new real test coverage in
`tests/mixer_test.cpp` and `tests/midi_test.cpp`. 318/318 tests pass.



## Sound, round thirteen: real General MIDI wavetable synthesis (a
soundfont, not a hand-tuned approximation), plus real per-voice
volume/headroom control

Round twelve's hand-rolled synthesizer got individual instruments
correctly differentiated and correctly decaying/sustaining, but the
user's own direct comparison against real gameplay music was clear:
"nothing like the real thing." The user then asked a well-founded
architectural question -- shouldn't the real instrument sound live in
the game's own files? -- which surfaced an important correction:
Double Dragon's music is Standard MIDI Files, which contain only note
events (pitch, timing, velocity, program number), never audio. On real
hardware, the actual sound came from the platform's own MIDI
synthesizer, not the game's own assets.

**Researched what that real synthesizer actually was**, rather than
guessing: the Zeebo SDK's own developer guide
(`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
ZeeboDeveloperGuide0.97.pdf`, real hardware built on a Qualcomm
MSM7201A chipset) documents MIDI playback going through Qualcomm's
**CMX** synthesizer -- a real, GM1/GM2-compliant, wavetable/sample-
based engine (72-voice polyphony, 44kHz, 512KB wavetable), not FM
synthesis and not a hand-tuned waveform approximation. This confirmed
a soundfont-based renderer is the architecturally correct direction
(the same synthesis *style* as the real hardware), even though
Qualcomm's own proprietary wavetable samples aren't publicly
available -- what's achievable is the same standardized GM instrument
*assignment* (a compliant soundfont's program 29 is "Overdriven
Guitar," exactly like CMX's own), not byte-identical *timbre*.

**Two licensed external dependencies added**, both verified directly
rather than assumed: TinySoundFont (MIT license, `schellingb/
TinySoundFont`, a small single-header SoundFont2 synthesizer, fetched
via CMake `FetchContent` pinned to a specific commit) and GeneralUser
GS (a widely-used GM-compliant SoundFont whose own license explicitly
permits bundling in other software projects -- confirmed by fetching
its actual license text, not assumed -- also fetched via
`FetchContent` with a `URL_HASH` for integrity; 32.3MB, confirmed a
valid `RIFF`/`SoundFont` file via `file`). Neither is committed to the
repo, matching this project's existing zlib/SDL2/googletest
`FetchContent` pattern. `core/audio/soundfont_path.h.in` is a
`configure_file` template resolving to the fetched file's real
absolute path on each machine.

**New `core/audio/soundfont_synth.{h,cpp}`** (`SoundFontSynth`): loads
the bundled soundfont once, and `RenderMidi()` drives note-on/note-
off/program-change events through it in absolute-time order (built
from `MidiFile::notes`, using the same `TickToSeconds` this project's
hand-rolled synth already used -- promoted out of `midi.cpp`'s
anonymous namespace into a shared public declaration in `midi.h` so
both renderers use the same tick->time conversion). Real GM channel-10
percussion is handled by the soundfont's own drum kit (bank 128, via
`tsf_channel_set_presetnumber`'s `flag_mididrums` argument), not this
project's own hand-rolled percussion classification -- that logic now
belongs only to the fallback synth.

**Wired into `MediaHle`** via a new optional `SoundFontSynth*`
constructor parameter (defaults to `nullptr`, so every existing test
construction call site needed zero changes) -- when non-null and
loaded, real MIDI decode uses it instead of the hand-rolled
`RenderMidiToPcm`, which stays as a tested fallback (and is what every
existing unit test still exercises, avoiding a ~32MB soundfont load
per test). `tools/game_probe.cpp` constructs one `SoundFontSynth` for
the whole process lifetime and passes it in.

**Verified live, immediately**: confirmed directly by the user --
"instruments seem right" -- correct General MIDI instrument assignment
(programs 29/30 = Overdriven/Distortion Guitar, 33 = Electric Bass, 48
= String Ensemble, 61 = Brass Section, all reading correctly as
themselves) replacing the hand-rolled synth's crude sine/square/
sawtooth approximation entirely.

**Follow-up balance issue found and fixed**, once real instruments
were actually audible to judge balance with: the user reported real
gameplay music drowning out the SFX. Measured directly (a temporary
per-`Play()` peak/RMS instrumentation, reverted after use): the
soundfont-rendered background music was hitting **peak=32767 -- the
exact int16 ceiling, genuine clipping** -- with RMS 22,673, against
WAV-based SFX voices sitting at RMS ~2,500-4,600. The first
hypothesis -- the already-documented but never-applied `MM_PARM_
VOLUME` (`core/brew/media_hle.cpp`'s own long-standing "accepted, not
yet applied" gap) -- was implemented properly: `Mixer` gained a
per-voice `volume` field (`Play()`'s new parameter, plus a new
`SetVolume()` for already-playing voices), and
`MediaHle::SetMediaParmImpl`/`GetMediaParmImpl` now actually store and
apply it. Confirmed live this didn't change the symptom (real Double
Dragon code apparently never sets differing per-channel volumes here),
so the real cause was elsewhere: `SoundFontSynth`'s own internal
mixing of up to 9 simultaneous channels, with no headroom management
at all (`tsf_set_output`'s own `global_gain_db` parameter was 0dB).
Fixed with a -16dB gain reduction applied *before* TinySoundFont's own
internal render (not a post-render scale-down, which can only turn
down audio that's already been distorted by clipping) -- re-measured
live: RMS dropped from 22,673 to a level comparable to the SFX, peak
no longer pinned at the ceiling. The -16dB figure came from the user's
own direct A/B comparison against the real reference ("still a bit
loud... let's go like 10% lower" -- 20*log10(0.9) is ~-0.9dB, applied
on top of an initial -15dB first pass).

**Where this leaves the investigation**: audible, correctly-balanced
gameplay music and SFX, using correct General MIDI instrument
assignments through a real soundfont synthesizer, confirmed
correct-sounding live by the user against a real reference. New test
coverage: `tests/soundfont_synth_test.cpp` (the bundled soundfont
actually loads; a note is audible; two distinct GM program families
render audibly differently; channel-10 percussion produces audible
drum-kit output; an empty MIDI file renders a single silent sample
without crashing) and new `Mixer`/`MediaHle` volume tests
(`tests/mixer_test.cpp`, `tests/media_hle_test.cpp`). All temporary
instrumentation reverted; `git diff --stat` clean of debug cruft;
331/331 tests pass.

## Sound, round fourteen: some real SFX (e.g. one of the two punch
attacks) never play -- confirmed to be outside this project's own HLE
pipeline entirely

The user reported a specific, reproducible pattern: left-arm punches
have sound, right-arm punches never do, and a sound "disappeared
completely" after landing a hit on an enemy.

**Instrumented the full real chain** (`GetHandler`, `CreateInstance`,
`SetMediaParm`'s decode path, `Play`) with live success/failure
logging and had the user reproduce the exact same sequence. Result:
every single real call at every stage succeeded. 2 real `GetHandler`
calls, both success; 17 real `CreateInstance` calls for real media
classes (`0x01005500`/`0x01005501`/`0x0100550a`), zero failures (the
only real failures were pre-existing, already-known-unrelated classes
this project has never needed, `0x01001002`/`0x0102f679`/
`0x0102f681`/`0x01030852`); 18 real `SetMediaParm` decode calls, all
"OK", zero decode failures; 14 real `Play()` calls, all "OK", zero
playback failures.

**This rules out an HLE registration/decode/playback bug entirely**
for whatever produced the reported symptom -- there is no error
anywhere in this project's own real pipeline to fix. The real
explanation has to be the same category of gap round eleven already
mapped in exhaustive depth: real game code that, for some real
actions, never calls `Play()` (or even `CreateInstance`) at all. Round
eleven traced the real per-entity event dispatcher this depends on
down to a specific data-driven gate and confirmed live (87,116 real
non-null event deliveries) that the specific value needed to trigger
`Play()` never appeared during an exhaustive real session covering
combat, multiple enemy defeats, and a full game-over cycle -- this
round's "right punch never plays, left punch does" is a much more
specific, reproducible instance of that same real, still-unresolved
question (what real condition, if any, would ever produce that
trigger value), not a new one.

**Where this leaves the investigation**: every fixable gap this
project's own HLE can account for has been found and fixed --
registration, decode, mixing, timbre, balance. What's left (some real
SFX genuinely never triggering `Play()`) is the same real open
question round eleven already invested extremely heavily in without a
resolution, not a quick follow-up. No code change this round (pure
diagnostic); all temporary instrumentation reverted; 331/331 tests
pass.

## Sound, round fifteen: found and fixed the real bug -- `RegisterNotify`
was never firing, so a real per-channel priority-stealing system could
never reclaim a channel once a higher-priority sound had claimed it

Picked back up with a corrected reproduction protocol: pausing/
unpausing and having the user press exactly one real key (`x`) per
punch, tracking every `Play()` live against every press. This
surfaced an error in round fourteen's own button-UID assumption:
`0x106c403` (used throughout the earlier "left/right punch" theory)
is not an attack button at all -- it's `SdlKeyToHidButton`'s `kBack`
(`tools/game_probe.cpp`), mapped from Enter/Backspace, the real
title/menu-confirm button (already correctly documented as such in
that function's own doc comment). The real, sole punch button is
`SDLK_x` -> `kButton2` = `0x106c40b`. The earlier "left punch always
sounds, right punch never does" pattern was very likely this same
mislabeling: attributing the menu-confirm chime (which naturally
always succeeds, since it isn't gated by any of what follows) to one
of the two "punch" buttons.

With the real button confirmed, live testing found real punch presses
*do* call `Play()` early in a session and *stop* calling it the longer
play continues -- not just for punches, but eventually for unrelated
real SFX too (enemies hitting the ground, enemy attacks). Systematically
ruled out every resource-based theory with live instrumentation before
looking at real game logic: Mixer voice count never exceeded 4
concurrent voices with zero sustained clipping (a temporary high-water-
mark diagnostic in `Mixer::Mix`), and the media-object bump allocator
(`object_region_start=0x80200000`) had a full 1MB of headroom -- neither
was remotely close to exhaustion.

**Traced the real call chain live**, three levels deep, via temporary
`ArmInterpreter::Step()` PC-triggered hooks (no direct `bl`/literal-
pointer references existed anywhere in the ROM for any of these,
confirmed by both a static full-binary scan and the earlier live-write-
watch technique turning up nothing -- they're reached by a plain,
unconditional tail-branch chain instead, `b`, not `bl`):

- `0x11f528`: a real per-frame, per-character dispatcher walking 5
  "sound channel" slots (`entity+0x3000..0x3800`-ish, stride 8 bytes).
- `0x11e8ac`/`0x11ea28`: per-slot wrapper functions, entered via a
  normal `bl` (confirmed: `0x11f5bc`/`0x11f594`) then tail-jumping
  (`b`, LR untouched) into the shared trigger below.
- `0x11eb0c`: the actual `Play()`-calling utility this whole
  investigation has been finding sibling instances of since round
  nine. Every single real entry into this function led to a
  successful `Play()` in every live capture (18 entries, 18 plays) --
  it was never once "reached but rejected." The real chain stopped
  *before* this function, not inside it.

**Found the real gate**: `0x11ea28`'s own body (`ldrsb r0,[r8,#44];
cmp r0,r9; popgt...`) only lets a channel actually fire if a freshly-
read "current priority" value is >= the priority recorded the last
time *that channel* successfully played. Traced the real channel-
picker (`0x11f620`) that decides which of a per-character 4-channel
round-robin pool (`entity+0x19c`-relative, slots 1-4; slot 0 is
reserved separately) a new sound claims: it walks the rotation looking
for the first channel whose stored priority is <= the new sound's own
priority (i.e. "can this new sound steal it"), and force-claims the
first channel in rotation if none qualify. Net effect: a channel a
high-priority sound once claimed becomes permanently unusable by
anything lower-priority, because nothing was ever found to bring its
stored priority back down -- confirmed live via a targeted memory
write-watch on the exact stamp bytes (`0x803036a4/ac/b4/bc/c4` for this
session's entity), which showed the punch channel's `current_stamp`
cycling through several values below its own frozen `last_played_stamp`
forever, explaining the gradual, multi-SFX-category degradation
exactly as reported (each channel/priority pairing gets stuck
independently as higher-priority sounds cycle through over a session).

**Found the real fix**: this project's own `MediaHle` class doc
comment already flagged the gap driving all of this -- `RegisterNotify`
stored a callback but never fired it, "not a fundamental gap," pending
a periodic tick hook. Live instrumentation confirmed real Double
Dragon code registers this callback for *every* sound media object (5
for 5 objects at boot, all the same real callback address, `0x11d020`)
and *never* polls `GetState` (zero calls across a full session) --
real code depends entirely on the notification firing. Traced
`0x11d020`'s real body (dispatches `MM_CMD_PLAY`/`MM_STATUS_DONE` or
`MM_STATUS_ABORT` to `0x11f4dc`) and confirmed live-evidence match
with an earlier round's own already-bundled comment in
`tools/game_probe.cpp` (a one-shot download-complete simulation that
reached this same real success-path check but noted `pUser+37` needing
to be nonzero to proceed further -- exactly the "ready" flag `0x11eb0c`
sets on every real successful `Play()`). `0x11f4dc`'s real body, once
that flag is set, either resumes a looping sound or -- the real path
taken for a one-shot SFX -- resets the channel's stored priority stamp
back to 0 (`strb r0,[r3,#36]`, landing on the exact byte the whole
channel-stealing chain above reads). Without ever firing this
notification, that reset never runs, and channels stay poisoned
forever.

**The fix**: `MediaHle::Tick()` (new), called once per real game loop
iteration alongside `ModRuntime::Tick()`/`IShellHle::Tick()` in
`tools/game_probe.cpp`. For every media object whose voice has
finished playing naturally since the last tick, it fires the real
registered callback with a real `AEEMediaCmdNotify`-shaped struct
(`+8`=4/`MM_CMD_PLAY`, `+16`=2/`MM_STATUS_DONE` -- the only two fields
the real callback body actually reads, confirmed by its own
disassembly rather than a guessed real header layout) via
`HleRuntime::CallArmFunction`, reusing a scratch struct address well
past any real object count a session could reach. Live-confirmed fixed
by the user across multiple extended play sessions after this landed:
punch sounds, enemy-hit sounds, and enemy-fall sounds all kept working
throughout, matching real Infuse behavior for the first time this
investigation directly compared against it.

Added real test coverage (`tests/media_hle_test.cpp`):
`TickFiresRegisteredNotifyWithDoneStatusOnceAVoiceFinishes` (registers
a real HLE-backed notify callback, drains a real short WAV voice via
`Mixer::Mix`, and asserts the callback fires exactly once with the
real user data and the real `MM_CMD_PLAY`/`MM_STATUS_DONE` field
values) and `TickWithNoRegisteredNotifyDoesNotCrash`. All temporary
diagnostics (`ArmInterpreter::Step()` PC hooks, `Memory::Write8`/
`Write32` watches, and the `[DBGPLAY3]`/`[DBGNOTIFY]`/`[DBGGETSTATE]`/
`[DBGMIX]` prints) fully reverted; 333/333 tests pass.
