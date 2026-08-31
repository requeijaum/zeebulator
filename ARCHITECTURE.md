# Zeebulator — Architecture

Status: Draft v0.1
Companion to PRD.md — read that first for goals/scope.

## 1. Design Principles

1. **Core/frontend separation from day one.** The emulation core must not
   touch a window, an audio device, or a keyboard/gamepad directly. All I/O
   goes through an abstract backend interface. This is non-negotiable
   because the libretro core and the standalone build must be the *same*
   core logic behind two thin frontends — retrofitting this later (as many
   emulator projects learn the hard way) is far more expensive than doing it
   upfront.
2. **HLE, not LLE.** We intercept BREW AEE interface calls at the API
   boundary, not by emulating Qualcomm's actual BREW binary. No BREW OS
   binary is ever loaded or required.
3. **Clean-room implementation.** BREW API behavior is (re)implemented from
   the public API reference documentation and observed game behavior, never
   from decompiled/copied Qualcomm code.
4. **Get it correct before it's fast.** Ship an ARM interpreter first;
   promote to a JIT only once a game is provably running correctly, to keep
   the correctness/performance concerns decoupled during bring-up.
5. **One game at a time.** Compatibility work is inherently per-title
   reverse engineering. The architecture should make it cheap to add
   per-game quirks/patches without polluting the general HLE layer.

## 2. System Overview

```
                        +---------------------------+
                        |   Game dump (user-owned)   |
                        |  GGZ archive / BAR / MIF   |
                        +-------------+---------------+
                                      |
                                      v
                        +---------------------------+
                        |     Asset/Loader Layer     |
                        |  GGZ reader, BAR/MIF parse |
                        |  ARM .mod loader/relocator |
                        +-------------+---------------+
                                      |
                                      v
    +----------------+     +---------------------+     +------------------+
    |  Memory Bus /   |<--->|     ARM CPU Core    |<--->|  BREW HLE Layer   |
    |  Address Space  |     | (interpreter / JIT) |     |  (AEE interfaces) |
    +----------------+     +---------------------+     +--------+---------+
                                                                  |
                    +---------------------------------------------+---------------------------+
                    |                       |                     |                            |
                    v                       v                     v                            v
          +------------------+   +-------------------+   +-----------------+       +----------------------+
          | Graphics Subsys   |   |  Audio Subsystem   |   | Input Subsystem |       | Misc AEE services    |
          | GLES1.1 -> hostGL |   | PCM/ADPCM/MIDI/MP3 |   | IHID -> Z-Pad   |       | IFile, ISound, etc.  |
          +---------+---------+   +---------+---------+   +--------+---------+       +----------+-----------+
                    |                       |                       |                            |
                    +-----------------------+-----------------------+----------------------------+
                                                        |
                                                        v
                                     +-------------------------------------+
                                     |     Backend Abstraction Interface     |
                                     |  (video buffer, audio buffer, input) |
                                     +--------------------+------------------+
                                                           |
                              +----------------------------+----------------------------+
                              |                                                         |
                              v                                                         v
                +---------------------------+                          +---------------------------+
                |     Libretro Core Shim     |                          |   Standalone Frontend      |
                | retro_run/retro_load_game  |                          |  (SDL2, dev/debug builds)  |
                | video/audio/input callbacks|                          |                             |
                +---------------------------+                          +---------------------------+
```

## 3. Component Breakdown

### 3.1 ARM CPU Core
- Emulates the ARM1136J-S (ARMv6, ARM11 family) application core only — the
  ARM9 baseband core is out of scope (it never runs game code).
- v1: a straightforward ARMv6 interpreter (correctness-first, per Design
  Principle 4).
- v2+: swap in / wrap an existing ARMv6-capable JIT. `dynarmic` (0BSD
  license, used by Citra/yuzu-lineage projects) is the leading candidate:
  permissive license, proven ARMv6/v7 support, active maintenance. `unicorn`
  (GPLv2, Capstone family) is a fallback if `dynarmic` integration proves
  difficult.
- Interface: `IArmCore` — `Step()`, `Run(cycles)`, register access,
  exception/fault hooks (needed so the HLE layer can trap on SWI/undefined
  instructions used as the BREW call-out mechanism, or on calls to known
  unmapped "trap" addresses where AEE vtable functions are expected to
  live — exact trapping mechanism is a research task, see TASKS.md Phase 1).

### 3.2 Memory Subsystem
- Flat, sparse 32-bit address space matching the MSM7201A memory map
  (approximate regions: NAND-loaded app code/data, 160 MB working RAM,
  MMIO/register windows for anything HLE needs to fake presence of).
  Register-level MMIO emulation should be minimal/stubbed since HLE avoids
  touching real hardware registers wherever possible — the `dump/regs/`
  data from the preservation mirror is a fallback reference only if a game
  insists on poking real hardware registers directly (unlikely for
  well-behaved BREW apps, possible for ports of native engines).
- Exposes typed read/write plus bulk load (for mapping GGZ-extracted code
  segments in at load time).

### 3.3 Loader (GGZ / BAR / MIF / .mod)
- GGZ: gzip-based asset archive container. Reader built from clean-room
  understanding of the format (cross-referencing `ggzbrewtools`' documented
  behavior, not its source) — extracts contained assets (models, sprites,
  audio, `.mod` code modules) into the loader's working set.
- BAR / MIF: BREW archive / Module Information File — MIF carries module
  metadata (entry points, resource references) consistent with standard
  BREW semantics; needed to know how to place and start a `.mod`.
- `.mod` loader: maps the native ARM code/data segments into the memory
  subsystem, performs any relocation BREW's loader would have done, and
  sets the CPU core's entry point/initial register state to match what a
  real BREW app expects on entry (stack pointer, argument registers, etc.
  per the Developer Guide's documented ARM calling-convention notes).

### 3.4 BREW HLE Layer
This is the largest and most open-ended component — expect it to grow
incrementally, one API call at a time, driven by what real games actually
call.
- Reimplements the BREW AEE interface vtables the game links against:
  `IShell` (app lifecycle, notifications), `IDisplay` (2D framebuffer/blit
  ops), `IFile`/`IFileMgr` (virtual filesystem backed by the loaded GGZ
  contents — never the real filesystem directly, to keep games sandboxed
  and portable), `IMedia`/`ISound` (audio playback control), `INetwork`
  (stubbed — return "unavailable"/graceful-fail, since ZeeboNet is dead and
  emulating it is out of scope per PRD §4), and the Zeebo-specific `IHID`
  extension (gamepad input).
- Mechanism: when the CPU core traps a call into "BREW territory" (an
  address or instruction pattern that indicates a call through an AEE
  interface vtable), the HLE layer intercepts it, marshals arguments per
  the BREW OEM API Reference's documented signatures, executes the
  equivalent host-side behavior, and returns control to the ARM core with
  the expected return value/register state.
- Organized as one source file/module per interface (`ishell.cpp`,
  `idisplay.cpp`, `ifile.cpp`, ...) so coverage gaps are easy to see and
  per-game quirks stay localized.
- Per-game quirks/patches live in a separate `compat/` layer keyed by game
  ID/hash, never inline in the general HLE code — keeps the core logic
  clean per Design Principle 5.

### 3.5 Graphics Subsystem
- Translates `IDisplay` 2D blit operations and OpenGL ES 1.0/1.1 draw calls
  into the host's graphics API.
- **Confirmed architecture (Phase 5 research), not the originally-assumed
  one.** BREW-era OpenGL ES is *not* an OS-provided service reached through
  `IShell`. Real Qualcomm BREW SDK sample source (`EGL_1x.c`/`GLES_1x.c`/
  `GLES_ext.c`, statically compiled into every game's own `.mod` per the
  sample `.mak` build rules) shows that `gl*`/`egl*` calls are thin wrapper
  functions that dispatch through two real, documented AEE interfaces,
  **`IGL`** and **`IEGL`**, obtained via global pointers (`gpIGL`/`gpIEGL`)
  set up once at app startup (`IGL_Init`/`IEGL_Init`). Vtable slot order for
  both was read directly from the real `AEEGL.h` (extracted from a genuine
  Qualcomm "OpenGL ES Extension for BREW SDK 4.x" installer, MSI → CAB →
  source — same clean-room "read the header, never the implementation"
  method used for `IShell`/`IDisplay`/`IFile`): `AddRef, Release,
  QueryInterface`, then 77 `gl*` methods for `IGL` (80 slots total), 25
  `egl*` methods for `IEGL` (28 slots total). Confirmed applicable to the
  real target game too — Double
  Dragon's `.mod` contains the strings `eglGetColorBufferQUALCOMM` and
  `OpenGL.cpp`, consistent with this exact pattern.
- **Practical consequence**: this project does not need to design or ship
  its own GLES1.1 fixed-function state machine. `IGL`/`IEGL` HLE objects
  are built the same way `IShell`/`IDisplay`/`IFile` were (real vtable
  order, CPU call-out traps), and each implemented slot forwards to a real
  host OpenGL context — the actual fixed-function transform/lighting/
  texture-combiner math is the host GL driver's problem, not ours.
- The one new HLE-layer wrinkle this interface introduces: pointer
  arguments (`glVertexPointer`, `glTexImage2D`, `glDrawElements`'s index
  buffer, ...) are emulated ARM addresses, not host pointers, and must be
  copied out of emulated memory into real host-side buffers at draw/upload
  time rather than forwarded directly to host GL calls.
- v1 target: OpenGL (desktop core-compatible/compatibility-profile
  subset), since it's available on all three target OSes and libretro has
  first-class OpenGL hardware-render support (`retro_hw_render_callback`).
  Direct3D/Metal backends are a possible future optimization, not a v1
  requirement.
- macOS's OpenGL deprecation (compatibility/fixed-function profile capped
  and unavailable in modern contexts) remains the flagged cross-platform
  risk — see §10.

### 3.6 Audio Subsystem
- **Real interface confirmed (Phase 6 research)**: the app-facing surface
  is `IMedia` (`AEEIMedia.h`, extracted from the same genuine BREW MP SDK
  installer already used for Phase 4/5's research — MSI/NSIS → full
  extraction → `platform/media/inc/AEEMedia.h` + `AEEIMedia.h`) — a
  small, 14-slot vtable (`AddRef, Release, QueryInterface`, then
  `RegisterNotify, SetMediaParm, GetMediaParm, Play, Record, Stop, Seek,
  Pause, Resume, GetTotalTime, GetState`), confirmed against the real
  `INHERIT_IMedia`/`INHERIT_IQI` macros. Unlike `IDisplay`/`IFile`, almost
  everything (assigning the media data source, volume, repeat count,
  channel sharing, ...) goes through the generic `SetMediaParm`/
  `GetMediaParm(nParamID, p1, p2)` pair rather than dedicated per-feature
  slots — real Qualcomm sample source (`ctsoundmgr.c`, `AEEMediaUtil.c`,
  research-only) confirms this pattern and that codec selection is
  inferred from the file extension / MIME-sniffed content at
  `ISHELL_CreateInstance` time, not chosen explicitly by the app.
- **Real target-game codec need confirmed from Double Dragon's actual
  `sound.ggz`, not assumed**: 62 `.wav` files (sound effects + voice) and
  12 `.mid` files (background music) — zero MP3. Inspecting a real
  extracted `.wav`'s `fmt ` chunk shows `audio_format=0x1` (plain 16-bit
  PCM, mono, 22050Hz) on every file checked — **not IMA-ADPCM** despite
  the real BREW SDK sample (`ctsoundmgr.c`) naming its own bundled test
  assets `ADPCM_FILE_*`. Real `.mid` files are genuine Standard MIDI
  Format 1 files. Consequence: PCM decode (trivial — these files barely
  need "decoding", just RIFF/WAVE container parsing) and MIDI playback
  are the two codecs that actually matter for M1; IMA-ADPCM and MP3
  support are real, legitimate BREW codecs (the SDK sample genuinely uses
  both) but not required by the target title — deprioritized, not
  dropped, same category of decision as BAR-format parsing in Phase 2.
- Mixer + ring buffer feeding the Backend Abstraction Interface (§3.8)
  rather than any OS audio API directly — `Backend::PushAudioSamples`
  already exists (defined since Phase 0) and is finally used starting
  this phase.

### 3.7 Input Subsystem
- Reimplements the Zeebo `IHID` extension, mapping the standard Z-Pad
  layout (D-pad, two analog sticks, 7 buttons, ZL/ZR, Home) onto whatever
  input state the backend abstraction provides (a RetroArch joypad, or an
  SDL2 gamepad/keyboard in the standalone build). Input mapping is
  configurable but ships with a sane default matching a standard
  Xbox-layout controller.

### 3.8 Backend Abstraction Interface
- The seam between "the emulator" and "the outside world." Defines:
  `PushVideoFrame(buffer, w, h, format)`, `PushAudioSamples(...)`,
  `PollInput() -> ZPadState`, plus lifecycle hooks (`OnLoad`, `OnReset`,
  `OnSaveState`/`OnLoadState`). Both frontends implement this interface;
  the core never knows which one it's talking to.

### 3.9 Libretro Core Shim
- Implements the standard libretro API surface: `retro_init`,
  `retro_deinit`, `retro_get_system_info`, `retro_get_system_av_info`,
  `retro_load_game`, `retro_run`, `retro_reset`, `retro_serialize` /
  `retro_unserialize` (save states — backed by a full memory + CPU register
  snapshot from the core), `retro_set_environment` and friends.
- Owns zero emulation logic itself — it's purely an adapter translating
  libretro's callback style into calls against the Backend Abstraction
  Interface and the core's public API.
- Ships a standard `.info` file (`zeebo.info` or similar) describing
  supported extensions, core name, and required firmware (**none** — see
  PRD §3.2), as libretro core conventions expect.

### 3.10 Standalone Frontend
- A minimal SDL2-based window/audio/input frontend, used primarily for
  development and debugging (faster iteration than rebuilding/reloading a
  libretro core in RetroArch on every change). Not the primary end-user
  distribution target, but useful and low-cost to maintain given the
  Backend Abstraction Interface already forces a clean seam.

## 4. Directory Layout (proposed)

```
zeebulator/
  core/                   # the emulation core — no platform/UI code allowed here
    cpu/                  # ARM interpreter + JIT backend(s)
    memory/
    loader/                # GGZ/BAR/MIF/.mod parsing
    brew/                  # BREW HLE layer, one file per AEE interface
      compat/               # per-game quirks/patches, keyed by title hash
    graphics/              # GLES1.1 -> host GL translation
    audio/                 # codec decoders + mixer
    input/                 # IHID -> ZPadState mapping
    backend.h               # Backend Abstraction Interface (the seam)
  frontends/
    libretro/               # libretro core shim (retro_*.cpp)
    standalone/             # SDL2 dev/debug frontend
  third_party/             # vendored deps (dynarmic, etc.) via git submodule/CMake FetchContent
  tools/                   # dev tooling: GGZ inspector, HLE call tracer, etc.
  tests/                   # unit + integration tests
  docs/
  CMakeLists.txt
  PRD.md / ARCHITECTURE.md / TASKS.md
```

## 5. Boot Sequence (data flow)

1. Frontend (libretro or standalone) receives a game path, calls into
   `core::LoadGame(path)`.
2. Loader opens the GGZ archive, extracts the `.mod` code module + BAR/MIF
   metadata + referenced assets into the in-memory virtual filesystem.
3. Loader maps the `.mod`'s code/data into the Memory Subsystem and sets up
   initial CPU register state per BREW's documented app entry convention.
4. Frontend's run loop calls `core::RunFrame()` each tick.
5. CPU core executes ARM instructions until it hits a trapped BREW
   call-out, a fixed instruction budget, or a vsync-equivalent yield point.
6. On a trapped call-out, BREW HLE Layer dispatches to the relevant AEE
   interface implementation, which may push video/audio through the
   Backend Abstraction Interface or read input state.
7. Frontend takes whatever the Backend Abstraction Interface accumulated
   (video frame, audio samples) and hands it to the OS/libretro as
   appropriate.

## 6. Libretro Integration Details

- No BIOS/firmware requirement to declare (a genuine advantage of the HLE
  approach — most libretro cores need to document a required-firmware list;
  ours doesn't).
- Core options (`retro_variable`) for things like: interpreter-vs-JIT
  toggle (useful during bring-up / for debugging), input mapping presets,
  aspect ratio (Zeebo native output is 640×480 4:3).
- Save states must be feasible from a single memory+register snapshot,
  since HLE has no complex hardware state to serialize beyond what the
  core already tracks (open question: whether any AEE interface holds
  state that needs explicit (de)serialization hooks — likely yes for things
  like open file handles into the virtual GGZ filesystem).

## 7. Build & CI

- CMake as the single build system across all three OS targets and both
  frontend types (a `ZEEBULATOR_BUILD_LIBRETRO` / `ZEEBULATOR_BUILD_STANDALONE`
  option each).
- GitHub Actions matrix: `windows-latest`, `macos-latest` (arm64 runner),
  `ubuntu-latest`, building both frontend targets on each.
- Third-party deps (`dynarmic`, SDL2, etc.) pulled via CMake `FetchContent`
  or git submodules — pin versions, no system-package assumptions, to keep
  builds reproducible across the three OSes.

## 8. Testing Strategy

Framework: **GoogleTest** (BSD-3-Clause), pulled via CMake `FetchContent`,
run through CTest (`ctest --test-dir build`). Every unit of work is expected
to land with tests covering it, and the full suite is expected to pass
before that work is considered done — not just at phase boundaries.

- **CPU core**: unit tests against known ARMv6 instruction-behavior test
  vectors (independent of any Zeebo-specific content — pure ISA
  correctness).
- **Loader**: unit tests against a small set of publicly-available BREW
  sample apps (from the official SDK) as fixtures, since their format/
  behavior is officially documented and low-risk to include in test
  fixtures if license terms allow, or regenerate synthetic fixtures if not.
- **HLE layer**: integration tests that boot known-simple titles/samples
  and assert on observable milestones (reaches main menu, responds to
  input) rather than pixel-perfect output initially.
- **Regression**: once M1 (first playable game) is hit, that game becomes a
  permanent CI regression fixture — never let a previously-working title
  silently break.

## 9. Technology Choices & Rationale

| Concern | Choice | Rationale |
|---|---|---|
| Language | C++17/20 | Libretro core convention is overwhelmingly C/C++; needed for low-level CPU/memory work and to integrate `dynarmic` directly |
| Build system | CMake | Cross-platform standard, plays well with libretro-super/buildbot expectations |
| ARM core (JIT) | `dynarmic` (0BSD) | Permissive license (no copyleft entanglement either way), proven ARMv6 support, active project — still the primary choice on technical merits |
| ARM core (fallback) | `unicorn` (GPLv2) | Backup if `dynarmic` integration stalls; license-compatible with a GPLv3 project (GPLv2-only code can be included in a GPLv3-or-later work — verify the exact GPLv2 vs. "GPLv2 or later" terms on `unicorn` before depending on it) |
| Graphics | Desktop OpenGL via libretro HW render | Available cross-platform, first-class libretro support; revisit Metal/D3D only if OpenGL deprecation on macOS becomes a real blocker |
| Standalone windowing/audio/input | SDL2 (zlib license) | Cross-platform, minimal, dev-focused, permissively licensed — fine as a dependency under GPLv3 |
| Testing | GoogleTest (BSD-3-Clause) via CMake `FetchContent`, run through CTest | Standard, well-supported, integrates cleanly with the existing CMake-based build across all three OSes |
| License | **GPLv3** (decided — PRD §6.3) | Matches emulation-scene convention (Dolphin, PCSX2, RPCS3, PPSSPP, yuzu, RetroArch); copyleft prevents a closed-fork outcome |

## 10. Key Technical Risks (architecture-specific)

- **Call-trapping mechanism is unresearched.** How BREW app code actually
  invokes AEE interface methods (direct vtable calls into real
  addresses vs. SWI/syscall-style trap) needs to be nailed down early — it
  determines how the CPU core exposes hooks to the HLE layer. This is
  Phase 1 research work, not an assumption to build on blind.
- **GLES-on-macOS.** Apple's OpenGL deprecation may force the
  shader-generation path (§3.5 option b) sooner than Windows/Linux need it,
  adding complexity asymmetry across platforms.
- **Per-game HLE coverage gaps are open-ended by nature** — the `compat/`
  layer design contains this, but it doesn't eliminate the underlying
  reverse-engineering cost per new title.
