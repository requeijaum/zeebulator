# Zeebulator

[![Build & Tests](https://img.shields.io/badge/tests-587%2F587%20passing-brightgreen.svg)]()
[![Compatibility](https://img.shields.io/badge/compatibility-96.8%25%20(61%2F63%20titles)-blue.svg)](docs/COMPATIBILIDADE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-informational.svg)]()
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

**Zeebulator** is a modern, high-level-emulation (HLE) open-source emulator for the **Zeebo** game console (Brazil, Mexico, and Indonesia, 2009–2011), built on the **Qualcomm MSM7201A** chipset and the **Qualcomm BREW 4.0.2** mobile operating system.

Unlike traditional fixed-function consoles, the Zeebo operates as a connected mobile compute platform executing native 32-bit ARM binaries against BREW's component-based Application Execution Environment (AEE) and hardware-accelerated OpenGL ES 1.0/1.1. Zeebulator reimplements BREW's system interfaces, virtual filesystem, graphics pipeline, and peripheral drivers natively, **requiring no proprietary BIOS, NAND dumps, or copyrighted system firmware** to run games.

---

## Current Status & Compatibility

Zeebulator has achieved feature and architectural compatibility parity across the entire official retail catalog:

- **62 of 63 official NAND titles (98.4%)** load modules (`AEEMod_Load`), instantiate their applets (`CreateInstance`), and reach the main application event loop (`EVT_APP_START` completed or interactive system shell).
- **26 titles with verified active in-game guest loops** dispatching cooperative threads and rendering at 40–60 FPS.
- **Z-Wheel (Official System Shell) Fully Bootable**: Boots `tectoy.mod`, opens and queries SQLite preferences, builds the 24-slot visual widget tree, loads all 15 carousel titles, and renders the top stage and bottom roller interface.
- **Homebrew & Injections**: Validated support for OpenZeebo homebrew and BREW mobile ports (including *Kingdom Hearts V-CAST*).
- **Automated Test Suite**: **587 unit and integration tests passing (100% green)** via GoogleTest and `ctest`.

For detailed title-by-title status and telemetry breakdown, see [**`docs/COMPATIBILIDADE.md`**](docs/COMPATIBILIDADE.md).

---

## Architectural Highlights

### 1. High-Performance ARM Execution & Memory
- **Interpreter Fast-Paths**: Page-level memory resolution fast-paths for 16-bit and 32-bit memory operations (`Read16`, `Read32`, `Write16`, `Write32`), achieving **~48 MIPS** on modern x86_64 CPUs.
- **ARMv5TE DSP Instructions**: Native execution of saturated math and DSP instructions (e.g., `SMLABB`, `SMLABT`, `SMULBB`).
- **ARM/Thumb Interworking**: Strict PC bit-masking (bit 0 selects Thumb state; bit 1 is masked to guarantee proper halfword alignment).
- **CP15 Coprocessor Emulation**: Functional emulation of ARM11 core control registers (architecture ID, cache, and MMU control masks).
- **Soft-Float Support**: Complete IEEE 754 floating-point emulation matching the ARM BREW toolchain ABI.

### 2. Qualcomm BREW 4.0.2 HLE Runtime
- **`IShell`**: Complete applet lifecycle management, sub-millisecond timers (`SetTimer`, `CancelTimer`, `Resume`), cooperative thread scheduling (`IThread`), MIME type detection (`DetectType`, `GetHandler`), and cross-module event dispatch (`SendEvent`).
- **`IFileMgr` & `IFile`**: Virtual Filesystem (VFS) with POSIX/FAT32 lexical normalization, parent directory traversal (`../`), case-insensitive hash lookups, and transparent fallback resolution.
- **`IDisplay` & `IBitmap`**: 2D software framebuffers, compatible bitmap allocations (`CreateCompatibleBitmap`), and `IDIB` 640×480 RGB565 structures.
- **`IGraphics` & `IGLES11` / `QEGL`**: Full desktop OpenGL translation for Qualcomm OpenGL ES 1.1, supporting Qualcomm/Adreno extensions (`GL_AMD_compressed_ATC_texture`, `EGL_QUALCOMM_swap_control`, `eglGetColorBufferQUALCOMM`) and `eglGetProcAddress` trap resolution.
- **`IHID`**: Real gamepad input injection with native Zeebo Game Controller button UIDs, analog stick mappings, and key event dispatching.
- **`ISQLMgr` & `ISQLDatabase`**: Full SQLite 3.46.1 integration managing `tt_prefs.db`, `tt_dlqueue.db`, and `asset_cache`.
- **Widget & UI Framework**: Complete 24-slot implementation of BREW's widget hierarchy (`IWIDGET_HandleEvent` with inverted boolean convention, `IVectorModel`, `OwnerDrawWidget`, `StageWidget`, and TrueType font rendering).

### 3. Archive & Container Loaders
- **AEZ (Fishlabs)**: Native archive parser supporting both zlib-compressed streams and raw uncompressed payloads (`compressed_size == 0xFFFFFFFF`).
- **FUFS (`.vfs`)**: 12-byte header entry parser with reverse-engineered case-insensitive polynomial hash matching (`h = h * 67 + (toupper(c) - 113)`).
- **SAR (`SWVARC`)**: Full container extraction for Superscape 3D games.
- **BAR & PAKZ**: Direct parsing and extraction of BREW resource archives and compressed packages.
- **MIF (`Module Information File`)**: Automatic extraction of application `ClassID` records directly from binary section tables.

### 4. Audio Engine
- **Dynamic Stereo Resampling**: Real-time linear stereo resampling supporting arbitrary guest sample rates with zero latency.
- **WAV Stream Capture**: Real-time audio logging to disk via `ZEEB_DUMP_AUDIO=output.wav`.
- **Multimedia Codec Hierarchy**: Registration and factory routing for MP3 (`0x01005502`), MIDI (`0x01005501`), QCP (`0x01005503`), PCM (`0x01005511`), ADPCM, AMR, AAC, MMF, and PMD.

### 5. Video Backend & Tooling
- **OpenGL FBO Capture**: Framebuffer Object (FBO) integration capturing real OpenGL ES frames via inverted `glReadPixels` into standard PPM files.
- **NDJSON Remote Control Server**: Programmatic TCP socket interface for automation, single-stepping (`step`), state inspection (`state`), and on-demand screenshot dumping (`screenshot`).
- **HTTP Mirror & Web Debug UI**: Built-in HTTP server (port `48750`) providing live web previews of the framebuffer, CPU registers, and execution logs.

---

## Building

### Prerequisites

| Platform | Required Packages |
|---|---|
| **Ubuntu / Debian** | `build-essential cmake libsdl2-dev libgl1-mesa-dev zlib1g-dev liblzma-dev` |
| **Fedora / RHEL** | `gcc-c++ cmake SDL2-devel mesa-libGL-devel zlib-devel xz-devel` |
| **Arch Linux** | `base-devel cmake sdl2 mesa zlib xz` |
| **macOS** | `brew install cmake sdl2 xz` |
| **Windows** | Visual Studio 2019+ or Clang, with CMake and SDL2 |

### Build Instructions

```sh
# Clone the repository
git clone https://github.com/requeijaum/zeebulator.git
cd zeebulator

# Generate build files
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Compile all targets
cmake --build build -j$(nproc)

# Run the test suite
ctest --test-dir build --output-on-failure -j$(nproc)
```

### CMake Configuration Options
- `-DZEEBULATOR_BUILD_STANDALONE=ON` — Builds the standalone SDL2 dev/test frontend (`zeebulator_standalone`).
- `-DZEEBULATOR_BUILD_LIBRETRO=ON` — Builds the libretro core for RetroArch (`zeebulator_libretro.so`).
- `-DZEEBULATOR_BUILD_TESTS=ON` — Builds the automated GoogleTest suite (`zeebulator_tests`).

---

## Input & Controls

Zeebulator supports both modern gamepads (Xbox, PlayStation, generic USB HID controllers) and standard PC keyboards. Both the **Handheld layout** (Z/X/C/V) and the **Arcade / MAME layout** (WASD + J/K/U/I) are simultaneously active.

### Controller & Keyboard Mapping

| Zeebo Game Controller (Z-Pad) | Handheld Keyboard Layout | Arcade / MAME Layout | Standard Gamepad (XInput) |
|---|---|---|---|
| **D-Pad Up** | `Up Arrow` | `W` | D-Pad Up / Left Stick Up |
| **D-Pad Down** | `Down Arrow` | `S` | D-Pad Down / Left Stick Down |
| **D-Pad Left** | `Left Arrow` | `A` | D-Pad Left / Left Stick Left |
| **D-Pad Right** | `Right Arrow` | `D` | D-Pad Right / Left Stick Right |
| **Button 1 (A)** | `Z` | `J` | Button A / Cross |
| **Button 2 (B)** | `X` | `K` | Button B / Circle |
| **Button 3 (C)** | `C` | `U` | Button X / Square |
| **Button 4 (D)** | `V` | `I` | Button Y / Triangle |
| **Left Shoulder (L)** | `Q` | `1` | Left Bumper (LB) / Trigger (L2) |
| **Right Shoulder (R)** | `E` | `2` | Right Bumper (RB) / Trigger (R2) |
| **Back / Confirm** | `Return` / `Backspace` | `Space` | Back / Select |
| **Home / Menu** | `Escape` | `Escape` | Guide / Home |

### Development Hotkeys
- **`F1`**: Save state to disk.
- **`F2`**: Quick-load state from disk.
- **`F5` – `F8`**: Window scale factor (1x, 2x, 3x, 4x).
- **`F9`**: Toggle performance & telemetry overlay.
- **`F11`**: Toggle borderless fullscreen window.

---

## Tools & Utilities

### 1. `zeebulator_game_probe`
The primary execution engine and automated testing tool:
```sh
# Launch a game module (automatically resolves companion .bar, .mif, and boot.pkg files)
./build/tools/zeebulator_game_probe /path/to/game.mod

# Launch with real audio dump and remote NDJSON control server on port 48900
ZEEB_DUMP_AUDIO=gameplay.wav ZEEB_CONTROL_PORT=48900 ./build/tools/zeebulator_game_probe /path/to/game.mod
```

### 2. Format Inspectors
Custom reverse-engineering inspection tools are included in `build/tools/`:
- **`zeebulator_mif_inspector`**: Inspects metadata and extracts applet ClassIDs from `.mif` files.
- **`zeebulator_sar_inspector`**: Lists and extracts assets from Superscape `.sar` archives.
- **`zeebulator_bar_inspector`**: Inspects image, sound, and binary resources inside BREW `.bar` containers.
- **`zeebulator_pakz_inspector`**: Validates and decompresses `.pakz` archive streams.
- **`zeebulator_obm1_inspector`**: Inspects 3D meshes and geometry in `.obm1` model files.

---

## Project Structure

```text
zeebulator/
├── core/
│   ├── arm/         # ARMv5TE/ARM11 interpreter, Thumb interworking, and CPU core
│   ├── audio/       # Stereo audio mixer, resampling, and SDL2 pipeline
│   ├── brew/        # BREW 4.0.2 HLE runtime (IShell, IFileMgr, IDisplay, SqlHle, etc.)
│   ├── gl/          # OpenGL ES 1.1 and QEGL desktop translation layer
│   ├── loader/      # Parsers for ELF, MOD, AEZ, FUFS (.vfs), SAR, MIF, and PAKZ
│   └── memory/      # Guest memory management, page caching, and fast-paths
├── frontends/
│   └── standalone/  # Interactive SDL2 frontend and presentation backend
├── tools/           # zeebulator_game_probe and container inspectors
├── tests/           # Automated GoogleTest test suite
├── docs/            # Technical specifications and compatibility census
└── third_party/     # SQLite 3.46.1 amalgamation, Zydis, Zycore, Dynarmic, and fmt
```

---

## License

This project is licensed under the terms of the **GNU General Public License v3.0 (GPLv3)**. See the [LICENSE](LICENSE) file for the complete text.

All trademarks, product names, and company names referenced herein (including Zeebo, Qualcomm, and BREW) are the property of their respective owners and are used strictly for historical preservation, technical research, and interoperability purposes.
