# Zeebulator — Dev/QA Tooling Task Breakdown

Companion to `TASKS.md` (emulation-correctness work) — this file tracks
player-facing QA/dev-experience tooling for `tools/game_probe.cpp` (or its
eventual promoted successor): resolution scaling, save states, and
controller binding, plus hotkeys and an on-screen overlay to control them
live. Exists so
bug-hunting sessions (see `PHASE8_LOG.md`) get faster to reproduce and
easier to play through end-to-end.

## Context: what's already there

- `tools/game_probe.cpp` is the de facto "play a real game" entry point
  today, even though it lives under `tools/` — window is a fixed 640x480,
  non-resizable (`SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL`, no
  `SDL_WINDOW_RESIZABLE`).
- `frontends/standalone/sdl2_unified_backend.{h,cpp}` already implements
  real `SDL_GameController` polling (`PollController()`) into `ZPadState`
  (`core/backend.h`) — but `game_probe.cpp`'s actual play loop never calls
  `Backend::PollInput()` at all. All real player input today goes through a
  separate, keyboard-only path (`SdlKeyToHidButton`/`SdlKeyToAvk` in
  `game_probe.cpp`) that injects synthesized real HID button events/AVK key
  events directly into the guest's own registered callbacks.
- `Sdl2UnifiedBackend::PresentFrame`'s GL viewport is set from the
  backend's own stored `width_`/`height_` (fixed at construction), not the
  SDL window's actual live drawable size — the video quad itself is
  already drawn in normalized 0..1 texture/vertex coordinates, so once the
  viewport tracks the real window size, scaling falls out for free.
- No save/restore-state mechanism exists anywhere in the codebase today.
  Emulator state is spread across: `ArmInterpreter` (registers/flags/mode),
  `Memory` (sparse page-based guest address space), and several
  independent host-side C++ classes each owning their own state
  (`MediaHle`, `IShellHle`, `FileHle`, `ModRuntime`, `Mixer`, HID device
  state, ...) — none of them expose a serialize/deserialize hook yet.

## Phase A — Resolution scaling (2x/3x/4x)

Exit criterion: the player can pick 1x/2x/3x/4x window scale and see the
same 640x480 emulated output stretched cleanly, without affecting anything
the guest app itself observes (still reports 640x480 via `AEEDeviceInfo`/
`IDisplayHle`).

- [x] Decouple "logical/emulated resolution" (640x480, unchanged — what
      `IDisplayHle`/`AEEDeviceInfo` report to the guest) from "window/
      presentation size" (what the real SDL window actually is) — done via
      a real offscreen FBO (`Sdl2UnifiedBackend::fbo_`/`fbo_texture_`,
      always rendered at the fixed logical size, with a real depth
      renderbuffer attached too — the real app's own `GL_DEPTH_TEST`
      usage needs one, matching the same `SDL_GL_DEPTH_SIZE` request
      already made for the window's own context) rather than the
      originally-planned "just retarget the viewport" approach, since
      real app-driven GLES draws (not just the 2D quad) also needed to
      scale correctly, not only the `PushVideoFrame` path
- [x] Make the game window resizable (`SDL_WINDOW_RESIZABLE`), plus
      `Sdl2UnifiedBackend::SetWindowScale` doing an explicit
      `SDL_SetWindowSize` to `640*scale x 480*scale` for the hotkey case
- [x] `Sdl2UnifiedBackend::PresentFrame`: blits the offscreen FBO onto
      the real window using its live drawable size
      (`SDL_GL_GetDrawableSize`) every frame, letterboxed (see next item)
      — supersedes the original plan of retargeting `PushVideoFrame`'s
      own viewport directly
- [x] Preserve aspect ratio (4:3) — `frontends/standalone/letterbox.{h,cpp}`
      (`ComputeLetterboxedViewport`), a pure function covering both the
      pillarbox and letterbox cases plus degenerate inputs
- [x] Runtime hook to change scale: F5-F8 hotkeys (see Phase D) via
      `SetWindowScale` — no `--scale=N` CLI flag added (not needed once
      the window is freely resizable + hotkey-drivable; revisit if
      non-interactive/scripted use ever needs it)
- [x] Test: `tests/letterbox_test.cpp`, 6 cases covering exact-aspect-match,
      pillarbox, letterbox, degenerate inputs, and an exhaustive
      never-exceeds-window-bounds sweep
- [x] Bonus, not originally scoped here: F11 fullscreen toggle
      (`SDL_WINDOW_FULLSCREEN_DESKTOP`) — falls out for free once the
      letterboxed blit handles arbitrary drawable sizes anyway

## Phase B — Save states

Exit criterion: the player can save the exact current emulation state to a
file and reload it later, resuming play from that exact point (including
audio/timers, not just CPU+memory/visuals) — specifically to let QA
sessions like the sound investigation (`PHASE8_LOG.md`) capture "right
here" instead of re-driving a whole session to reproduce a bug.

**Status: CPU/memory (stage 1) and GL texture/visual state (stage 2a) are
both done and live-confirmed working from a cold relaunch. Audio/timer
state (stage 2b) is the one real gap left** — a cold `--load-state`
resumes gameplay and renders correctly, but produces no music/SFX until
the guest naturally re-triggers them.

Scoped in stages since host-side state (GL textures, HLE timers/active
sound voices/media handles) is real, separate work from guest CPU/memory
state, and turned out to itself split further once actually tested live
(see stage 2a) — a stage-1-only snapshot looked useful in isolation but
didn't actually solve the real motivating use case (a cold relaunch) until
stage 2a landed too.

- [x] **Stage 1 — CPU + guest memory only:**
  - [x] `ArmInterpreter::Serialize`/`Deserialize`: all 16 registers +
        CPSR, delegating memory to `Memory`'s own (deliberately excludes
        `call_out_base_`/`call_out_size_`/`call_out_handler_` -- harness
        wiring, not resumable game state, and the handler is a
        `std::function` that couldn't be serialized meaningfully anyway)
  - [x] `Memory::Serialize`/`Deserialize`: only pages actually allocated
        so far (page index + full 4KB contents each) — `Deserialize`
        fully replaces existing contents rather than merging
  - [x] `core/save_state.{h,cpp}`: `SaveState`/`LoadState`, a small magic
        ("ZBSS") + version header wrapping `ArmInterpreter::Serialize` —
        version mismatches or a wrong magic fail the load cleanly rather
        than misreading stage-2 fields that aren't there yet
  - [x] Wired into `game_probe.cpp`'s main loop: F1 saves to
        `<rom-path>.savestate` (single fixed slot for now), F2 loads —
        both show a real success/failure status message
        (`ShowStatusMessage`), not just silent success
  - [x] `--load-state` CLI flag: auto-loads the fixed-slot save right
        after setup, before the event loop starts, no F2 keypress
        needed — specifically so relaunching the tool to look at a
        player-reported bug can jump straight to their saved point
        non-interactively
  - [x] Documented the known gap (both here and in `README.md`'s own
        Controls section): reloading a stage-1-only save resumes guest
        code/data correctly, but host-side transient state (in-flight
        timers, active Mixer voices, MediaHle notify registrations) is
        NOT restored — expect audio/timer glitches immediately after a
        stage-1 load until the guest naturally re-arms them itself
  - [x] Tests: `tests/memory_test.cpp` (round-trip, empty-memory case,
        truncated-stream failure), `tests/cpu_test.cpp` (round-trip
        registers+CPSR+memory, empty-stream failure), and
        `tests/save_state_test.cpp` (the versioned wrapper: round-trip,
        wrong magic, future version, empty stream) — 9 new tests total
- [x] **Stage 2a — GL texture state (found live-necessary, not in the
      original scope below):** confirmed live that Stage 1 alone doesn't
      actually solve the real motivating use case (loading a save from a
      *cold* relaunch, e.g. to inspect a player-reported bug) — a fresh
      process's real OpenGL context has never uploaded any of the real
      sprite/UI textures the loaded guest memory references by ID, and
      real gameplay (not the fixed boot sequence) is what normally
      creates them, so a cold load rendered as solid-color garbage
      where real textures should be.
  - [x] `core/gl_texture_log.{h,cpp}`: `GlTextureRecordingBackend`, a
        `GlBackend` decorator recording every real
        GenTextures/DeleteTextures/BindTexture/TexParameter/TexImage2D
        call (forwarding everything unchanged to a wrapped real
        backend) — plus `Serialize`/`DeserializeGlTextureLog` and
        `ReplayGlTextureLog`, which re-issues the recorded calls
        against a target backend and verifies (not just assumes) the
        replayed real texture IDs match the recorded ones, relying on
        every real desktop GL driver's own practical (if not formally
        spec-guaranteed) deterministic sequential-ID-assignment
        behavior from a fresh context
  - [x] Wired into `game_probe.cpp`: `GlHle` now dispatches through a
        `GlTextureRecordingBackend` wrapping the real backend; F1
        appends the recorded log after the CPU/memory save; `--load-state`
        replays it (through the same recorder, so a later save's own
        history stays consistent) after `LoadState` restores CPU/memory
  - [x] `GlTextureRecordingBackend::ClearLog()`, called once right after
        the always-identical boot/setup sequence finishes: that
        sequence's own real texture creation doesn't belong in a saved
        log (a fresh relaunch already recreates it identically on its
        own) — replaying it on top of a process that just did the same
        boot would double-create those textures and desync every real
        ID from there on
  - [x] Fixed a real, live-confirmed bug this surfaced:
        `Sdl2UnifiedBackend`'s own `video_texture_` (used by
        `PushVideoFrame`) was created *lazily*, via a raw `glGenTextures`
        call entirely outside the `GlBackend` interface (so invisible to
        recording) and tied to real frame-presentation timing rather
        than guest instruction execution — meaning it could consume a
        real texture ID at a different relative point across two
        separate runs, permanently offsetting every later real ID by
        one. Now created eagerly, unconditionally, in the constructor
        (alongside the FBO's own `fbo_texture_`), matching
        `PushVideoFrame`'s own always-fixed real width/height
        (`width_`/`height_`) — confirmed live: a real save/cold-load
        round trip through actual gameplay (several enemies defeated)
        now renders correctly with no GL texture ID mismatch at all
  - [x] Tests: `tests/gl_texture_log_test.cpp` (recording captures real
        assigned IDs; `TexImage2D` pixel data is copied by value, not
        referenced; `ClearLog`; serialize/deserialize round-trip; replay
        reproduces the same textures on a fresh backend; replay reports
        failure on an ID mismatch rather than silently rendering wrong)
        — 6 new tests
- [ ] **Stage 2b — remaining host-side HLE state (audio/timers, original
      scope):** confirmed live this is still missing — a save/cold-load
      round trip resumes gameplay and renders correctly (see stage 2a)
      but produces no music/SFX until the guest naturally re-triggers
      them; this is the real remaining gap.
  - [ ] Give each stateful HLE class (`MediaHle`, `IShellHle`, `FileHle`,
        `ModRuntime`, `Mixer`, HID device state, ...) an explicit
        `Serialize`/`Deserialize` pair — likely a small shared interface
        or free-function pattern, not a virtual base (these classes
        aren't currently related by any common base and don't need to
        become so just for this)
  - [ ] `Mixer`: voice list (sample data can potentially be re-derived
        from the still-registered `MediaHle` object it came from rather
        than duplicated into the save file — confirm this is actually
        safe before assuming it, since a since-`Stop()`'d or reused
        object could make that unsafe)
  - [ ] `IShellHle`: pending timers (callback address, user data,
        remaining ms)
  - [ ] `MediaHle`: `media_by_object_` (decoded sample data is real audio
        data and could be large — consider whether to re-decode from the
        source VFS entry on load instead of embedding it verbatim)
  - [ ] `ModRuntime`: heap allocator state
  - [ ] Test: save/reload mid-sound-effect and confirm the sound actually
        keeps playing correctly afterward (this is the concrete case
        stage 1/2a are known to still get wrong)
- [ ] Multiple save slots (not just one "the" save state) — QA sessions
      often want several different bug-repro checkpoints alive at once
- [ ] Decide where save files live (a `saves/` dir alongside the ROM? a
      dedicated scratch dir?) — keep them out of the git-ignored
      `research/games/` tree's own concerns, this is tooling output, not
      research material

## Phase C — Controller binding (map keyboard-equivalent actions to a real gamepad)

Exit criterion: playing through `game_probe` with a real Xbox-style
(XInput) controller works exactly as well as keyboard does today — same
real HID button injection pipeline, driven by controller input instead of
(or alongside) keyboard input.

- [x] Wired `Backend::PollInput()` into `game_probe.cpp`'s main loop —
      polled once per tick, only while `Sdl2UnifiedBackend::HasController()`
      is true (a real `SDL_GameController` is connected), so it never also
      picks up `PollInput`'s own separate keyboard-fallback scheme
      (different keys than `SdlKeyToHidButton`'s own) alongside real
      keyboard handling
- [x] New, independently testable module
      (`frontends/standalone/zpad_edges.{h,cpp}`, mirrors `letterbox.{h,cpp}`'s
      precedent of a pure-logic frontend file linked directly into
      `zeebulator_tests` rather than through `zeebulator_core`):
  - [x] `DiffZPadButtonEdges(previous_buttons, current_buttons)` diffs two
        polled `ZPadState::buttons` snapshots into the same shape of
        press/release edges the existing `SDL_KEYDOWN`/`SDL_KEYUP` keyboard
        path already produces
  - [x] `StickTiltToDpadBits`/`NormalizeZPadState` — left stick tilt past a
        fixed deadzone (~24% of the real int16 range) quantizes down to the
        same D-pad bits digital input already uses, since Double Dragon's
        own recognized HID UID subset has no analog-axis UID at all, only
        the digital D-pad. Right stick isn't mapped (only one real D-pad's
        worth of directional HID input exists to feed).
  - [x] Test: fake `ZPadState`/stick sequences through the edge-detection
        and stick-quantization logic produce the expected press/release
        edges and D-pad bits (`tests/zpad_edges_test.cpp`) — needed no real
        SDL controller hardware
- [x] `ZPadButtonToHidUid` (`game_probe.cpp`) mirrors `SdlKeyToHidButton`'s
      mapping one-for-one (arrows -> D-pad, Start/Home -> the confirmed
      real `kHidUidBack`/title-progression button, shoulders -> the two
      real upper shoulder UIDs, face buttons -> Button_1-4 in ZPadState's
      West/South/North/East order, matching keyboard's Z/X/C/V order) — no
      AVK-code table: `SdlKeyToAvk` only covers number keys 0-9, which have
      no real gamepad equivalent, so there's nothing for a controller to
      feed into that path
- [x] Both keyboard and controller feed the exact same shared injection
      tail (`InjectHidButtonEvent`, extracted out of the old
      keyboard-only inline block) — no diverging codepaths
- [x] Keyboard and controller work simultaneously — the two input sources
      are polled/handled independently and never gate each other
- [ ] Live-verified against a real physical Xbox-style controller (only
      built + unit-tested so far, no real hardware in this environment to
      exercise `PollController`/`HasController` end to end)

## Phase D — Hotkeys + on-screen overlay (decided: no ImGui toolbar)

Exit criterion: the three features above are reachable live, without
editing code or restarting with different CLI flags, via hotkeys plus a
transient on-screen text overlay for feedback/status.

Decision: hotkeys + the existing text-overlay precedent, not a real
immediate-mode GUI library — cheapest, fastest to ship, no new
dependency, and it reuses `Sdl2UnifiedBackend::DrawFpsOverlay`'s already-
working transient-text-over-GL approach directly rather than building a
second, separate rendering path alongside it.

- [x] Hotkey table (F5-F8 scale, F9 overlay toggle, F11 fullscreen —
      documented in README.md's own Controls section; none collide with
      any real key `SdlKeyToAvk`/`SdlKeyToHidButton` forwards to the guest,
      since none of those use function keys):
  - [x] Cycle/select window scale (1x/2x/3x/4x, Phase A) — F5/F6/F7/F8
  - [ ] Save state / load state, with some way to pick a slot once Phase
        B has more than one (even if just "hold + number key" at first)
  - [ ] Enter/exit a controller-rebinding mode (Phase C)
  - [x] **Toggle the overlay's visibility itself** — F9
        (`SetOverlayVisible`), independent of the other hotkeys, so it
        can be turned off entirely once the player's just playing
- [x] Extended the old `DrawFpsOverlay` into `DrawOverlay` (still the
      same save/restore-GL-state pattern), with a `DrawText` helper and
      a second, transient status-message line (`ShowStatusMessage`,
      auto-clears after ~2.5s) rather than a second one-off text-drawing
      path — also extended the tiny 3x5 bitmap font with the letters
      status messages actually need (A/C/D/E/H/I/L/N/O/R/S/T/U/V/W/X)
- [x] Overlay-visible state persists across the hide toggle correctly —
      `DrawOverlay` returns immediately (draws nothing, including the FPS
      line) while `overlay_visible_` is false
- [ ] Persist settings (scale, bindings, and whether the overlay starts
      shown or hidden) across runs — a small config file (JSON/INI) next
      to wherever save states end up living (Phase B)

---

Not a replacement for `TASKS.md`'s own phases — this file is purely the
player-facing QA/dev-experience tooling the user asked for after Sound,
round fifteen (`PHASE8_LOG.md`) wrapped up, to make the next round of
manual bug-hunting faster.
