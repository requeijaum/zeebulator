# Zeebulator — Product Requirements Document

Status: Draft v0.1
Owner: Marcelo
Last updated: 2026-07-16

## 1. Summary

Zeebulator is a free, open-source, high-level-emulation (HLE) emulator for the
Zeebo game console (Tectoy/Qualcomm, 2009, Brazil/Mexico/Indonesia). It targets
native builds on Windows, macOS, and Ubuntu/Linux, plus a **libretro core** so
it can run inside RetroArch. The project exists for preservation: the Zeebo
online store has been offline since 2011, the hardware is scarce outside
Brazil, and no other open-source emulator for the platform exists today.

## 2. Background

Zeebo is not a traditional fixed-hardware console. It is a Qualcomm
MSM7201A-based device (ARM1136J-S / ARM11 CPU @ 528 MHz, Adreno 130 GPU,
160 MB RAM, 1 GB NAND) running **Qualcomm BREW 4.0.2** as its OS/middleware
layer — the same mobile runtime used on feature phones of that era. Games are
native ARM machine code (BREW `.mod` modules) using BREW's C/C++ AEE
interfaces plus OpenGL ES 1.0/1.1 for graphics, packaged in a GGZ archive
container alongside BAR/MIF metadata files. Distribution was over a 3G
network (ZeeboNet); those servers are dead, but the commercial game library
has been dumped and preserved (archive.org).

Public technical resources available to this project:
- Official Zeebo SDK (v0.93, v1.2.4) and Developer Guide (mirrored, publicly archived)
- BREW OEM API Reference for MSM chipsets (mirrored, publicly archived)
- ARM1136 Technical Reference Manual (ARM's own public document)
- `ggzbrewtools` — an existing open-source(-adjacent) GGZ archive format reader, useful as *format documentation*, not as code to vendor wholesale
- Full commercial game library preserved on archive.org
- Raw NAND/RAM/register dumps from real hardware (preservation mirror)

## 3. Goals

1. **HLE emulation** of the BREW 4.0.2 runtime and Zeebo-specific extensions
   (input/HID), running unmodified, user-supplied dumps of commercial Zeebo
   games.
2. **No copyrighted firmware/BIOS required.** Because this is HLE (not LLE),
   the emulator ships with zero Qualcomm/Zeebo binary or SDK material. Users
   only supply their own legally-obtained game dumps. This is a hard
   requirement, not just a nice-to-have — it's what keeps the project
   distributable.
3. **Cross-platform native builds**: Windows (x86_64), macOS (arm64/x86_64),
   Ubuntu/Linux (x86_64), via a shared C++ core with thin per-OS
   packaging.
4. **Libretro core**: the same emulation core compiled as a `.dll`/`.dylib`/`.so`
   libretro core, loadable in RetroArch, with standard input/video/audio/save-state
   integration.
5. **Free and open source**, under GPLv3 (see §6.3) — matches standard
   practice across the emulation scene and keeps the copyleft protection
   against a closed-fork outcome.
6. **Playability over accuracy**: prioritize getting real games running
   correctly at full speed over cycle-accurate hardware fidelity. This is a
   BREW/HLE emulator, not a silicon simulator.

## 4. Non-Goals / Out of Scope (v1)

- Low-level (LLE) emulation of the MSM7201A SoC, baseband/ARM9 modem core, or
  boot ROM. HLE only.
- Emulating ZeeboNet's over-the-air provisioning, Z-Credits purchase flow, or
  any network service (the servers are dead; nothing to talk to, and it's out
  of scope even if it weren't).
- Mobile/handheld ports (Android/iOS/Steam Deck as a *target build*) — may
  come later since libretro cores port relatively easily, but not a v1
  commitment.
- Netplay.
- 100% game compatibility. Track compatibility incrementally; a small list of
  fully-playable titles at launch is the realistic target.
- Redistributing any Zeebo game files, SDK files, or firmware. The project
  distributes only the emulator.

## 5. Users & Use Cases

- **Preservationists / retro enthusiasts** who own or have legally dumped
  Zeebo game files and want to play them on modern hardware.
- **RetroArch users** who want Zeebo alongside their other cores in a unified
  frontend, with standard RetroArch features (save states, shaders, input
  remapping).
- **Contributors / future reverse engineers** who want an open codebase to
  extend BREW API coverage and add game compatibility.

## 6. Requirements

### 6.1 Functional

- FR1: Load a game from a user-supplied GGZ archive (and/or extracted BAR/MIF/`.mod` files).
- FR2: Execute the game's native ARM code via an interpreter and/or JIT.
- FR3: Intercept and service BREW AEE interface calls (IShell, IDisplay,
  IFile, IMedia/ISound, INetwork-stub, IHID) in HLE.
- FR4: Translate OpenGL ES 1.0/1.1 draw calls to the host's graphics API.
- FR5: Decode and mix supported audio formats (PCM, IMA-ADPCM, MIDI, MP3).
- FR6: Map RetroArch/host joypad input to the Zeebo Z-Pad HID layout.
- FR7: Provide save states (at minimum: full memory + CPU register
  snapshot) in the libretro build.
- FR8: Run identically (same core logic) across standalone native builds and
  the libretro core — no platform-specific emulation behavior.

### 6.2 Platform Requirements

- PR1: Native builds for Windows 10+, macOS 12+ (arm64 + x86_64), Ubuntu
  22.04+ (and reasonably portable to other modern distros).
- PR2: A libretro core targeting the standard libretro API, buildable via
  the same CMake project, following libretro core packaging conventions
  (`.info` file, core options via `retro_variable`, no direct
  windowing/audio/input — everything through libretro callbacks).
- PR3: No hard dependency on proprietary/non-redistributable libraries in
  the default build. Any optional dependency (e.g., a proprietary GLES
  translation layer) must be swappable for an open alternative.

### 6.3 Legal / Licensing

- LR1: Project license: **GPLv3** (decided). Matches standard practice
  across the emulation scene (Dolphin, PCSX2, RPCS3, PPSSPP, yuzu/Citra,
  RetroArch itself, etc. are all GPL), and its copyleft prevents a
  closed binary-only fork of the project down the line. Practical
  implication: a GPLv2-only dependency (e.g. `unicorn`) is fine to
  depend on — GPLv2 and GPLv3 compatibility should still be checked
  case-by-case, but there's no permissive-license constraint to protect
  the way there would be under MIT — see ARCHITECTURE.md §9.
- LR2: No Qualcomm/BREW/Zeebo copyrighted binaries, headers, or SDK material
  may be committed to the repository. The BREW OEM API reference is used as
  *documentation* to inform a clean-room HLE reimplementation of the
  interfaces (function signatures/semantics), analogous to reimplementing a
  documented OS API — not copying Qualcomm's implementation. Contributors
  must not paste SDK source into the codebase.
- LR3: Project naming/branding should avoid implying official affiliation
  with the Zeebo or Qualcomm trademarks (defunct company, but trademarks can
  outlive it).
- LR4: This is not formal legal advice; if the project gains traction,
  revisit with an actual IP lawyer, particularly around API reimplementation
  in your jurisdiction.

## 7. Success Metrics / Milestones

- **M0 — Hello World — ACHIEVED.** A minimal BREW-shaped app boots and
  reaches a visible, correctly-painted screen via the standalone frontend,
  screenshot-verified. Used our own from-scratch test app (compiled with a
  real ARM cross-compiler, not Qualcomm's SDK/toolchain) rather than a
  compiled SDK sample, since we don't have Qualcomm's `elf2mod.exe` and
  can't legally distribute a compiled version of their sample source
  either way — see TASKS.md Phase 3 for the full writeup.
- **M1 — First Commercial Game**: One simple commercial title (candidate:
  *Double Dragon*, a simple 2D game and a reasonable first target) is fully
  playable start-to-finish at full speed, standalone build.
- **M2 — Libretro Parity**: The same game is fully playable through the
  libretro core in RetroArch, with working save states.
- **M3 — Compatibility Growth**: A public, maintained compatibility list;
  target 5–10 playable titles.
- **M4 — Packaged Releases**: Signed/notarized builds (where applicable) for
  Windows/macOS/Linux, and a libretro core submission considered for the
  official buildbot.

## 8. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| BREW API surface is large; undocumented edge cases per game | High — could stall on every new title | Scope v1 to titles that exercise a minimal API subset (samples, simple 2D games) before attacking 3D/engine-heavy titles |
| GPU translation (GLES 1.0/1.1 fixed-function → host GL) has subtle behavior gaps | Medium — visual bugs, not crashes | Start with a strict state-machine translation layer; expect and budget time for visual glitches during bring-up |
| Per-game engine quirks (games didn't all use BREW "properly") | High — likely the single biggest time sink | Treat each new game as its own mini reverse-engineering task; don't assume API coverage generalizes |
| Legal ambiguity around clean-room API reimplementation | Low-medium | Document clean-room process; never commit Qualcomm source/binaries; revisit with legal counsel if the project grows |
| Solo/small-team bandwidth vs. project scope | High | Milestone-gate ruthlessly (§7); resist scope creep into LLE or network emulation |
| Trademark/naming friction | Low | Keep branding clearly unofficial/fan-made |

## 9. Open Questions

- ~~Project/repo name~~ — **Resolved: "Zeebulator."**
- ~~Open source or not~~ — **Resolved: yes, open source, confirmed by project owner.**
- ~~Final license~~ — **Resolved: GPLv3.**
- CPU core strategy: interpreter-only first, or integrate an existing
  ARMv6-capable JIT (e.g., `dynarmic`, 0BSD-licensed) from day one? (See
  ARCHITECTURE.md §9 for tradeoffs.)
