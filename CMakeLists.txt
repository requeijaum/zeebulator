cmake_minimum_required(VERSION 3.20)
project(zeebulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No build type was ever being set (single-config generators default to
# an unoptimized, non-NDEBUG build when CMAKE_BUILD_TYPE is empty --
# confirmed via this project's own build/core/CMakeFiles/*/flags.make,
# which had no -O flag at all). That's the real, measured bottleneck
# behind a software ARM interpreter running Double Dragon's title
# screen at ~12fps against its own ~60Hz internal tick budget (TASKS.md
# Phase 8, PHASE8_LOG.md "Real controller input" section's FPS
# overlay work): an unoptimized per-instruction dispatch loop is
# routinely 5-10x slower than an optimized one. Defaulted (not forced)
# to Release so `cmake --build . --config Debug`/`-DCMAKE_BUILD_TYPE=
# Debug` still works for anyone who explicitly wants it.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(ZEEBULATOR_BUILD_LIBRETRO "Build the libretro core" ON)
option(ZEEBULATOR_BUILD_STANDALONE "Build the standalone dev frontend" ON)
option(ZEEBULATOR_BUILD_TESTS "Build the test suite" ON)
option(ZEEBULATOR_BUILD_TOOLS "Build dev tools" ON)

# Declared at top level (not inside core/) so zlib_SOURCE_DIR/zlib_BINARY_DIR
# and the zlibstatic target are visible to sibling subdirectories too
# (e.g. tests/, which also needs zlib directly for building test fixtures).
include(FetchContent)
FetchContent_Declare(
  zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

if(ZEEBULATOR_BUILD_STANDALONE)
  FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
  )
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)

  # System-provided on all three target OSes (opengl32 ships with any
  # Windows toolchain, the OpenGL framework ships with Xcode/macOS, Linux
  # needs its distro's GL dev package -- e.g. libgl1-mesa-dev on
  # Debian/Ubuntu, the same category of build-time-only dependency as the
  # X11 dev headers SDL2 itself needed, see TASKS.md Phase 3).
  find_package(OpenGL REQUIRED)
endif()

# Real General MIDI wavetable synthesis (PHASE8_LOG.md's "Sound, round
# thirteen"): the real Zeebo hardware's own MIDI playback went through
# Qualcomm's real CMX synthesizer -- a real, GM1/GM2-compliant,
# wavetable/sample-based engine (confirmed directly in the real Zeebo
# SDK developer guide, not guessed) -- not the hand-rolled sine/square/
# sawtooth approximation this project started with. TinySoundFont (MIT
# license) is a small, single-header real SoundFont2 synthesizer;
# GeneralUser GS is a real, widely-used GM-compliant SoundFont whose
# own license explicitly permits bundling in other software projects
# ("feel free to use it in your software projects... modify the
# SoundFont bank or its packaging to suit your needs"). This can't
# reproduce Qualcomm's own proprietary wavetable samples (not publicly
# available), but both are real GM-standard engines using the same
# real program-number instrument assignments, so the *instrument
# choice* matches even though the exact *timbre* isn't byte-identical.
# Real Zeebo Sports Tênis/Peteca/Volei, Zeeboids, and one kids'-activity
# title's own shared `resources.pakz` asset container (see
# core/loader/pakz.h) stores each entry as a standard classic-".lzma"
# ("LZMA_ALONE") compressed stream -- confirmed directly against the real
# file (5-byte properties/dict header, 8-byte size, real decompressed
# content starting with a real, legible "...Crazyball Engine. Copyright"
# string). liblzma's own `lzma_alone_decoder` is the real, standard way to
# read that container -- runtime-installed on this dev machine but with
# no dev headers/pkg-config file, so vendored the same way zlib already
# is above rather than depending on a system install this project can't
# assume elsewhere.
FetchContent_Declare(
  liblzma
  GIT_REPOSITORY https://github.com/tukaani-project/xz.git
  GIT_TAG v5.6.3
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
# Without this, xz's own internal self-tests (test_stream_flags, test_vli,
# ...) register as CTest tests inside *this* project's own test run,
# noisy and unrelated to anything this project's own test suite checks.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(liblzma)

FetchContent_Declare(
  tinysoundfont
  GIT_REPOSITORY https://github.com/schellingb/TinySoundFont.git
  GIT_TAG 853a0a171759f1ddba0de1442133a75912bbeffa
)
FetchContent_MakeAvailable(tinysoundfont)

FetchContent_Declare(
  generaluser_gs_soundfont
  URL https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/main/GeneralUser-GS.sf2
  URL_HASH SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(generaluser_gs_soundfont)

# Scoped here (after every FetchContent_MakeAvailable above, before any
# of this project's own add_subdirectory calls below) so it only applies
# to this project's own targets, not vendored third-party ones (zlib,
# SDL2) -- CMake directory-scoped compile options only affect targets
# declared in the current directory and subdirectories added afterward.
# No -Werror: this project has no CI enforcing it yet (TASKS.md Phase
# 10), and an unverified warning on some other platform/compiler
# shouldn't be able to break the build here.
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(core)

if(ZEEBULATOR_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(ZEEBULATOR_BUILD_STANDALONE)
  add_subdirectory(frontends/standalone)
endif()

if(ZEEBULATOR_BUILD_LIBRETRO)
  add_subdirectory(frontends/libretro)
endif()

if(ZEEBULATOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
