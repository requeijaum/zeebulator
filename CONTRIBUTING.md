# Contributing to Zeebulator

## Clean-room policy (required reading before contributing)

Zeebulator is a high-level-emulation (HLE) reimplementation of the BREW
runtime and Zeebo-specific extensions. It works by observing documented API
signatures/semantics and real game behavior, then writing an independent,
original implementation of that behavior — never by copying Qualcomm's or
Zeebo Inc.'s own code.

This matters legally, not just stylistically:

- **Never commit Qualcomm/BREW/Zeebo copyrighted material** to this
  repository: no SDK source or headers, no decompiled/disassembled BREW
  binaries, no firmware dumps, no game files, no leaked internal
  documentation. If you're not sure whether something you have is safe to
  reference (vs. commit), assume it isn't safe to commit and ask first.
- Public specifications and reference documentation (e.g. Qualcomm's BREW
  OEM API Reference, ARM's own public Technical Reference Manuals) are fine
  to *read and use as a guide* for implementing an interface's behavior —
  the same way anyone implementing a documented OS API from its spec would.
  What's not fine is transcribing or copying Qualcomm's actual
  implementation code into this project.
- Per-game reverse engineering (figuring out what API calls a specific
  title makes and how it expects them to behave) should be documented as
  *observed behavior*, not as leaked internal notes about how Qualcomm/Zeebo
  built the original.
- If a PR's description or commit history suggests code was copied from a
  decompiled/leaked source rather than independently written, it will be
  rejected regardless of whether it "works."

See PRD.md §6.3 for the full legal/licensing rationale.

## Getting the project locally

Zeebulator has no required firmware — it's HLE, so there's nothing
proprietary to install to build or run the emulator itself. You will need
your own legally-obtained Zeebo game dumps to actually test anything past
the earliest bring-up milestones (see TASKS.md).

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Build options:
- `ZEEBULATOR_BUILD_STANDALONE` (default ON) — the SDL2 dev/debug frontend
- `ZEEBULATOR_BUILD_LIBRETRO` (default ON) — the libretro core
- `ZEEBULATOR_BUILD_TESTS` (default ON) — the test suite (GoogleTest, fetched via CMake `FetchContent`)

## Testing

Every unit of work should come with tests covering it, and the full suite
(`ctest --test-dir build`) should pass before considering that work done —
this applies whether it's a CPU instruction, an HLE API stub, or a loader
format. Tests live in `tests/`, mirroring the module being tested (e.g.
`core/cpu/...` behavior gets covered by a `tests/cpu_test.cpp`-style file).

## Where things live

See ARCHITECTURE.md §4 for the directory layout and §3 for what each
component is responsible for. Roughly:
- `core/` — the emulation core. No platform/UI/windowing code belongs here.
- `core/brew/compat/` — per-game quirks and patches, keyed by title hash.
  Keep these out of the general HLE code (`core/brew/*.cpp`).
- `frontends/` — the libretro core shim and the standalone dev frontend.
  Both talk to `core/` only through `core/backend.h`.

## Scope

Check PRD.md before proposing large new scope — LLE hardware emulation,
network/Z-Credits provisioning, and netplay are explicit non-goals for now.
Compatibility work (getting more titles running) is always in scope.

## License

By contributing, you agree your contributions are licensed under this
project's GPLv3 license (see `LICENSE`).
