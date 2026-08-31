# Zeebulator

A free, open-source, high-level-emulation (HLE) emulator for the
[Zeebo](https://en.wikipedia.org/wiki/Zeebo) game console — a Qualcomm
MSM7201A / BREW-based console sold in Brazil, Mexico, and Indonesia
(2009–2011). Targets native builds on Windows, macOS, and Ubuntu/Linux,
plus a [libretro](https://www.libretro.com/) core for RetroArch.

**Status: early development.** There is no working emulation yet — this
repo currently has a buildable project skeleton and a CPU/memory core in
progress. See [TASKS.md](TASKS.md) for where things stand.

## Why HLE?

Zeebo isn't fixed-function console hardware — it's a Qualcomm MSM7201A
(ARM1136/ARM11 @ 528 MHz) running **Qualcomm BREW 4.0.2** as its OS layer,
the same mobile runtime used on feature phones of that era. Games are
native ARM code written against BREW's C/C++ APIs plus OpenGL ES 1.0/1.1.
Zeebulator reimplements the BREW API surface in high-level emulation
rather than emulating the SoC at the hardware level — which also means
**no copyrighted firmware/BIOS is required** to run it. You just need your
own legally-obtained game dumps.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Build options (all default `ON`):
- `ZEEBULATOR_BUILD_STANDALONE` — SDL2 dev/debug frontend
- `ZEEBULATOR_BUILD_LIBRETRO` — libretro core
- `ZEEBULATOR_BUILD_TESTS` — test suite (GoogleTest)

## Controls

Keyboard mapping for `zeebulator_game_probe` (the current standalone
game-running tool), if no gamepad is connected:

| Key | Action |
| --- | --- |
| Arrow keys | D-pad |
| Backspace / Enter | Back (menu confirm / title progression) |
| Z / X / C / V | Face buttons 1–4 (X is punch/attack in Double Dragon) |
| Q / E | Left / right shoulder |

A connected Xbox-style (XInput) gamepad works alongside the keyboard
(not instead of it — both work at the same time):

| Button | Action |
| --- | --- |
| D-pad / left stick | D-pad |
| A / B / X / Y | Face buttons West / East / South / North (X is punch/attack in Double Dragon) |
| Left / right shoulder | Left / right shoulder |
| Start | Back (menu confirm / title progression) |

A/X are swapped from what a "standard" Xbox layout would suggest --
live-tested against a real Xbox Wireless Controller over Bluetooth on
this project's own dev desktop, which genuinely reports A and X
swapped (a real quirk of that device/driver/SDL combination, not a
choice). If your controller doesn't have this quirk, A and X will feel
swapped from what you'd expect; see `Sdl2UnifiedBackend::PollController`'s
own doc comment.

Frontend hotkeys (always active, independent of the game controls above):

| Key | Action |
| --- | --- |
| F1 / F2 | Save / load state (single slot, next to the ROM file as `<rom>.savestate`) |
| F5 / F6 / F7 / F8 | Window scale 1x / 2x / 3x / 4x |
| F9 | Show/hide the on-screen overlay (FPS + status messages) |
| F11 | Toggle fullscreen |

The window is also freely resizable by dragging its edge — the emulated
640x480 output is always letterboxed to fit, at any size.

Save states capture CPU registers, full guest memory, and (via
`--load-state`, see below) the real GL texture uploads needed to render
correctly again from a cold relaunch. They do not yet capture host-side
audio/timer state — expect no music/SFX right after a cold load until
the game naturally re-triggers them. See
[TASKS_TOOLING.md](TASKS_TOOLING.md) Phase B for that remaining gap.

Pass `--load-state` on the command line to auto-load the fixed-slot save
right at startup, without needing to press F2 — this is also what
replays the real GL texture uploads needed for correct visuals from a
cold process (F2, loading within the same already-running session,
skips that replay since the live session's textures are already
correct). Useful for jumping straight back to a previously-saved point
non-interactively, e.g. relaunching to inspect a reported bug.

## Project docs

- [PRD.md](PRD.md) — goals, scope, milestones
- [ARCHITECTURE.md](ARCHITECTURE.md) — component design, directory layout, technology choices
- [TASKS.md](TASKS.md) — phased task breakdown and current progress
- [PHASE8_LOG.md](PHASE8_LOG.md) — detailed investigation log for Phase 8's real-game bring-up (Double Dragon)
- [CONTRIBUTING.md](CONTRIBUTING.md) — clean-room policy (read before contributing) and dev setup

## Legal

This project distributes no Qualcomm, BREW, or Zeebo/Tectoy copyrighted
material — no firmware, no SDK files, no game data. You are responsible
for supplying your own legally-obtained game dumps. Zeebulator is an
independent, fan-made project with no affiliation to Zeebo Inc., Tectoy,
or Qualcomm.

## License

[GPLv3](LICENSE).
