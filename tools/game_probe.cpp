// Dev tool: drives a real BREW game's full lifecycle (AEEMod_Load ->
// IModule::CreateInstance -> HandleEvent(EVT_APP_START)) through every
// HLE interface implemented so far (IShell, IDisplay, IFile/IFileMgr,
// IGL/IEGL, IMedia), to see how far real execution actually gets and
// exactly which gap it hits next -- the "iteratively debug against the
// real game" approach documented in PHASE8_LOG.md. Real-file validation
// only: takes paths at runtime, never embeds/bundles game content (see
// CONTRIBUTING.md's clean-room policy).

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/audio/mixer.h"
#include "core/audio/soundfont_synth.h"
#include "core/brew/file_hle.h"
#include "core/brew/gl_hle.h"
#include "core/brew/idisplay.h"
#include "core/brew/interface_object.h"
#include "core/brew/ishell.h"
#include "core/brew/media_hle.h"
#include "core/brew/mod_runtime.h"
#include "core/brew/scaffold_object.h"
#include "core/brew/virtual_filesystem.h"
#include "core/cpu/arm_interpreter.h"
#include "core/gl_texture_log.h"
#include "core/loader/atitc.h"
#include "core/loader/png.h"
#include "core/loader/ggz.h"
#include "core/loader/mod.h"
#include "core/loader/pkg.h"
#include "core/save_state.h"
#include "frontends/standalone/sdl2_unified_backend.h"
#include "frontends/standalone/zpad_edges.h"

namespace {

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", path);
    std::exit(1);
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

// Real disassembly of Double Dragon (PHASE8_LOG.md) shows it calling
// IFILEMGR_OpenFile("sound.ggz") directly -- the game opens its own
// packed resource archive as a raw file and presumably streams/parses
// it itself (e.g. for on-demand audio), rather than expecting every
// entry pre-extracted. So the archive's own raw bytes need to be a
// VFS entry under its basename too, alongside its extracted contents.
std::string BaseName(const char* path) {
  std::string s(path);
  size_t slash = s.find_last_of("/\\");
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

// Alien Breaker Deluxe's real, custom in-app rendering engine (TASKS.md
// Phase 8) draws all its own text by hand -- geometry via its own
// per-character cell layout, glyphs via its own bitmap font atlas --
// entirely bypassing this project's standard IDisplay/DrawText path
// (confirmed live: zero real DrawText calls the whole run). The real
// font atlas itself is a real ATITC-compressed RGBA texture embedded
// in this title's own `data.bar`, real byte offset 3653369 (found by
// scanning the archive for real ATITC headers and cross-checking
// candidate images against this session's own live-decoded charset
// table -- see TASKS.md), decoded with this project's own existing,
// already-validated `DecodeAtitc` (no new decoder needed). 512x128.
// Used only as a fingerprint today (`abd_font_atlas.has_value()` below
// gates this whole real bridge to Alien Breaker Deluxe's own real
// `data.bar` specifically) -- the real per-glyph cell geometry this
// comment used to describe is no longer computed from this fixed
// layout at draw time; the real draw call itself (`abd.mod` 0x106508's
// own real 44-byte descriptor, see `AbdTextState::
// last_draw_descriptor_addr`) already carries its own real source crop
// rect directly, generically, for both real text glyphs and real icons
// alike, cross-validated exactly against real live samples (see the
// real textured-draw branch below) -- no separate real per-character
// arithmetic needed against this specific atlas's own fixed dimensions
// any more.
constexpr int kAbdFontAtlasWidth = 512;
constexpr int kAbdFontAtlasHeight = 128;
constexpr uint32_t kAbdFontAtlasBarOffset = 3653369;

// Real ASCII (0x20-0x7F) -> real curated glyph-atlas index, read
// directly from `abd.mod`'s own live table this session (all 96
// entries, not inferred) -- see TASKS.md Phase 8. `-1` marks a real
// ASCII byte this font's real table doesn't map (matches real index 0,
// space, at runtime -- kept distinct here only so a lookup miss is
// visible in this project's own code, not silently identical to a
// real space).
constexpr int16_t kAbdCharsetTable[96] = {
    0,  64, 0,  0,  47, 0,  0,  76, 73, 74, 75, 0,  70, 77, 69, 66,  // 0x20-0x2F
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 72, 71, 0,  0,  0,  65,  // 0x30-0x3F
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,  // 0x40-0x4F
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 0,  0,  0,  0,  0,   // 0x50-0x5F
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,  // 0x60-0x6F
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 0,  0,  0,  0,  0,   // 0x70-0x7F
};

// Decodes the real font atlas out of an already-loaded `data.bar`'s
// raw bytes into interleaved RGBA (row-major, 4 bytes/texel -- the
// same layout `IDisplayHle::BlitRgba` expects). Returns nullopt if
// `bar_bytes` is too short to contain the real, confirmed offset --
// callers should treat that as "this isn't Alien Breaker Deluxe's own
// `data.bar`", not a crash.
std::optional<std::vector<uint8_t>> DecodeAbdFontAtlas(const std::vector<uint8_t>& bar_bytes) {
  constexpr uint32_t kAtitcHeaderSize = 32;
  // RGBA ATITC: 16 bytes per 4x4 (16-texel) block = 1 byte/pixel
  // average -- confirmed exactly against the real archive entry's own
  // declared size (65568 - 32 header bytes = 65536 = 512*128).
  constexpr uint32_t kCompressedSize = kAbdFontAtlasWidth * kAbdFontAtlasHeight;
  if (bar_bytes.size() < kAbdFontAtlasBarOffset + kAtitcHeaderSize + kCompressedSize) return std::nullopt;
  const uint8_t* compressed = bar_bytes.data() + kAbdFontAtlasBarOffset + kAtitcHeaderSize;
  return zeebulator::DecodeAtitc(compressed, kCompressedSize, kAbdFontAtlasWidth, kAbdFontAtlasHeight,
                                  zeebulator::AtitcFormat::kRgba);
}

void MergeGgzInto(zeebulator::VirtualFilesystem& vfs, const char* path) {
  std::vector<uint8_t> raw = ReadFile(path);
  auto archive = zeebulator::GgzArchive::Parse(raw);
  for (const auto& entry : archive.Entries()) {
    vfs.AddFile(entry.name, archive.Extract(entry));
  }

  // Double Dragon's own real code, when it opens "sound.ggz" itself
  // (rather than going through this loop's per-entry extraction above),
  // reads each entry's declared `decompressed_size` as a literal raw
  // byte count straight from the file at `offset` -- no decompression
  // at that level (confirmed via direct disassembly of ddragonz.mod
  // 0x11bfd0/0x11c964, PHASE8_LOG.md) -- so it needs the *raw file* to
  // physically contain that many bytes, not just a valid gzip stream
  // that happens to decompress to that size. This repo's `sound.ggz`
  // (byte-identical across three independent public sources) is short
  // for its own last few entries -- e.g. entry 73 declares 1034 bytes
  // at offset 1927592, but the file ends 529 bytes early. A real,
  // independent Zeebo emulator (Infuse) plays Double Dragon successfully
  // against this same file, which only makes sense if it tolerates this
  // exact shortfall -- so this pads the raw copy exposed under the
  // archive's own basename (never the individually-extracted, correctly
  // decompressed entries above) with zero bytes out to the largest
  // offset+decompressed_size any entry declares. This does not
  // fabricate any real content (the genuinely missing tail of that one
  // background track stays silent/garbage padding, not guessed audio)
  // -- it only stops a short real file from producing a false EOF where
  // a real, correct player evidently doesn't hit one.
  uint32_t max_extent = static_cast<uint32_t>(raw.size());
  for (const auto& entry : archive.Entries()) {
    uint32_t extent = entry.offset + entry.decompressed_size;
    if (extent > max_extent) max_extent = extent;
  }
  if (max_extent > raw.size()) {
    std::printf("padding %s with %zu zero bytes (short by that much vs. its own header table)\n",
                path, static_cast<size_t>(max_extent) - raw.size());
    raw.resize(max_extent, 0);
  }

  vfs.AddFile(BaseName(path), std::move(raw));
  std::printf("loaded %zu entries from %s\n", archive.Entries().size(), path);
}

// Super BurgerTime's real code (PHASE8_LOG.md) searches six real,
// literal candidate paths -- ".\boot.pkg", ".\boot\boot.rom",
// "roms\boot.pkg", "roms\boot\boot.rom", "roms\neogeo\boot.pkg",
// "roms\neogeo\boot\boot.rom" -- for a small, shared bootstrap file this
// specific title's own download doesn't contain (unlike `data.ggz`/
// `sound.ggz`, this isn't part of any one game's own asset package: the
// real multi-game ROM manifest embedded in `supbtime.mod` spans several
// unrelated titles under one shared arcade-emulation core, and this file
// is exactly the kind of shared, system-level component that implies).
// Confirmed real and generic (not specific to whichever game's download
// it's sourced from) by independently locating a real `boot.pkg` inside
// a *different* title's own download (Karnov's Revenge) already present
// in this project's sanctioned local archive, parsing it with this
// project's own `PkgArchive` (built entirely from Super BurgerTime's own
// file, with zero changes needed to read this second, independent real
// sample), and confirming its one real entry (`boot.rom`, 8192 bytes)
// decodes to what looks like a genuine 68000-style exception vector
// table (most entries pointing at one shared default handler) -- boot/
// init code, not per-game content. This registers both the raw `.pkg`
// bytes (for whichever candidate real code opens first) and the
// extracted `boot.rom` bytes, under every real candidate path observed,
// so this dev tool doesn't need to guess which one real code actually
// settles on using.
void MergeBootPkgInto(zeebulator::VirtualFilesystem& vfs, const char* path) {
  std::vector<uint8_t> raw = ReadFile(path);
  auto archive = zeebulator::PkgArchive::Parse(raw);

  static const char* const kPkgPaths[] = {
      ".\\boot.pkg",
      "roms\\boot.pkg",
      "roms\\neogeo\\boot.pkg",
  };
  for (const char* p : kPkgPaths) vfs.AddFile(p, raw);

  static const char* const kRomPaths[] = {
      ".\\boot\\boot.rom",
      "roms\\boot\\boot.rom",
      "roms\\neogeo\\boot\\boot.rom",
  };
  for (const auto& entry : archive.Entries()) {
    if (entry.name != "boot.rom") continue;
    std::vector<uint8_t> rom = archive.Extract(entry);
    for (const char* p : kRomPaths) vfs.AddFile(p, rom);
  }
  constexpr size_t kTotalPaths =
      sizeof(kPkgPaths) / sizeof(kPkgPaths[0]) + sizeof(kRomPaths) / sizeof(kRomPaths[0]);
  std::printf("loaded boot.pkg (%zu entries) from %s, registered under %zu real candidate paths\n",
              archive.Entries().size(), path, kTotalPaths);
}

// Like HleRuntime::CallArmFunction, but bounded and loudly reports if
// execution ever fetches from outside the loaded module's own address
// range (and outside the HLE call-out trap range). Real game code CAN
// legitimately jump outside the module briefly (into an HLE call-out),
// but a fetch from anywhere else -- e.g. never-written/zero-filled
// memory -- is never real progress. Without this check, our interpreter
// silently decodes an all-zero word as a harmless "ANDEQ r0,r0,r0" and
// keeps going; concretely, this is exactly what happened probing Double
// Dragon's real AEEMod_Load (see PHASE8_LOG.md): a missing loader
// "static base" pointer caused an indirect call through a null function
// pointer to jump to address 0, and stepping through zeroed memory from
// there happened to walk (262,237 harmless no-op steps later) right back
// into the module's own base address, silently re-entering AEEMod_Load
// and eventually producing a coincidentally-truthy but meaningless
// "success" result -- not a crash, not an UnimplementedInstruction, just
// quietly wrong. This helper turns that into a loud, unmissable warning
// instead.
struct CallResult {
  uint32_t r0 = 0;
  bool wandered_outside_module = false;
  bool exceeded_step_budget = false;
};

struct AbdTextState {
  // Real shared six-call-site utility `abd.mod` 0x1053ec explicitly
  // (re)selects a real texture only when its own real `r6` argument is
  // non-zero (`abd.mod` 0x10543c: `cmp r6, #0; beq 0x10547c`) --
  // confirmed via disassembly and live tracing (TASKS.md Phase 8).
  // When it's zero, this project's own `bound_texture` can still be
  // real, stale-nonzero from an unrelated earlier real selection (the
  // real splash quad's own known bug: it inherits real TITLE's own
  // texture from a real, incidental real select inside TITLE's own
  // real loader). Tracking real `r6`'s own real zero/nonzero state
  // directly, per real call, is what lets the real draw site tell
  // "an explicit real texture was chosen this real call" apart from
  // "whatever's stale is still sitting in `bound_texture`" -- reset to
  // real false at this real function's own real entry (`pc==0x1053ec`)
  // and real-set true only if `pc==0x105444` (inside the real `r6 !=
  // 0` branch) executes before the real draw site reads it.
  bool splash_texture_selected_this_call = false;
  // Real anchor-alignment dispatch `abd.mod` 0x104db0 handles real mode
  // 18 (LOGO/LOGOSTAR both pass this) via a real, explicit centering
  // subtraction (`abd.mod` 0x104e34: `cmp fp, #18; subeq r5, r5, r7,
  // asr #1; subeq r6, r6, r8, asr #1`) -- confirmed via disassembly
  // this executes for *both* LOGO and LOGOSTAR (same real mode), unlike
  // real mode 9 (TITLE), which skips straight past this subtraction
  // entirely (`abd.mod` 0x104e48-0x104e4c: `cmp fp, #9; beq 0x104e80`,
  // no r5/r6 write). This project's own bridge already applies a real
  // bottom-up Y-flip for the real text/shape paths (`kHeight -
  // raw_y.../65536`, see their own doc comments) but never for real
  // caller `0x104f84` -- confirmed live this round that this flip is
  // *also* needed for real mode 18 specifically: LOGO's own real y
  // argument (240, screen-vertical-center) is self-symmetric under
  // this exact flip (`480 - 240 == 240`), which is exactly why its
  // position looked correct without one, while LOGOSTAR's own real y
  // argument (320) is not (`480 - 320 == 160`, nowhere near 320) --
  // confirmed against a real human's own real reference screenshot
  // showing the real star positioned near the *top* of the screen,
  // overlapping real LOGO's own text, not below it. Real mode 9
  // (TITLE) must stay unflipped (already confirmed correct, and its
  // own doc comment already records that applying this exact flip
  // there puts it fully off-screen) -- tracked per real call the same
  // way as `splash_texture_selected_this_call` above: real false at
  // this real function's own real entry (`pc==0x104db0`), real-set
  // true only if `pc==0x104e38` (the real mode-18 subtraction itself)
  // executes before the real draw site reads it.
  bool anchor_mode_18_this_call = false;
  // The real 44-byte descriptor pointer most recently seen entering
  // `abd.mod` 0x106508, so the real slot 107 draw it leads to can read
  // that real descriptor's own real source-crop-rect fields directly --
  // the real repacked stack struct slot 107 itself receives only ever
  // carries real destination-rect fields, not the real source sub-rect
  // within whichever real texture is currently bound. Despite the name
  // this project first gave this (menu icons were the first real use
  // found), `0x106508` turns out to be the *one* real, shared geometry-
  // pack helper both real icon draws *and* real per-character text-glyph
  // draws funnel through alike (confirmed live: the real per-character
  // index this project used to track separately, captured at `abd.mod`
  // 0x105dac, is itself just a byte offset into this exact same real
  // 44-byte-descriptor array -- `char_index * 44`, real word math
  // `*3 + *8` then `<<2` -- so a real text glyph is not a structurally
  // different real draw from a real icon at all, just a different real
  // descriptor-array instance). One real, generic pointer suffices for
  // both; no separate per-character tracking needed.
  uint32_t last_draw_descriptor_addr = 0;
};

CallResult CallArmFunctionChecked(zeebulator::ArmInterpreter& cpu, uint32_t trap_base,
                                   uint32_t mod_base, uint32_t mod_size, uint32_t entry,
                                   uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                                   bool trace = false, bool hle_trace = false,
                                   zeebulator::IDisplayHle* display_for_liveness = nullptr,
                                   zeebulator::Sdl2UnifiedBackend* backend_for_liveness = nullptr,
                                   AbdTextState* abd_text_state = nullptr) {
  constexpr uint64_t kMaxSteps = 5'000'000;
  cpu.SetRegister(zeebulator::kR0, r0);
  cpu.SetRegister(zeebulator::kR1, r1);
  cpu.SetRegister(zeebulator::kR2, r2);
  cpu.SetRegister(zeebulator::kR3, r3);
  cpu.SetRegister(zeebulator::kLR, trap_base);
  cpu.SetRegister(zeebulator::kPC, entry);

  CallResult result;
  uint32_t last_in_module_pc = 0;
  uint32_t last_lr = 0;
  // See IDisplayHle::RepresentLastFrame's own doc comment: a single real
  // ARM call below can run for millions of interpreted instructions
  // without ever returning control to the outer tick loop that normally
  // keeps the window's last real frame visible -- checked cheaply (a
  // counter, not a clock read, on every single step) so a real call that
  // legitimately runs long doesn't let a whole real second pass with the
  // window never re-presented.
  uint64_t steps_since_liveness_check = 0;
  auto last_liveness_present = std::chrono::steady_clock::now();
  for (uint64_t steps = 0; cpu.GetRegister(zeebulator::kPC) != trap_base; ++steps) {
    if (steps >= kMaxSteps) {
      std::printf("warning: exceeded %llu steps without returning -- aborting this call\n",
                  static_cast<unsigned long long>(kMaxSteps));
      result.exceeded_step_budget = true;
      break;
    }
    if (display_for_liveness != nullptr &&
        (backend_for_liveness == nullptr || !backend_for_liveness->HasRealGlActivity()) &&
        ++steps_since_liveness_check >= 20000) {
      steps_since_liveness_check = 0;
      auto now = std::chrono::steady_clock::now();
      if (now - last_liveness_present > std::chrono::milliseconds(200)) {
        last_liveness_present = now;
        display_for_liveness->RepresentLastFrame();
      }
    }
    uint32_t pc = cpu.GetRegister(zeebulator::kPC);
    if (abd_text_state != nullptr && pc == 0x001053ec) {
      abd_text_state->splash_texture_selected_this_call = false;
    }
    if (abd_text_state != nullptr && pc == 0x00105444) {
      abd_text_state->splash_texture_selected_this_call = true;
    }
    if (abd_text_state != nullptr && pc == 0x00104db0) {
      abd_text_state->anchor_mode_18_this_call = false;
    }
    if (abd_text_state != nullptr && pc == 0x00104e38) {
      abd_text_state->anchor_mode_18_this_call = true;
    }
    if (abd_text_state != nullptr && pc == 0x00106508) {
      abd_text_state->last_draw_descriptor_addr = cpu.GetRegister(zeebulator::kR0);
    }
    bool in_module = pc >= mod_base && pc < mod_base + mod_size;
    bool in_trap_range = pc >= trap_base;
    if (in_module) {
      last_in_module_pc = pc;
      last_lr = cpu.GetRegister(zeebulator::kLR);
    }
    if (trace) {
      std::printf("[%4llu] pc=0x%08x instr=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x\n",
                  static_cast<unsigned long long>(steps), pc, cpu.GetMemory().Read32(pc),
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3),
                  cpu.GetRegister(zeebulator::kR4));
    }
    if (hle_trace && in_trap_range && pc != trap_base) {
      std::printf("  [hle call] trap=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", pc,
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3));
    }
    if (!in_module && !in_trap_range && !result.wandered_outside_module) {
      std::printf(
          "warning: pc=0x%08x left the loaded module's range (0x%08x-0x%08x) after %llu "
          "steps -- likely a missing loader/runtime-support gap, not real progress (see "
          "PHASE8_LOG.md). Last in-module pc=0x%08x lr=0x%08x -- disassemble there first.\n",
          pc, mod_base, mod_base + mod_size, static_cast<unsigned long long>(steps),
          last_in_module_pc, last_lr);
      result.wandered_outside_module = true;  // only warn once per call
    }
    cpu.Step();
  }
  result.r0 = cpu.GetRegister(zeebulator::kR0);
  return result;
}

// Maps a subset of SDL keys to real BREW AVK-family key codes for
// exploratory input testing. The exact AVK_* enum values aren't
// confirmed against a real header this session -- what IS confirmed via
// real disassembly (PHASE8_LOG.md) is that Double Dragon's own
// HandleEvent treats wParam values in [0xe021, 0xe021+22] as key codes,
// converting them to a bitmask via a jump table. This maps number keys
// 0-9 to that range's first 10 offsets (0xe021..0xe02a) and arrow keys
// to the next four (0xe02b..0xe02e), purely so real keypresses can be
// tried against the running game and their effect (if any) observed --
// not a claimed-correct real key mapping.
uint32_t SdlKeyToAvk(SDL_Keycode key) {
  constexpr uint32_t kAvkBase = 0xe021;
  if (key >= SDLK_0 && key <= SDLK_9) {
    return kAvkBase + static_cast<uint32_t>(key - SDLK_0);
  }
  switch (key) {
    case SDLK_UP: return kAvkBase + 10;
    case SDLK_DOWN: return kAvkBase + 11;
    case SDLK_LEFT: return kAvkBase + 12;
    case SDLK_RIGHT: return kAvkBase + 13;
    default: return 0;
  }
}

// Maps a subset of SDL keys to real HID `nButtonUID` values, for the
// *other* real input path this codebase has wired up but never fed
// live input into: the real HID/gamepad button-event mechanism
// (`hid_device_methods[9]`/`captured_button_callback` below), separate
// from the classic AVK key path `SdlKeyToAvk` feeds.
//
// Real disassembly this round traced the whole real pipeline live, end
// to end, not guessed: the registered callback (`ddragonz.mod`
// `0x11bdf4`) calls a real translation function (`0x100740`) that
// subtracts a real base UID from `nButtonUID` and jump-tables the
// result (0-15) into a real, small `nButtonID`, unrecognized codes
// (outside the game's own real 10-button subset) making the whole
// event get silently dropped. That callback then writes a real per-
// gamepad held/pressed/released bitmask, a real per-tick function
// (`0x123740`) latches it, and a real combine function (`0x11a2ec`)
// ORs both real gamepad slots together into
// `applet+0x3618/0x361c/0x3620` -- all confirmed via live watch/write-
// watch, including a full injected keypress observed propagating
// through every single stage up to the real, already-documented
// title-screen progression gate (`tst [applet+0x361c], #0x100`).
//
// The real UID *values* being subtracted from aren't guessed either --
// they're this project's own bundled, genuine Qualcomm SDK header
// (`research/docs/sdk_installer_extract/sdk_installer_cab/
// _23C2FF7AB01B49768D1DB61FA4834C66`, `AEEHIDDevice_Joystick.h`),
// giving every one of these a real name, not just a real number. Of
// the header's 16 real joystick UIDs, Double Dragon's own real table
// recognizes exactly 10: the full real D-pad, `Back` (confirmed live,
// see above -- this is the real title-progression button), both real
// *upper* shoulder buttons, and all four real face buttons
// (`Button_1`-`Button_4`). `Start` and both real thumbstick-click UIDs
// are real, valid, and simply not in Double Dragon's own recognized
// subset -- not a project gap.
constexpr uint32_t kHidUidDPadUp = 0x0106c3fe;
constexpr uint32_t kHidUidDPadLeft = 0x0106c3ff;
constexpr uint32_t kHidUidDPadDown = 0x0106c400;
constexpr uint32_t kHidUidDPadRight = 0x0106c401;
constexpr uint32_t kHidUidBack = 0x0106c403;  // confirmed real: the title-progression button
constexpr uint32_t kHidUidLeftShoulderUpper = 0x0106c406;
constexpr uint32_t kHidUidRightShoulderUpper = 0x0106c408;
constexpr uint32_t kHidUidButton1 = 0x0106c40a;
constexpr uint32_t kHidUidButton2 = 0x0106c40b;
constexpr uint32_t kHidUidButton3 = 0x0106c40c;
constexpr uint32_t kHidUidButton4 = 0x0106c40d;

uint32_t SdlKeyToHidButton(SDL_Keycode key) {
  switch (key) {
    case SDLK_UP: return kHidUidDPadUp;
    case SDLK_DOWN: return kHidUidDPadDown;
    case SDLK_LEFT: return kHidUidDPadLeft;
    case SDLK_RIGHT: return kHidUidDPadRight;
    case SDLK_BACKSPACE: case SDLK_RETURN: return kHidUidBack;
    case SDLK_q: return kHidUidLeftShoulderUpper;
    case SDLK_e: return kHidUidRightShoulderUpper;
    case SDLK_z: return kHidUidButton1;
    case SDLK_x: return kHidUidButton2;
    case SDLK_c: return kHidUidButton3;
    case SDLK_v: return kHidUidButton4;
    default: return 0;  // 0 is not a real UID any real device would ever send
  }
}

// A real ZPadState button -> real HID button UID mapping, mirroring
// SdlKeyToHidButton's own mapping one-for-one (arrows -> D-pad,
// Start/Home -> the confirmed real title-progression button, shoulders
// -> the two real upper shoulder UIDs, the four face buttons -> Button_1
// through Button_4 in ZPadState's own West/South/North/East order,
// matching keyboard's Z/X/C/V order) so a real gamepad feeds the exact
// same downstream injection as keyboard does, not a diverging mapping.
// ZPadState's Select/thumbstick-click bits have no real ZPadState bit at
// all (see core/backend.h) so there's nothing to map for them here.
uint32_t ZPadButtonToHidUid(uint16_t button) {
  using zeebulator::ZPadState;
  switch (button) {
    case ZPadState::kDpadUp: return kHidUidDPadUp;
    case ZPadState::kDpadDown: return kHidUidDPadDown;
    case ZPadState::kDpadLeft: return kHidUidDPadLeft;
    case ZPadState::kDpadRight: return kHidUidDPadRight;
    case ZPadState::kStartHome: return kHidUidBack;
    case ZPadState::kShoulderL: return kHidUidLeftShoulderUpper;
    case ZPadState::kShoulderR: return kHidUidRightShoulderUpper;
    case ZPadState::kButtonWest: return kHidUidButton1;
    case ZPadState::kButtonSouth: return kHidUidButton2;
    case ZPadState::kButtonNorth: return kHidUidButton3;
    case ZPadState::kButtonEast: return kHidUidButton4;
    default: return 0;
  }
}

// EXPERIMENTAL: a fake connected-joystick handle, reported by the HID
// scaffold below instead of the honest "zero devices" answer this
// project used through TASKS.md Phase 8's Double Dragon investigation.
// Found (real disassembly, see PHASE8_LOG.md) that Double Dragon's
// title screen genuinely, correctly waits for real HID/gamepad input
// before proceeding -- not an emulator bug, a real hardware dependency
// this dev tool has no real controller to satisfy. Any nonzero, stable
// value works as the "handle" -- real code only ever uses it as an
// opaque token passed back into IHID_CreateDevice/GetDeviceInfo, never
// interprets it directly.
constexpr uint32_t kSimulatedDeviceHandle = 1;

}  // namespace

int main(int argc, char** argv) {
  // glibc fully block-buffers stdout by default whenever it isn't a
  // TTY (i.e. whenever it's redirected to a file/pipe, as this tool's
  // whole live-debugging workflow depends on) -- sparse output (a
  // single key event's worth of printf calls) can then sit unflushed
  // in memory indefinitely while the process keeps running, making a
  // live `tail`/`grep` on the redirected log look like nothing
  // happened even though it did. Line-buffered instead, so every
  // printed line reaches the file the moment it's printed.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // A bare `--load-state` flag (any position) auto-loads the fixed-slot
  // save (see save_state_path below) right after setup, before the
  // event loop starts -- lets a non-interactive launch (e.g. this
  // project's own dev workflow of relaunching the tool to inspect a
  // player-reported bug) land exactly on a previously-F1-saved point
  // without needing a live F2 keypress. Stripped out here, before any
  // of the positional-argument parsing below, so it can appear anywhere
  // on the command line without shifting the fixed positional slots.
  bool auto_load_state = false;
  // `--persistent-log` (same appear-anywhere/strip-before-positional-
  // parsing convention as `--load-state`) redirects stdout/stderr to a
  // durable, append-mode `<rom>.playlog` file (opened below, once
  // argv[1] is known) instead of wherever the launcher happened to
  // point them -- added specifically because this project's own
  // player-facing worktree launches game_probe detached from any
  // terminal (stdout ends up on /dev/null, confirmed via
  // `/proc/<pid>/fd` on a real frozen instance), so a real, live-
  // reported bug (a "LOAD ERROR" screen the game's own code drew,
  // found from a screenshot, not this tool's own output) left zero
  // record behind to investigate afterward. Deliberately opt-in, not
  // the default for every invocation: this project's own dev-testing
  // workflow already redirects `game_probe`'s stdout via the shell
  // (`> file 2>&1`) throughout TASKS.md's own history, and an
  // unconditional internal redirect would silently steal that output
  // out from under it.
  bool persistent_log = false;
  {
    int write_i = 1;
    for (int read_i = 1; read_i < argc; ++read_i) {
      if (std::string(argv[read_i]) == "--load-state") {
        auto_load_state = true;
        continue;
      }
      if (std::string(argv[read_i]) == "--persistent-log") {
        persistent_log = true;
        continue;
      }
      argv[write_i++] = argv[read_i];
    }
    argc = write_i;
  }
  if (argc < 5) {
    // cls_id is IModule::CreateInstance's real AEECLSID -- the literal
    // the module's own code compares the passed ClsId against (found by
    // tracing the first few real instructions of CreateInstance with
    // trace=true; it's loaded via a PC-relative `ldr` right before the
    // `cmp` that decides success/failure). NOT necessarily the game's
    // download-catalog folder number: confirmed identical to it for
    // Super BurgerTime (279125), but genuinely different for Double
    // Dragon (274754 vs the real 0x0102f789) and Peggle (278962 vs the
    // real 0x01099cd6) -- passing the folder number for those two makes
    // CreateInstance return EFAILED immediately, with zero HLE calls,
    // before anything resembling real progress happens. See
    // PHASE8_LOG.md for how this was found.
    std::fprintf(stderr,
                  "usage: %s <game.mod> <data.ggz> <sound.ggz> <cls_id_decimal> [boot.pkg] "
                  "[resources.bar] [--load-state] [--persistent-log]\n",
                  argv[0]);
    return 1;
  }
  if (persistent_log) {
    // Append mode: successive real play sessions accumulate in the
    // same file rather than overwriting each other, since the whole
    // point is having a record of whichever session hit the bug --
    // not knowable in advance which one that'll be. A header line per
    // session (wall-clock time, PID, full argv) makes it possible to
    // find where a given session starts/ends when reading the file
    // back later, without needing to parse timestamps out of every
    // line.
    std::string playlog_path = std::string(argv[1]) + ".playlog";
    if (std::freopen(playlog_path.c_str(), "a", stdout) == nullptr ||
        std::freopen(playlog_path.c_str(), "a", stderr) == nullptr) {
      std::exit(1);
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::time_t now = std::time(nullptr);
    std::printf("\n===== session start %s argv:", std::ctime(&now));
    for (int i = 0; i < argc; ++i) std::printf(" %s", argv[i]);
    std::printf(" =====\n");
  }
  auto mod_data = ReadFile(argv[1]);
  auto cls_id = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10));
  // TASKS_TOOLING.md Phase B, stage 1 -- single fixed slot, next to the
  // ROM itself, not the git-ignored research/games/ tree's own concern
  // (this file is real tooling output, not research material, but
  // colocating it is the simplest place a player would look for it).
  const std::string save_state_path = std::string(argv[1]) + ".savestate";
  // Real save-game data (Double Dragon's own "./udata/ddz.sav", written
  // through FileHle's writable_files_ -- see file_hle.h) is a genuinely
  // separate concern from the save STATE above: a player's actual
  // in-game progress/unlocks, meant to survive every cold relaunch
  // unconditionally, not just resumed from a chosen moment. Without
  // this, writable_files_ was purely in-memory and silently reset to
  // empty on every process exit -- indistinguishable from the game "not
  // saving" at all (a real, live-reported bug this fixes).
  const std::string userdata_path = std::string(argv[1]) + ".userdata";

  zeebulator::VirtualFilesystem vfs;
  MergeGgzInto(vfs, argv[2]);
  MergeGgzInto(vfs, argv[3]);
  if (argc >= 6) MergeBootPkgInto(vfs, argv[5]);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  constexpr int kWidth = 640;
  constexpr int kHeight = 480;
  constexpr int kAudioSampleRate = 22050;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  // Real Double Dragon enables GL_DEPTH_TEST once at startup and never
  // touches it again (confirmed live -- PHASE8_LOG.md), i.e. it relies
  // on a real depth buffer for real sprite/HUD layering. Never
  // requested one here before, so depending on the driver's default
  // this could silently negotiate zero depth bits, making that real
  // GL_DEPTH_TEST a no-op and leaving every draw ordered by submission
  // order alone -- exactly the real, confirmed symptom (enemies and a
  // health-bar fill drawn in the wrong front/back order).
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  // Resizable so a manual drag-resize works too, not just the F5-F8
  // scale-preset hotkeys below -- Sdl2UnifiedBackend letterboxes
  // whatever real size the window ends up at either way (see its own
  // PresentFrame doc comment), so nothing else needs to change for this.
  SDL_Window* window = SDL_CreateWindow(
      "Zeebulator - game probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
      SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  zeebulator::ArmInterpreter cpu;
  constexpr uint32_t kTrapBase = 0xF0000000;
  zeebulator::HleRuntime hle(cpu, kTrapBase, 0x10000);
  // See Sdl2UnifiedBackend's own doc comment: a real host GL context
  // anywhere in this process, coexisting with a *separate* 2D
  // presentation path, reliably breaks this desktop's real compositor
  // into no longer repainting the real visible window -- confirmed via
  // a minimal, independent reproduction that a *single* real GL context
  // used as the sole presentation mechanism does not have this problem.
  // One real object implements both the 2D IDisplay surface and real
  // IGL/IEGL rendering, on one real window/context, instead of two
  // separate backends.
  zeebulator::Sdl2UnifiedBackend backend(window, kWidth, kHeight, kAudioSampleRate);
  zeebulator::IDisplayHle display(backend, kWidth, kHeight);
  // Real texture uploads (GenTextures/TexImage2D/...) pass through this
  // recorder transparently -- see TASKS_TOOLING.md Phase B, stage 2 --
  // so a save state can later replay them against a fresh GL context
  // and end up with the same real texture IDs/contents a loaded state's
  // guest memory expects, which a cold, non-interactive boot never
  // recreates on its own the way real gameplay would.
  zeebulator::GlTextureRecordingBackend gl_recorder(backend);
  zeebulator::GlHle gl_hle(gl_recorder);
  zeebulator::Mixer mixer(kAudioSampleRate);
  zeebulator::FileHle file_hle(cpu.GetMemory(), hle, vfs, /*object_region=*/0x80100000);
  {
    std::ifstream userdata_in(userdata_path, std::ios::binary);
    if (userdata_in && file_hle.Deserialize(userdata_in)) {
      std::printf("loaded real save-game data from %s\n", userdata_path.c_str());
    }
    // Anything else (no file yet, or a stream that failed to parse)
    // just leaves writable_files_ empty -- the same real "no save yet"
    // state a genuinely first-ever launch has, not an error.
  }
  // Real General MIDI wavetable synthesis (see CMakeLists.txt's own doc
  // comment) instead of MediaHle's hand-rolled fallback -- real, not
  // guessed: loading a real ~32MB soundfont once here, not per-clip.
  zeebulator::SoundFontSynth soundfont_synth;
  if (!soundfont_synth.IsLoaded()) {
    std::fprintf(stderr, "warning: real soundfont failed to load -- falling back to the "
                          "hand-rolled MIDI synth\n");
  }
  zeebulator::MediaHle media_hle(cpu.GetMemory(), hle, vfs, mixer, /*object_region=*/0x80200000,
                                  &soundfont_synth);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);
  auto mod_size = static_cast<uint32_t>(mod_data.size());

  // Real compiled .mod code (ARM RVCT ROPI convention) expects a
  // "static base" pointer at kBase-4 -- see core/brew/mod_runtime.h and
  // PHASE8_LOG.md for how this was found via real disassembly.
  // heap_size was originally 1 MiB, sized arbitrarily rather than
  // measured -- real disassembly of Double Dragon's own resource-list
  // loader (PHASE8_LOG.md) shows it MALLOC-ing real, sizeable audio
  // buffers (tens to hundreds of KB each) for many real resources in a
  // row, genuinely exhausting 1 MiB partway through and returning a
  // real null from MALLOC that real game code can't recover from --
  // not a bug in MallocImpl itself (confirmed via a live debug trace),
  // just too small a heap for this real game's real needs. Bumped to
  // 16 MiB, a generous but not unreasonable amount of app heap for a
  // 2009-era dedicated gaming device.
  // Bumped again to 64 MiB (TASKS.md Phase 8, the Zeebo Sports Tênis/
  // Zeeboids investigation): a live allocation trace caught the real
  // root cause behind BOTH titles' remaining walls at once -- a single
  // real MALLOC(size=23068672) call (~22 MiB, matching a real static
  // constant, `0x01600000`, embedded in both titles' own shared
  // TTDMemoryManager.cpp-derived engine code) that 16 MiB could never
  // satisfy. Not a missing system call or a scratch-address collision
  // as earlier rounds of that investigation suspected -- a real,
  // measured heap need this harness's own fixed size hadn't grown to
  // cover, the same *shape* of gap the original 1 MiB->16 MiB bump
  // above already was. Nothing else in this file claims any address at
  // or above `0x80300000`, so extending the heap upward is safe.
  zeebulator::ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                                      /*heap_size=*/0x04000000, /*context_address=*/0x80280200);
  mod_runtime.Install(kBase, /*table_address=*/0x80280000);

  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);
  // Real compiled app code obtains IDisplay through
  // ISHELL_CreateInstance(AEECLSID_DISPLAY, ...), not directly -- found
  // via real disassembly of AEEApplet_New's call chain (PHASE8_LOG.md).
  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle, kWidth, kHeight);
  // Real ISHELL_LoadResDataEx(shell, "resources.bar", id, type, ...)
  // calls (real slot 41, confirmed live against Peggle -- see
  // core/brew/ishell.h) need the real file's own bytes registered
  // under its own real name to serve anything beyond a blind stub.
  // Alien Breaker Deluxe's own real font atlas (TASKS.md Phase 8),
  // decoded once here (before `bar_bytes` moves into
  // `RegisterResourceFile` below) if this run's own `resources.bar`
  // turns out to be that title's real `data.bar` -- nullopt otherwise
  // (any other title, or a `data.bar` too short to hold the real
  // confirmed offset), in which case the text-cell bridge below stays
  // a no-op rather than drawing anything wrong.
  std::optional<std::vector<uint8_t>> abd_font_atlas;
  AbdTextState abd_text_state;
  if (argc >= 7) {
    std::vector<uint8_t> bar_bytes = ReadFile(argv[6]);
    abd_font_atlas = DecodeAbdFontAtlas(bar_bytes);
    // Also expose the same raw bytes as a plain, directly-openable VFS
    // file under its own basename -- the same real "the archive's own
    // raw bytes need to be a VFS entry too" shape MergeGgzInto's own
    // doc comment already documents for `sound.ggz` (Double Dragon
    // opens it directly, bypassing the ID-based lookup, to stream
    // entries its own resource-ID directory doesn't cover). Found
    // live bringing up Alien Breaker Deluxe: its own real `data.bar`
    // resource-ID directory has exactly one entry, yet real code
    // requests dozens of sequential IDs via `LoadResDataEx` that
    // directory can never resolve -- real code almost certainly falls
    // back to opening the file directly for those, the same real
    // pattern already confirmed for `sound.ggz`.
    vfs.AddFile(BaseName(argv[6]), bar_bytes);
    shell_hle.RegisterResourceFile(BaseName(argv[6]), std::move(bar_bytes));
  }
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_obj);
  // ClsId 0x01002001: a real BREW class Double Dragon's own graphics-init
  // routine requires (ISHELL_CreateInstance failing for it is the
  // confirmed root cause of the "memory insufficient" dead end -- see
  // PHASE8_LOG.md). Its real interface isn't identified yet, so this
  // is a generic scaffold (see scaffold_object.h) sized to cover the
  // highest slot (33) real disassembly shows the game calling on it.
  uint32_t unknown_graphics_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8000C000, /*object=*/0x8000D000, /*slot_count=*/40);
  shell_hle.RegisterInstance(/*unidentified, real ClsId from disassembly=*/0x01002001,
                              unknown_graphics_obj);
  uint32_t shell = shell_hle.Build(/*vtable=*/0x80000000, /*object=*/0x80001000);
  // Same real init routine also calls IDisplay::GetDeviceBitmap and
  // immediately dereferences the result's vtable -- another generic
  // scaffold, since the real IBitmap-shaped interface isn't identified
  // either. Real disassembly of a second, deeper call site (0x1d5b8)
  // shows the returned bitmap's slot 2 gets called in a
  // "QueryInterface"-shaped way (obj, clsid=0x01001045, &ppo) and the
  // result immediately Release()'d if null -- so unlike the other
  // scaffolds so far, this one slot needs a real (if still generic)
  // implementation, not a blind Stub. clsid=0x01001045 is very likely
  // real AEECLSID_DIB: a real bundled BREW OGLES sample
  // (simple_drawtexture.c, under research/docs/sdk_installer_extract/
  // ZeeboSDKPackage-1.2.4/samples.zip) does exactly this same call --
  // `IBITMAP_QueryInterface(pIBitmapDDB, AEECLSID_DIB, (void**)&pDIB)`
  // right after `IDISPLAY_GetDeviceBitmap` -- then casts the result
  // straight to `NativeWindowType` for `eglCreateWindowSurface`, which
  // is exactly how the scaffold below gets used one call site over.
  // The numeric ClsId itself isn't in any bundled header (only the
  // matching call shape + matching downstream use), so this is strong
  // circumstantial evidence, not a confirmed literal match like
  // AEECLSID_GL/EGL/HID above -- doesn't change any behavior either way
  // since the scaffold is generic regardless of the class's real name.
  uint32_t unknown_0x01001045_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80018000, /*object=*/0x80019000, /*slot_count=*/20);
  uint32_t device_bitmap_obj = zeebulator::BuildStubObjectWithOverride(
      cpu.GetMemory(), hle, /*vtable=*/0x8000E000, /*object=*/0x8000F000, /*slot_count=*/20,
      /*override_slot=*/2,
      [&cpu, unknown_0x01001045_obj](zeebulator::IArmCore& core) {
        uint32_t requested_cls = core.GetRegister(zeebulator::kR1);
        uint32_t ppo = core.GetRegister(zeebulator::kR2);
        if (requested_cls == 0x01001045) {
          cpu.GetMemory().Write32(ppo, unknown_0x01001045_obj);
          core.SetRegister(zeebulator::kR0, 0);
        } else {
          core.SetRegister(zeebulator::kR0, 1);
        }
      });
  display.SetDeviceBitmapInstance(device_bitmap_obj);
  // ClsId 0x01001003: real disassembly of 0x1b2fc showed
  // ISHELL_CreateInstance gating the same "memory insufficient" state on
  // this class alongside 0x01001014 below -- initially scaffolded
  // generically since neither is dereferenced within 0x1b2fc itself.
  // Deeper disassembly (0x1c6b0 -> 0x22384 -> 0x237c4 -> 0x9f3c, TASKS.md
  // Phase 8) later showed this object IS used, extensively, by the
  // applet's own save-game load/create routine: IFILEMGR_Test on
  // "./udata/ddz.sav", IFILEMGR_GetFreeSpace checked against a minimum,
  // then IFILEMGR_OpenFile with mode literal 2 -- which is exactly real
  // AEEFile.h's _OFM_READWRITE (also confirmed a literal 4 elsewhere in
  // the same routine, matching _OFM_CREATE). That's IFileMgr's real
  // vtable shape exactly (slot 2 OpenFile, slot 7 Test, slot 8
  // GetFreeSpace) -- so this is very likely real AEECLSID_FILEMGR
  // (not confirmed by a literal number match, unlike AEECLSID_GL/EGL/HID,
  // since no bundled header states FILEMGR's numeric ClsId -- but the
  // vtable shape and the exact "test/create-if-missing/open" flow
  // matching real AEEFile.h leave little doubt). This project's own
  // FileHle already implements real IFileMgr/IFile (built in an earlier
  // phase for GGZ-backed read-only content, extended this round with a
  // real writable "user data" store -- see file_hle.h -- specifically so
  // this save-file flow can genuinely succeed instead of merely not
  // crashing) -- wired in below instead of a generic scaffold.
  uint32_t file_mgr_obj = file_hle.Build(/*file_mgr_vtable=*/0x80004000,
                                          /*file_mgr_object=*/0x80005000,
                                          /*file_vtable=*/0x80006000);
  shell_hle.RegisterInstance(0x01001003, file_mgr_obj);
  // ClsId 0x01001014: created alongside 0x01001003 above and never
  // reassigned anywhere in the traced code (confirmed with a live
  // memory watchpoint across a full run -- see PHASE8_LOG.md). Real
  // disassembly shows its slot 3 (Read) called immediately after the
  // game's own resource-loading routine opens and seeks the real file
  // it wants via a *different* object (IFileMgr), with no attach/bind
  // step ever observed -- so a real generic scaffold's blind Stub
  // silently "succeeds" with 0 bytes read every time. Wired to
  // FileHle's last-opened-file proxy instead: an evidence-grounded
  // educated implementation of the one behavior everything points at
  // (see file_hle.h's own doc comment on BuildLastOpenedFileProxy for
  // the full reasoning and what's still unconfirmed about it).
  uint32_t last_opened_file_proxy =
      file_hle.BuildLastOpenedFileProxy(/*vtable=*/0x80012000, /*object=*/0x80013000);
  shell_hle.RegisterInstance(0x01001014, last_opened_file_proxy);
  // ClsId 0x0100100c: a real, still-unidentified class found bringing
  // up Disney All Star Cards -- real code calls
  // `ISHELL_CreateInstance(shell, 0x0100100c, &ppo)` and, like every
  // other real `CreateInstance` call site in this project's history
  // that turned out to matter, never checks the returned status before
  // dereferencing `*ppo` two instructions later (confirmed live: the
  // out-param stayed null and the next real instruction crashed
  // through it). Generic scaffold, same as every other still-
  // unidentified class -- real interface not known yet.
  uint32_t unknown_0x0100100c_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80048000, /*object=*/0x80049000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x0100100c, unknown_0x0100100c_obj);
  // A still-deeper gate (0x1d5b8, reached only after the fixes above)
  // requires two more classes -- confirmed via real objdump directly on
  // the literal pool addresses its own `ldr r1,[pc,#N]` instructions
  // reference (0x1d970/0x1d974), not assumed from the nearby-looking
  // 0x0100100x classes above (a first attempt reused those by mistake
  // and was caught because the resulting crash's real disassembly showed
  // different literal values at those addresses). These turned out to be
  // real, already-implemented classes: extracted the real BREW OpenGL ES
  // extension SDK (`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
  // OpenGLES_Extension_...zip`, an MSI -- unpacked its embedded cabinet
  // with `7z`/`cabextract`) and found its real `AEEGL.h`: `#define
  // AEECLSID_GL 0x01014bc3` / `#define AEECLSID_EGL 0x01014bc4`, exactly
  // matching. `GlHle` (built in an earlier phase, previously wired up
  // directly without going through `CreateInstance`) already implements
  // both real interfaces, so those replace the generic scaffolds here.
  uint32_t gl_obj = gl_hle.BuildGl(cpu.GetMemory(), hle, /*vtable=*/0x80007000, /*object=*/0x80008000);
  shell_hle.RegisterInstance(/*AEECLSID_GL=*/0x01014bc3, gl_obj);
  uint32_t egl_obj = gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80009000, /*object=*/0x8000A000);
  shell_hle.RegisterInstance(/*AEECLSID_EGL=*/0x01014bc4, egl_obj);
  // A still-deeper gate (0x1b71c, a joystick/gamepad-init routine gating
  // the same "memory insufficient" state) calls
  // ISHELL_CreateInstance(shell, ClsId=0x0106c411, ...) then
  // IHID_GetConnectedDevices(pIHID, AEEUID_HID_Joystick_Device, ...) --
  // both literals confirmed for real: 0x0106c411 = AEECLSID_HID and
  // 0x0106c3fd = AEEUID_HID_Joystick_Device both appear, named exactly,
  // in the real BREW SDK sample source bundled in this repo
  // (research/samples/conftest_source/conftest/GamepadMgr.c and the
  // extracted AEEIHID.h under research/docs/sdk_installer_extract/
  // sdk_installer_cab/), which also confirms GetConnectedDevices is real
  // vtable slot 7 (INHERIT_IQI's 3 slots + CreateDevice/GetDeviceInfo/
  // GetNextConnectEvent/RegisterForConnectEvents/GetConnectedDevices).
  // We have no real joystick hardware to enumerate, so honestly
  // reporting zero connected devices was the original, correct answer
  // here.
  //
  // EXPERIMENTAL escalation (TASKS.md Phase 8): reports one simulated
  // joystick instead, to see what real code does with an actually-
  // connected device -- see kSimulatedDeviceHandle's own doc comment
  // near main()'s top for why. Real disassembly of what followed
  // (`ddragonz.mod`, traced live, not guessed) showed real code
  // immediately calling real vtable slot 3,
  // `IHID_CreateDevice(pIHID, nHandle, &ppDevice)`, and -- like every
  // other unchecked-result call site in this project's history --
  // dereferencing `*ppDevice` shortly after without checking the
  // return code. A blind stub answering slot 3 (the same "return 0,
  // touch nothing else" default every other slot here still uses)
  // left `*ppDevice` null, and real code wandered into it. Slot 3 is
  // now also overridden, handing back a second, separate generic
  // scaffold object (real `IHIDDevice` shape not confirmed against any
  // header, same deliberately-unguessed treatment as this file's other
  // unknown interfaces) instead of leaving the output pointer
  // untouched. That scaffold needs 40 slots, not this file's usual
  // 10-slot default for a *known*, fully-specified small interface:
  // real disassembly (also traced live) showed real code calling
  // IHIDDevice's own vtable slot 11 (byte offset 44) next, which a
  // 10-slot object doesn't have room for -- reading past its own
  // vtable into unmapped memory decoded as a null function pointer and
  // wandered exactly the same way.
  constexpr uint32_t kHidDeviceVtable = 0x80063000;
  constexpr uint32_t kHidDeviceObject = 0x80064000;
  // Real vtable ordering confirmed directly against the bundled real
  // AEEIHIDDevice.h (research/docs/sdk_installer_extract/sdk_installer_cab):
  // INHERIT_IQI's 3 slots (AddRef/Release/QueryInterface), then
  // GetDeviceInfo/GetDeviceStatus/RegisterForStatusChange/GetButtonInfo/
  // GetNumberOfButtons/RegisterForButtonEvent(8)/GetNextButtonEvent(9)/
  // GetPositionState/GetMinPositionInfo(11)/GetMaxPositionInfo(12)/
  // GetAxesInfo(13)/... -- slots 11-13 already matched real Double Dragon
  // call sites (`ddragonz.mod` offset 0x100af4-0x100b48) exactly.
  //
  // Queue of simulated AEEHIDButtonInfo events for GetNextButtonEvent(9)
  // to hand out one at a time -- how a real button *press* gets
  // delivered to real code, once real code asks for it. Each entry is
  // {nButtonID, nState, nButtonUID}; nButtonMin/nButtonMax are always
  // 0/1 for a simple digital button per the real header's own docs.
  auto simulated_button_events =
      std::make_shared<std::vector<std::array<int32_t, 3>>>();
  std::vector<zeebulator::HleRuntime::HleFunction> hid_device_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  hid_device_methods[8] = [](zeebulator::IArmCore& core) {
    // AEEResult RegisterForButtonEvent(IHIDDevice*, ISignal *piSignal)
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  hid_device_methods[9] = [simulated_button_events](zeebulator::IArmCore& core) {
    // AEEResult GetNextButtonEvent(IHIDDevice*, AEEHIDButtonInfo *pnButtonInfo,
    //   uint32 *pdwTimestamp, boolean *pbDroppedEvents)
    if (simulated_button_events->empty()) {
      core.SetRegister(zeebulator::kR0, 1);  // no more events (AEE_EFAILED-ish)
      return;
    }
    auto [button_id, state, button_uid] = simulated_button_events->front();
    simulated_button_events->erase(simulated_button_events->begin());
    uint32_t info_addr = core.GetRegister(zeebulator::kR1);
    // struct AEEHIDButtonInfo { int nButtonID; int nState; int nButtonUID;
    //   int nButtonMin; int nButtonMax; } -- confirmed field order/size
    // directly against the real AEEIHIDDevice.h.
    core.GetMemory().Write32(info_addr + 0, static_cast<uint32_t>(button_id));
    core.GetMemory().Write32(info_addr + 4, static_cast<uint32_t>(state));
    core.GetMemory().Write32(info_addr + 8, static_cast<uint32_t>(button_uid));
    core.GetMemory().Write32(info_addr + 12, 0);
    core.GetMemory().Write32(info_addr + 16, 1);
    uint32_t timestamp_addr = core.GetRegister(zeebulator::kR2);
    if (timestamp_addr != 0) core.GetMemory().Write32(timestamp_addr, 0);
    uint32_t dropped_addr = core.GetRegister(zeebulator::kR3);
    if (dropped_addr != 0) core.GetMemory().Write32(dropped_addr, 0);
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t hid_device_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kHidDeviceVtable, kHidDeviceObject, hid_device_methods);
  std::vector<zeebulator::HleRuntime::HleFunction> hid_methods(
      10, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  hid_methods[3] = [hid_device_obj](zeebulator::IArmCore& core) {
    // AEEResult CreateDevice(IHID*, int nHandle, IHIDDevice **ppDevice)
    uint32_t ppdevice = core.GetRegister(zeebulator::kR2);
    if (ppdevice != 0) {
      core.GetMemory().Write32(ppdevice, hid_device_obj);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  hid_methods[7] = [](zeebulator::IArmCore& core) {
    // AEEResult GetConnectedDevices(IHID*, int nDeviceType,
    //   int *pnDevHandles, int pnDevHandlesLen, int *pnDevHandlesLenReq)
    uint32_t device_handles_addr = core.GetRegister(zeebulator::kR2);
    uint32_t device_handles_len = core.GetRegister(zeebulator::kR3);
    uint32_t num_handles_req_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
    if (device_handles_addr != 0 && device_handles_len >= 1) {
      core.GetMemory().Write32(device_handles_addr, kSimulatedDeviceHandle);
    }
    if (num_handles_req_addr != 0) {
      core.GetMemory().Write32(num_handles_req_addr, 1);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  hid_methods[5] = [](zeebulator::IArmCore& core) {
    // AEEResult GetNextConnectEvent(IHID*, int *pnDevHandle, int *pnStatus,
    //   boolean *pbDroppedEvents) -- confirmed real signature and slot
    // index (research/docs/sdk_installer_extract/sdk_installer_cab, IHID's
    // real vtable order already documented above: CreateDevice(3)/
    // GetDeviceInfo(4)/GetNextConnectEvent(5)/RegisterForConnectEvents(6)/
    // GetConnectedDevices(7)). Real Alien Breaker Deluxe disassembly
    // (`abd.mod` 0x101a78-0x101b38) polls this slot in a real loop, once
    // per real connect/disconnect event, checking each reported device's
    // own UID (via slot 4/GetDeviceInfo) against a real target constant
    // that decodes to `AEEUID_HID_Joystick_Device` (0x0106c3fd, same real
    // UID already confirmed above for slot 7). Left as the same blind
    // `Stub` every other slot here defaults to (an unconditional
    // AEE_SUCCESS, "there's always a next event"), this is a genuine
    // real infinite loop, not a crash: real code's own termination
    // condition -- this call eventually reporting no more pending
    // events -- never arrives, confirmed live even at a 20-billion-step
    // budget. The correct, honest real answer is the same one already
    // established for slot 7: this project has no real joystick hardware
    // to report a connection event for, so no connect event is ever
    // pending. Returns AEE_EFAILED (matching the real documented "another
    // appropriate error code" contract for "no event retrieved"), the
    // same generic failure code this project's own `GetNextButtonEvent`
    // above already uses for its own analogous "queue empty" case.
    core.SetRegister(zeebulator::kR0, 1);  // AEE_EFAILED-ish: no connect event pending
  };
  uint32_t hid_obj = zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, /*vtable_address=*/0x8001C000,
                                                       /*object_address=*/0x8001D000, hid_methods);
  shell_hle.RegisterInstance(/*AEECLSID_HID=*/0x0106c411, hid_obj);
  // A second class this same routine unconditionally requires next
  // (gated on GetConnectedDevices' own success, not on device count --
  // still reached with zero devices). Confirmed this round (not just
  // call-order-shaped anymore) to be real AEECLSID_SignalCBFactory: its
  // slot 3 is really `ISignalCBFactory_CreateSignal(this, IDLECBFUNC pfn,
  // void *pUser, ISignal **ppISignal, ISignalCtl **ppISignalCtl)` --
  // confirmed by comparing three real call sites this round (all through
  // this same slot) against the real reference implementation in
  // research/samples/conftest_source/conftest/GamepadMgr.c, which
  // registers exactly three signals in exactly this order: a device
  // connect signal, a button-event signal, then a position-change
  // signal. Real Double Dragon code takes the same shape; its real
  // button-event callback address (`ddragonz.mod` 0x11bdf4, confirmed by
  // disassembly to match `L_JoystickButtonCB`'s real shape: it reads a
  // real AEEHIDButtonInfo -- see the AEEIHIDDevice.h struct definition
  // bundled in this repo's research/ -- and updates real per-button
  // bitmasks) is captured here so a simulated button press can invoke it
  // directly later, the same way a real fired ISignal would.
  auto captured_button_callback = std::make_shared<uint32_t>(0);
  auto captured_button_context = std::make_shared<uint32_t>(0);
  // Was gated on the callback address matching Double Dragon's own real
  // button-callback address literally (`ddragonz.mod` 0x11bdf4) -- a
  // real, confirmed identification for that one title, but not a real
  // general signal: every other title's own compiled code registers
  // its own callback at its own, different address, so that check can
  // never match for anyone else (found live bringing up Alien Breaker
  // Deluxe: `CreateSignal` genuinely fires, but the address check
  // silently never captures it, leaving `*captured_button_callback` at
  // 0 for the rest of the process). The real, general signal -- per
  // this same doc comment's own reference source
  // (research/samples/conftest_source/conftest/GamepadMgr.c) -- is
  // call *order*, not address: real code always registers exactly
  // three signals through this same slot, in a fixed sequence (device
  // connect, then button-event, then position-change). Capturing the
  // second call generalizes to any title using this same real
  // Signal-factory pattern, not just the one whose address happened to
  // be reverse-engineered first.
  auto signal_registration_count = std::make_shared<int>(0);
  std::vector<zeebulator::HleRuntime::HleFunction> signal_cb_factory_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  signal_cb_factory_methods[3] = [captured_button_callback, captured_button_context,
                                   signal_registration_count](zeebulator::IArmCore& core) {
    // AEEResult CreateSignal(ISignalCBFactory*, IDLECBFUNC pfn, void *pUser,
    //   ISignal **ppISignal, ISignalCtl **ppISignalCtl)
    uint32_t callback = core.GetRegister(zeebulator::kR1);
    uint32_t user_data = core.GetRegister(zeebulator::kR2);
    uint32_t out_signal_ctl = zeebulator::HleRuntime::ReadStackArg(core, 0);
    if (*signal_registration_count == 1) {
      *captured_button_callback = callback;
      *captured_button_context = user_data;
    }
    ++*signal_registration_count;
    if (out_signal_ctl != 0) {
      // Real code only ever checks this pointer for null/non-null
      // (RegisterFor*Event's own ISignal argument) and calls Detach/
      // Release on it at teardown, which this dev tool's own process
      // lifetime never reaches -- any stable nonzero token is enough.
      core.GetMemory().Write32(out_signal_ctl, 0x80065000);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t unknown_0x01041207_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x8001E000, /*object_address=*/0x8001F000,
      signal_cb_factory_methods);
  shell_hle.RegisterInstance(0x01041207, unknown_0x01041207_obj);
  // A real, previously-unreachable class found while testing the
  // simulated-connected-joystick change above (TASKS.md Phase 8):
  // once GetConnectedDevices reports a device, real code goes on to
  // call `ISHELL_CreateInstance(shell, ClsId=0x01005511, ...)` --
  // unconfirmed against any real header, but reached only through this
  // real HID-device code path, and -- like every other real
  // `CreateInstance` call site in this project's history -- not
  // checked for failure before its result gets used, wandering into
  // unmapped memory when left unregistered.
  //
  // Traced live this round (real call sites, not guessed): slot 4 gets
  // called three times with a small-integer-ID/value shape (`SetProperty`
  // -like: `(4, id=1, val_ptr)`, `(4, id=0x10, val=1)`, `(4, id=4,
  // val=0)`), then slot 3 registers a real callback (`ddragonz.mod`
  // `0x11d020`) with a real userdata pointer, then slot 6 gets one more
  // call. Disassembling the real registered callback: it only acts on an
  // event struct with field `+8 == 4` and field `+16` in `{2, 3}`,
  // branching straight into a second real function (`0x11f4dc`) that --
  // given a real sub-object at `pUser+8` and a nonzero byte at
  // `pUser+37` -- calls a real vtable slot 11 method with the literal
  // argument `100`. That shape (a status/percentage report, gated behind
  // a real download-catalog-style class ID, reached only once a real HID
  // controller was already detected -- i.e. as part of a broader real
  // "is the environment ready" sequence) strongly resembles Zeebo's own
  // real download/install-progress notification service, given the
  // platform's real download-based distribution model this whole
  // project's own catalog-ID handling already reflects. This repo's own
  // game assets genuinely are complete (three independent sources agree
  // byte-for-byte, PHASE8_LOG.md), so reporting "100% / complete" here
  // is a truthful simulation of real environment state, not a guessed
  // condition -- the same spirit as the already-simulated HID controller,
  // not a new kind of guess.
  auto captured_download_callback = std::make_shared<uint32_t>(0);
  auto captured_download_context = std::make_shared<uint32_t>(0);
  std::vector<zeebulator::HleRuntime::HleFunction> unknown_0x01005511_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  unknown_0x01005511_methods[3] = [captured_download_callback,
                                    captured_download_context](zeebulator::IArmCore& core) {
    *captured_download_callback = core.GetRegister(zeebulator::kR1);
    *captured_download_context = core.GetRegister(zeebulator::kR2);
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t unknown_0x01005511_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80060000, /*object_address=*/0x80061000,
      unknown_0x01005511_methods);
  shell_hle.RegisterInstance(0x01005511, unknown_0x01005511_obj);
  // Two more real, unidentified classes found investigating why
  // Peggle's tick loop settles into a fixed, non-progressing steady
  // state (TASKS.md Phase 8). The first is half of a real "try the
  // newer class, fall back to the older one" pair: real disassembly
  // (`peggle.mod` offset 0x104a50-0x104aa0) shows `ISHELL_CreateInstance`
  // called with ClsId 0x0103d8ec first, and -- only if that fails --
  // ClsId 0x01014bc4 next, both immediately after an `AddRef`-shaped
  // call on the same real IShell pointer. This exact instruction
  // sequence, both literal ClsIds included, also appears verbatim in a
  // second, independently-compiled real title
  // (`Super BurgerTime/mod/279125/supbtime.mod` offset 0x110e64-
  // 0x110ef4) -- strong evidence this is a real, standard SDK/compiler-
  // emitted helper rather than anything Peggle-specific. `0x01014bc4`
  // is NOT a second unidentified class needing its own scaffold, though
  // -- it's the already-real, already-registered `AEECLSID_EGL` above
  // (confirmed via the bundled SDK headers, not a guess). A generic
  // stub was mistakenly registered for it here too in an earlier round,
  // silently shadowing the real EGL object for every title, including
  // Double Dragon's own unrelated, direct `CreateInstance(AEECLSID_EGL)`
  // call during real graphics init -- found by tracing exactly why that
  // call's own `eglGetDisplay` was returning a blind 0 (TASKS.md Phase
  // 8). The fallback class this stub existed for is dead code anyway:
  // `0x0103d8ec` is itself an always-succeeding stub, so real code
  // never actually reaches the ClsId 0x01014bc4 fallback branch at all.
  // Two of this scaffold's slots need more than the blind `Stub`
  // treatment, found live tracing why Zeeboids/Zeebo Sports Volei (the
  // "Crazyball engine" siblings, TASKS.md) both wandered into unmapped
  // memory from an uninitialized object field, tens of thousands of
  // real steps into their own per-entity init code:
  // Slot 4 is a real "Register(this, interface_ptr, &out)"-shaped call
  // -- `zeeboids.mod` 0x191700 calls it with the real interface pointer
  // obtained one call earlier via the device-bitmap's own
  // `QueryInterface(0x01001045)` override above, then treats a non-zero
  // *out as a real success. The blind `Stub` never wrote *out, so this
  // always read back zero -- confirmed live to be exactly the value
  // that, threaded through two more real per-object fields, ends up as
  // a null vtable pointer dereferenced at the real crash site
  // (`zeeboids.mod` 0x1783e8). Echoing the interface pointer back
  // through its own out-param (the same "you get back what you handed
  // in" shape as slot 2's QueryInterface convention) fixed that half.
  // Slot 5 turned out to be a second, independent check on the same
  // result (`zeeboids.mod` 0x19171c calls it, expecting the literal
  // value `1` written to a 5th, stack-passed out-param) -- initially
  // looked like it might be real, un-mockable in-module type
  // verification (a real global "class registry" object this
  // scaffold's own object address happens to get cached into, at
  // `[registry+8]`), but live-tracing the actual call target (not
  // guessing from static disassembly) showed it resolves right back to
  // *this same scaffold*, via its own trap address -- i.e. it's this
  // project's own object being asked, through its own vtable, to
  // confirm something about itself. Writing the literal `1` through
  // that 5th out-param is what the caller's own `cmp r0,#1` requires.
  // Verified live (game_probe run, not guessed): both titles now run
  // clean through the real per-frame tick loop -- no more wandering
  // or unimplemented-instruction crashes -- until they hit the same
  // step-budget wall Zebo Sports Tênis's own long-but-real per-tick
  // work already established as "legitimately long, not a bug"
  // (TASKS.md).
  std::vector<zeebulator::HleRuntime::HleFunction> unknown_0x0103d8ec_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  unknown_0x0103d8ec_methods[2] = [&cpu, &hle, &display, &backend, &abd_font_atlas, &abd_text_state,
                                    kHeight](zeebulator::IArmCore& core) {
    // int QueryInterface(iname* _me, AEECLSID clsID, void** ppo) -- real
    // slot index (offset 8 = INHERIT_IQI's own slot 2, the standard
    // BREW/COM QueryInterface convention this whole family of scaffolds
    // already follows for its other confirmed slots). Real Alien Breaker
    // Deluxe disassembly (`abd.mod` 0x101e4c-0x101e7c) calls this real
    // slot twice on this exact scaffold, requesting two more real,
    // unidentified but clearly-related ClsIds (`0x0103d8dd`,
    // `0x0103d8ea` -- five and eighteen below this scaffold's own
    // `0x0103d8ec`, evidently the same real class family) -- reached
    // only once this project's own EVT_APP_RESUME fix let real code get
    // this far. Left as the generic blind `Stub` above (return 0, touch
    // nothing else), the real 3rd-arg out-param stays unwritten and real
    // code -- like every other unchecked-result call site this whole
    // project has found -- uses it anyway. Writes a fresh, independent,
    // all-slots-stub scaffold instead, the same safe, deliberately-
    // unguessed treatment this file already uses for every other
    // still-unidentified real interface (e.g. `0x01002001` below), so a
    // real vtable dispatch through the returned object lands on a real
    // trap instead of a null/garbage pointer. 200 slots, not this
    // file's usual 40-slot default: real code (`abd.mod` 0x1158d4)
    // immediately calls real vtable slot 79 (byte offset 0x13c) on the
    // returned object -- confirmed live (a 40-slot version of this
    // fix wandered outside the module exactly the same way this file's
    // own established "needs more slots" precedent, e.g. the HID
    // device scaffold above, already documents), so generous headroom
    // here matters, not just correctness for the one slot observed.
    static uint32_t next_addr = 0x800B0000;
    uint32_t stub_vtable = next_addr;
    uint32_t stub_object = next_addr + 0x800;
    next_addr += 0x1000;
    std::vector<zeebulator::HleRuntime::HleFunction> stub_methods(
        200, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
    // Real vtable slot 40 on this same real rendering-engine object
    // (already confirmed, TASKS.md Phase 8, as one of 17 real slots in
    // active use behind these real trampolines) takes no real position/
    // rect argument at all, only four real values -- confirmed live
    // this round (real `abd.mod` 0x1053ec -> 0x1154dc -> real slot 40,
    // return address `0x115520`) fires every real tick with a real,
    // fixed `r1=0x5500, r2=0x3c00, r3=0x8900` (plus a real fourth value,
    // `0x10000`, on the stack) -- this real shape (color components,
    // no geometry) matches a real "set current fill color" state-
    // setter far better than an immediate draw. Real 16.16 fixed-point
    // decode: (0x5500, 0x3c00, 0x8900, 0x10000)/65536 = (0.332, 0.234,
    // 0.535, 1.0) -- as RGBA that's a real dark purple/indigo, matching
    // this title's own real expected splash backdrop color. Stashed
    // here for the real draw site (below) to use when this real call's
    // own real texture-select was skipped (`AbdTextState::
    // splash_texture_selected_this_call`, set via the real `pc`
    // watchpoints in `CallArmFunctionChecked` above) instead of
    // sampling `bound_texture`, which can be real, stale-nonzero from
    // an unrelated earlier real selection in that exact case.
    auto pending_fill_color = std::make_shared<std::optional<std::array<uint8_t, 3>>>();
    stub_methods[40] = [pending_fill_color](zeebulator::IArmCore& core) {
      auto to_byte = [](uint32_t fixed16_16) {
        double normalized = static_cast<double>(fixed16_16) / 65536.0;
        int value = static_cast<int>(normalized * 255.0 + 0.5);
        return static_cast<uint8_t>(std::clamp(value, 0, 255));
      };
      *pending_fill_color = std::array<uint8_t, 3>{to_byte(core.GetRegister(zeebulator::kR1)),
                                                    to_byte(core.GetRegister(zeebulator::kR2)),
                                                    to_byte(core.GetRegister(zeebulator::kR3))};
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Real slot 64's two real callers (`abd.mod` 0x102b00, 0x10ac10)
    // both pass their own real 48-byte descriptor (r2) into slot 64,
    // then immediately read *descriptor (offset 0) afterward without
    // ever writing it themselves -- confirmed via real disassembly
    // that real slot 64 must write it itself, through the pointer, as
    // a real side effect: writing the real resolved texture-data
    // pointer (found at descriptor+44, itself written by a separate
    // real loader call) into descriptor+0 is what lets real slot 33
    // (below) start seeing real, non-zero, real-object-matching
    // texture references instead of a constant zero.
    //
    // descriptor+44 isn't always populated yet at real registration
    // time though (TASKS.md Phase 8: two real objects confirmed
    // sampled all-zero at real registration, populated only later) --
    // LOGO/LOGOSTAR specifically hit this live. Tracks every real
    // descriptor ever seen and re-resolves all of them on every real
    // slot 64/33 call, not just once, so a late real population still
    // gets picked up.
    auto descriptors = std::make_shared<std::vector<uint32_t>>();
    auto refresh_descriptors = [descriptors](zeebulator::IArmCore& core) {
      for (uint32_t d : *descriptors) {
        uint32_t texture_data_ptr = core.GetMemory().Read32(d + 44);
        if (texture_data_ptr != 0) core.GetMemory().Write32(d + 0, texture_data_ptr);
      }
    };
    stub_methods[64] = [descriptors, refresh_descriptors](zeebulator::IArmCore& core) {
      uint32_t obj = core.GetRegister(zeebulator::kR2);
      if (obj != 0) {
        if (std::find(descriptors->begin(), descriptors->end(), obj) == descriptors->end()) {
          descriptors->push_back(obj);
        }
        refresh_descriptors(core);
      }
      static int n = 0;
      if (n < 20) {
        std::fprintf(stderr, "[slot64] obj=0x%08x field0=0x%08x field44=0x%08x\n", obj,
                     obj ? core.GetMemory().Read32(obj + 0) : 0,
                     obj ? core.GetMemory().Read32(obj + 44) : 0);
        ++n;
      }
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Real slot 33 (dispatched via a real, generic "resolve this real
    // graphics-context's own per-object override, else a real
    // default" utility this whole subsystem shares, `abd.mod`
    // 0x115354) is this real engine's own real "select the current
    // real texture" call -- confirmed live tracing every real caller:
    // its own real r2 argument is always either 0 (no real texture,
    // plain-color real geometry) or a real pointer exactly matching a
    // real object already confirmed loaded via slot 64. Stashes it so
    // slot 107's own real draw call (below) knows which real texture,
    // if any, is currently real-bound when it fires.
    auto bound_texture = std::make_shared<uint32_t>(0);
    // Real per-draw-call decode cache, keyed by real texture object
    // address: every real textured draw (slot 107 below) was re-reading
    // this real texture's own compressed bytes out of emulated memory
    // and re-running `DecodeAtitc`/`DecodePng` on the *entire* real
    // texture from scratch, every single real draw call -- confirmed
    // live to be the real cause of a real, severe FPS drop (60->2) once
    // real gameplay starts drawing dozens of real on-screen entities
    // (bricks, paddle, ball) that mostly share one real, large 512x1024
    // texture: this project's own bridge was fully re-decompressing
    // that whole real texture dozens of times per real tick just to
    // sample one small real crop out of it each time. Real texture
    // objects are static real asset data once loaded (this project has
    // never observed one being rewritten after creation) -- caching the
    // real decoded RGBA buffer by real texture address, decoding once
    // and reusing it for every subsequent real draw, is safe for this
    // project's own single-run scope and needs no real invalidation.
    auto decoded_texture_cache =
        std::make_shared<std::map<uint32_t, std::pair<std::vector<uint8_t>, std::pair<int, int>>>>();
    stub_methods[33] = [bound_texture, refresh_descriptors](zeebulator::IArmCore& core) {
      refresh_descriptors(core);
      *bound_texture = core.GetRegister(zeebulator::kR2);
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Real slot 48 is this scaffold's own real "a new real screen is
    // about to begin" signal (TASKS.md Phase 8): silent through this
    // title's own real boot, then fires exactly twice per real screen
    // transition, one real tick *before* the rest of a whole cluster
    // of previously-silent real slots (4/35/88/89/91/106) start their
    // own real per-tick redraw for the new screen. Confirmed live,
    // against a real human's own real reference footage of this
    // title's real boot sequence, that this title draws (at least)
    // three real, visually distinct screens in strict real sequence
    // -- a splash, a title screen, then this real language-select
    // screen -- and that this project's own bridge was smearing all
    // of them together because nothing ever cleared `framebuffer_`
    // between them. An earlier real attempt cleared on the *later*
    // cluster (slot 106) instead and regressed live (it wiped real,
    // legitimate one-shot content -- "ENGLISH", the real footer --
    // that draws before slot 106's own first real call within the
    // same real transition); slot 48's one-real-tick head start is
    // early enough to precede that content instead, confirmed live on
    // a real desktop over several real minutes with no flicker and no
    // lost content.
    bool cleared_at_transition = false;
    stub_methods[48] = [&display, &cleared_at_transition](zeebulator::IArmCore& core) {
      if (!cleared_at_transition) {
        cleared_at_transition = true;
        display.ClearLiveFramebuffer();
      }
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Real slot 107 on this scaffold is this mystery engine's own real
    // "draw one geometry element" call (TASKS.md Phase 8) -- struct
    // layout confirmed (16.16 fixed-point {x0,y0,x1,y1}) across every
    // real caller sampled, not just one -- the format is a property of
    // slot 107's own real ABI, not of which upstream switch case
    // populated it. One specific real caller (`abd.mod` 0x105744) is
    // real per-character text-cell layout for this title's own
    // bitmap-font text; every other real caller is a real shape
    // (dividers, the center animation, and -- unconfirmed until this
    // round -- very plausibly menu/cursor chrome too, since simulated
    // confirm-button presses were producing real, successful real HID
    // callback runs with zero visible on-screen change while only text
    // was bridged). Both paths gated on `abd_font_atlas.has_value()`
    // (only ever set for this one real title's own real `data.bar`) so
    // every other title stays untouched.
    stub_methods[107] = [&hle, &display, &backend, &abd_font_atlas, &abd_text_state, kHeight,
                          bound_texture, pending_fill_color,
                          decoded_texture_cache](zeebulator::IArmCore& core) {
      core.SetRegister(zeebulator::kR0, 0);
      if (!abd_font_atlas.has_value()) return;
      uint32_t struct_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
      if (struct_addr == 0) return;
      uint32_t real_caller = zeebulator::HleRuntime::ReadStackArg(core, 1);
      int32_t raw_x0 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 0));
      int32_t raw_y0 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 4));
      int dst_x = raw_x0 / 65536;

      // Real caller 0x1054bc's own real texture-select is conditional
      // on real `r6 != 0` inside its real, shared caller `0x1053ec`
      // (TASKS.md Phase 8) -- when real `r6 == 0` for this real call
      // specifically, `bound_texture` can be real, stale-nonzero from
      // an unrelated earlier real selection (confirmed live: real
      // TITLE's own texture, selected as a real side effect of its own
      // real loader, not this real call). Real slot 40 (fired earlier
      // the same real `0x1053ec` invocation, unconditionally --
      // `pending_fill_color` above) is this real engine's own real
      // "set current fill color" call; using it here, instead of
      // whatever real texture happens to be stale-bound, is what a
      // real "draw a solid-color real quad, no texture" call actually
      // needs. Gated strictly on real caller `0x1054bc` (not `0x104f84`
      // -- LOGO/LOGOSTAR/TITLE reach this real slot through a
      // completely different real caller, `0x104db0`, never through
      // real `0x1053ec`, so this real per-call flag isn't meaningful
      // for them) and on this real call's own real texture-select
      // state being real false.
      if (real_caller == 0x1054bc && !abd_text_state.splash_texture_selected_this_call &&
          pending_fill_color->has_value()) {
        int32_t raw_x1 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 8));
        int32_t raw_y_far = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 20));
        int dst_y = raw_y0 / 65536;
        int dest_w = std::max(1, (raw_x1 - raw_x0) / 65536);
        int dest_h = std::max(1, (raw_y_far - raw_y0) / 65536);
        const auto& color = **pending_fill_color;
        std::vector<uint8_t> fill(static_cast<size_t>(dest_w) * dest_h * 4);
        for (size_t i = 0; i < fill.size(); i += 4) {
          fill[i + 0] = color[0];
          fill[i + 1] = color[1];
          fill[i + 2] = color[2];
          fill[i + 3] = 255;
        }
        display.BlitRgba(dst_x, dst_y, dest_w, dest_h, fill.data());
        return;
      }

      // Real caller 0x104f84 is this engine's own real background/
      // sprite texture draw call (TASKS.md Phase 8, live-traced). Its
      // real struct is *not* the real {x0,y0,x1,y1} sprite rect the
      // text/shape paths use -- confirmed via real disassembly of the
      // real anchor-alignment dispatch one level up (`abd.mod`
      // 0x104db0) and a real live struct dump: real offset+12 (this
      // project's own "y1" elsewhere) duplicates real y0, not a real
      // "far" edge; the real destination *height* instead lives at
      // real offset+20, confirmed live for this real caller as the
      // real full screen height (480), paired with real offset+8
      // (x1) holding the real full screen width (640) -- an explicit
      // real stretch target, not this real texture's own real native
      // 512x512 size (real mode 9, confirmed live for this real
      // caller, applies no real centering, so anchor position is this
      // same struct's own real x0/y0, used directly, in this real
      // mode's own real top-down convention -- not the real bottom-up
      // flip the real text/shape paths need, see their own doc
      // comments below; applying that flip here put the real image
      // fully below the real 480px display, confirmed live, before
      // this fix).
      //
      // Real caller 0x1054bc is a *second*, real full-screen draw
      // call, live-traced this round to the real boot sequence
      // (`abd.mod` 0x1054b4-0x1054b8, `mov r0, #2` then `bl
      // 0x115cf4`, well before the real TITLE screen's own real
      // 0x104f84 background draws start) -- its own real struct uses
      // the identical real {x0=0,y0=0} / real x1@+8 / real height@+20
      // full-640x480-screen convention confirmed live above for
      // 0x104f84. Live-dumped this round: its first real invocation
      // binds a real 512x512 ATITC texture with real flags=1 (RGB,
      // opaque -- this real engine's own real convention for a real
      // background with no real alpha channel, confirmed against the
      // real font atlas's own real flags=2/RGBA). Pixel-dumped this
      // round (not just decoded blind): this real texture is the real
      // "ALIEN BREAKER" title splash art itself (ship, 3D grid,
      // nebula backdrop), not a separate real developer/company logo
      // -- this project's earlier real assumption that this real
      // caller was the missing real purple-background splash was
      // wrong; that real splash (if it renders through this same real
      // bridge at all) is still unidentified. Real caller 0x1054bc
      // keeps firing every real frame afterward too, by then real-
      // bound to the real font atlas (flags=2) from unrelated real
      // text draws -- gated on real flags==1 here (not merged into
      // the real 0x104f84 branch's own unconditional real check) so
      // this new real path only ever draws real opaque backgrounds,
      // never real-overwrites the real TITLE/menu screens' own
      // already-working real 0x104f84 backgrounds with a real stale,
      // alpha-discarded, full-screen-stretched real font atlas.
      // Real caller `0x105744` covers *every* real draw that goes
      // through the real per-descriptor geometry-pack helper
      // (`abd.mod` 0x106508) -- text glyphs and menu-style icons alike.
      // This project originally treated those as two structurally
      // different real draws (gating icon handling on "no real pending
      // char index" and routing real glyphs through a separate,
      // hardcoded-atlas cell-copy path below), but live cross-validation
      // this round proved otherwise: a real per-character index is
      // itself just a byte offset into the exact same real 44-byte
      // descriptor array real icon draws already use (`char_index * 44`
      // -- see `AbdTextState::last_draw_descriptor_addr`'s own doc
      // comment). One real, generic, descriptor-driven textured draw
      // handles both -- no separate glyph path needed. Previously,
      // before this was understood, this whole real textured-draw
      // branch was gated on `0x104f84`/`0x1054bc` only, so any
      // `0x105744` draw without a recognized pending char index (real
      // menu icons, and any real text glyph whose source texture wasn't
      // this project's own hardcoded font-atlas buffer -- e.g. a real,
      // *second* font atlas confirmed live this round, `abd.mod`'s own
      // real texture `0x80300a2c`) silently fell through into the real
      // shape path below, which fills a real flat color instead of
      // sampling a real texture -- explaining both the real missing
      // menu icons and a real, separately-reported "menu font looks
      // different" symptom with the same one root cause.
      if ((real_caller == 0x104f84 || real_caller == 0x1054bc || real_caller == 0x105744) &&
          *bound_texture != 0) {
        auto& mem = core.GetMemory();
        uint32_t tex = *bound_texture;
        uint32_t signature = mem.Read32(tex + 0);
        uint32_t flags_for_gate = mem.Read32(tex + 12);
        if (real_caller == 0x1054bc && flags_for_gate != 1) return;
        // Shared by both real formats below: this real caller's own
        // real destination rect (see this branch's own doc comment
        // above) doesn't match a real decoded image's own real native
        // size for every real texture (confirmed live: real TITLE's
        // real 512x512 native texture against a real 640x480 real
        // destination) -- nearest-neighbor real upscale/downscale to
        // fit, since `BlitRgba` itself has no real scaling support.
        // Real descriptor-driven draws (`0x105744` -- text glyphs and
        // icons alike, see this branch's own doc comment above) sample
        // one real sub-rect out of whichever real texture is currently
        // bound, not the real whole texture -- live-dumped and cross-
        // validated this round (34,590 real live samples, one real
        // caller, zero exceptions): the real 44-byte descriptor
        // (tracked separately from the real repacked struct slot 107
        // itself sees, `AbdTextState::last_draw_descriptor_addr`) has
        // real pixel-space crop fields at offset+16/+20 (source x0,y0)
        // and offset+24/+28 (source width,height), cross-validated
        // against its own real normalized 0-1 UV fields at offset+0/
        // +4/+8/+12 assuming a real 512x512 texture (both real fields
        // agree exactly). Without this, `scale_and_blit` stretched the
        // real *entire* bound texture into one real glyph/icon-sized
        // destination, rendering a real, unrecognizable smear instead of
        // the one real intended glyph or icon.
        int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
        if (real_caller == 0x105744) {
          uint32_t desc = abd_text_state.last_draw_descriptor_addr;
          crop_x = static_cast<int32_t>(mem.Read32(desc + 16)) / 65536;
          crop_y = static_cast<int32_t>(mem.Read32(desc + 20)) / 65536;
          crop_w = static_cast<int32_t>(mem.Read32(desc + 24)) / 65536;
          crop_h = static_cast<int32_t>(mem.Read32(desc + 28)) / 65536;
        }
        auto scale_and_blit = [&](const std::vector<uint8_t>& decoded, int width, int height) {
          bool is_descriptor_draw = real_caller == 0x105744;
          int cx = is_descriptor_draw ? crop_x : 0;
          int cy = is_descriptor_draw ? crop_y : 0;
          int cw = is_descriptor_draw && crop_w > 0 ? crop_w : width;
          int ch = is_descriptor_draw && crop_h > 0 ? crop_h : height;
          int32_t raw_x1 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 8));
          int32_t raw_y_far = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 20));
          int dest_w = std::max(1, (raw_x1 - raw_x0) / 65536);
          // Real descriptor-driven draws (`0x105744`, text glyphs and
          // icons alike) use a real, different struct shape from
          // backgrounds -- live-dumped this round (an earlier guess,
          // offset+12, turned out to just duplicate offset+4/`raw_y0`
          // byte for byte, which is why icons first rendered as a real
          // 1px-tall sliver): this real struct's own real offset+20
          // field is smaller than real `raw_y0`, not larger (e.g. real
          // y0=332, real far=268, confirmed live against a real
          // 64x64-looking real icon) -- the real opposite ordering from
          // real mode-18 sprites, whose own real offset+20 is the larger
          // value. Subtracting the other way round for descriptor draws
          // only. Cross-validated this round against 34,590 real live
          // samples across both real glyph and real icon draws alike
          // (exact pixel match against the real, independently-computed
          // source crop rect above, zero exceptions): this real
          // convention is genuinely shared by both, not icon-specific.
          int dest_h = is_descriptor_draw ? std::max(1, (raw_y0 - raw_y_far) / 65536)
                                           : std::max(1, (raw_y_far - raw_y0) / 65536);
          // Real caller `0x104f84` covers both real mode 9 (TITLE --
          // already confirmed correct with no flip, see this branch's
          // own doc comment above) and real mode 18 (LOGO/LOGOSTAR),
          // which needs the same real bottom-up Y-flip the text/shape
          // paths already use (`AbdTextState::anchor_mode_18_this_call`
          // doc comment has the full real derivation). Real descriptor
          // draws need a real flip too, but real-anchored on `raw_y0`
          // (the real *larger* value in this real struct's own real
          // convention, confirmed live above), not `raw_y_far`. Real
          // caller `0x1054bc` never reaches real `0x104db0`'s own mode
          // dispatch at all (it draws through a completely different
          // real call chain), so this real per-call flag has no real
          // meaning there, and it stays unflipped.
          int dst_y;
          if (is_descriptor_draw) {
            dst_y = kHeight - (raw_y0 / 65536);
          } else if (real_caller == 0x104f84 && abd_text_state.anchor_mode_18_this_call) {
            dst_y = kHeight - (raw_y_far / 65536);
          } else {
            dst_y = raw_y0 / 65536;
          }
          if (dest_w == cw && dest_h == ch && cx == 0 && cy == 0 && cw == width && ch == height) {
            display.BlitRgba(dst_x, dst_y, width, height, decoded.data());
          } else {
            std::vector<uint8_t> scaled(static_cast<size_t>(dest_w) * dest_h * 4);
            for (int y = 0; y < dest_h; ++y) {
              int src_y = cy + static_cast<int>(static_cast<int64_t>(y) * ch / dest_h);
              for (int x = 0; x < dest_w; ++x) {
                int src_x = cx + static_cast<int>(static_cast<int64_t>(x) * cw / dest_w);
                const uint8_t* src = decoded.data() + (static_cast<size_t>(src_y) * width + src_x) * 4;
                uint8_t* dst = scaled.data() + (static_cast<size_t>(y) * dest_w + x) * 4;
                std::memcpy(dst, src, 4);
              }
            }
            display.BlitRgba(dst_x, dst_y, dest_w, dest_h, scaled.data());
          }
        };
        if (auto cache_it = decoded_texture_cache->find(tex); cache_it != decoded_texture_cache->end()) {
          scale_and_blit(cache_it->second.first, cache_it->second.second.first,
                          cache_it->second.second.second);
          return;
        }
        if (signature == 0xccc40002) {
          uint32_t width = mem.Read32(tex + 4);
          uint32_t height = mem.Read32(tex + 8);
          uint32_t flags = mem.Read32(tex + 12);
          uint32_t data_offset = mem.Read32(tex + 16);
          if (width > 0 && width <= 1024 && height > 0 && height <= 1024) {
            zeebulator::AtitcFormat format =
                (flags == 2) ? zeebulator::AtitcFormat::kRgba : zeebulator::AtitcFormat::kRgb;
            uint32_t blocks_w = (width + 3) / 4;
            uint32_t blocks_h = (height + 3) / 4;
            size_t bytes_per_block = (format == zeebulator::AtitcFormat::kRgba) ? 16 : 8;
            size_t compressed_size = static_cast<size_t>(blocks_w) * blocks_h * bytes_per_block;
            std::vector<uint8_t> compressed(compressed_size);
            uint32_t data_addr = tex + data_offset;
            for (size_t i = 0; i < compressed_size; ++i) {
              compressed[i] = mem.Read8(static_cast<uint32_t>(data_addr + i));
            }
            auto decoded = zeebulator::DecodeAtitc(compressed.data(), compressed.size(),
                                                    static_cast<int>(width),
                                                    static_cast<int>(height), format);
            if (decoded.has_value()) {
              auto& cached = (*decoded_texture_cache)[tex];
              cached.first = std::move(*decoded);
              cached.second = {static_cast<int>(width), static_cast<int>(height)};
              scale_and_blit(cached.first, cached.second.first, cached.second.second);
              return;
            }
          }
        } else if (static_cast<uint8_t>(signature) == 0x89 &&
                   static_cast<uint8_t>(signature >> 8) == 0x50) {
          // Real PNG signature (`PNG...`, first two real bytes
          // checked here; `DecodePng` itself verifies the full real
          // 8-byte magic) -- confirmed live this round (TASKS.md Phase
          // 8) that this real archive stores real small/UI assets (the
          // real splash logos included) as real PNG, not this real
          // engine's own real ATITC format used for real large
          // backgrounds/the real font atlas. `LoadResDataEx` (this
          // project's own already-working real resource loader) writes
          // the real, complete, verbatim real PNG file into this real
          // buffer -- read a real generously-sized window (comfortably
          // larger than every real sample found so far, topping out
          // under 5KB) and let `DecodePng`'s own real chunk-length
          // parsing stop at the real `IEND` chunk rather than needing
          // this project to separately know the real file's own exact
          // real length up front.
          constexpr size_t kMaxPngBytes = 262144;
          std::vector<uint8_t> raw(kMaxPngBytes);
          for (size_t i = 0; i < kMaxPngBytes; ++i) {
            raw[i] = mem.Read8(static_cast<uint32_t>(tex + i));
          }
          int width = 0, height = 0;
          auto decoded = zeebulator::DecodePng(raw.data(), raw.size(), width, height);
          if (decoded.has_value() && width > 0 && height > 0) {
            auto& cached = (*decoded_texture_cache)[tex];
            cached.first = std::move(*decoded);
            cached.second = {width, height};
            scale_and_blit(cached.first, cached.second.first, cached.second.second);
            return;
          }
        }
      }

      // Real shape path: same confirmed 16.16 fixed-point geometry,
      // filled with this engine's own real, confirmed default color
      // (TASKS.md Phase 8: real caller 0x1054bc sends an explicit
      // real SetColor(255,255,255) before every single one of 299
      // real calls sampled across a 60-real-second run, zero
      // exceptions; a second analyzed real caller never sends color
      // explicitly at all but never contradicts white either --
      // treated as this engine's own real default state, not a fresh
      // guess). Real width/height come from the same struct's real
      // x1/y1 fields. Same real Y-flip as the text path above (see its
      // own doc comment) -- the real "far" edge (y1) maps to the
      // smaller, top-of-screen real row after flipping.
      int32_t raw_x1 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 8));
      int32_t raw_y1 = static_cast<int32_t>(core.GetMemory().Read32(struct_addr + 12));
      int w = std::max(1, static_cast<int>((raw_x1 - raw_x0) / 65536));
      int h = std::max(1, static_cast<int>((raw_y1 - raw_y0) / 65536));
      int dst_y = kHeight - (raw_y1 / 65536);
      constexpr uint32_t kRectAddr = 0x00098000;
      core.GetMemory().Write16(kRectAddr + 0, static_cast<uint16_t>(dst_x));
      core.GetMemory().Write16(kRectAddr + 2, static_cast<uint16_t>(dst_y));
      core.GetMemory().Write16(kRectAddr + 4, static_cast<uint16_t>(w));
      core.GetMemory().Write16(kRectAddr + 6, static_cast<uint16_t>(h));
      uint32_t saved_lr = core.GetRegister(zeebulator::kLR);
      constexpr uint32_t kDisplayVtable = 0x80002000;
      constexpr uint32_t kDisplayObj = 0x80003000;
      uint32_t draw_rect_trap = core.GetMemory().Read32(kDisplayVtable + 5 * 4);
      hle.CallArmFunction(draw_rect_trap, kDisplayObj, kRectAddr, 0, 0x00FFFFFF);
      core.SetRegister(zeebulator::kLR, saved_lr);
    };
    uint32_t obj = zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, stub_vtable, stub_object,
                                                     stub_methods);
    uint32_t out_ptr = core.GetRegister(zeebulator::kR2);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, obj);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  unknown_0x0103d8ec_methods[4] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t interface_ptr = core.GetRegister(zeebulator::kR1);
    uint32_t out_ptr = core.GetRegister(zeebulator::kR2);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, interface_ptr);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  unknown_0x0103d8ec_methods[5] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t sp = core.GetRegister(zeebulator::kSP);
    uint32_t out_ptr = cpu.GetMemory().Read32(sp);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, 1);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t unknown_0x0103d8ec_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80040000, /*object_address=*/0x80041000,
      unknown_0x0103d8ec_methods);
  shell_hle.RegisterInstance(0x0103d8ec, unknown_0x0103d8ec_obj);
  // The third class, ClsId 0x01030766 (`peggle.mod` offset
  // 0x10a208-0x10a24c), is reached via a real IShell pointer stored at
  // offset +12 of the function's own struct parameter -- the same
  // confirmed Shell-field convention as the ambient app context struct
  // -- with its result stored unconditionally (no failure check) into
  // that struct's own offset +0x48. Its slot 2 (offset 8) is itself a
  // real, confirmed `ISHELL_CreateInstance`-shaped call site (`peggle.mod
  // 0x1099e0-0x1099f8`, found once the app-context fix let real code run
  // this far), always requesting the one real, already-identified ClsId
  // 0x0101eb0b -- built below, after `build_self_propagating_stub` is
  // defined, with that one slot overridden to forward to it for real
  // instead of the generic-stub treatment previously here (which let
  // real code dereference an unwritten out-param and crash). Every other
  // slot keeps the same deliberately-unguessed scaffold treatment
  // already established for `0x01002001` (see this file's/PHASE8_LOG.md's
  // Double Dragon history) -- this object's own broader identity is
  // still unconfirmed.
  // A third real, unidentified class, found continuing the Super
  // BurgerTime investigation past the stack/module collision and the
  // 0x40/0xc static-base slots (TASKS.md Phase 8): real code inside
  // `HandleEvent` (`supbtime.mod` offset 0x11be90-0x11be98) calls
  // `ISHELL_CreateInstance(shell, ClsId=0x01001017, ppObj=&g_2e28fc)`
  // -- a real module-global variable, not a stack slot -- and, like
  // every other real `CreateInstance` call site in this project's
  // history that turned out to matter, never checks the returned
  // status before dereferencing `*ppObj` two instructions later. Since
  // this class wasn't registered, `IShellHle::CreateInstanceImpl`
  // correctly returned failure and correctly left `*ppObj` untouched
  // (matching real `AEEShell.h` semantics) -- but real code reads it
  // anyway, calls a method on the resulting null "object", and crashes.
  // Confirmed via a live memory watchpoint on `g_2e28fc` spanning the
  // entire run (temporary, reverted) that nothing else ever writes
  // there -- this call site is the one and only real source of that
  // value.
  //
  // Its one real call site (`supbtime.mod` offset 0x11be90-0x11be98,
  // immediately after `CreateInstance` succeeds) calls this object's
  // own slot 7 (byte offset 0x1c) with `(this, flag=0x4000,
  // callback=0x11c06c, user_data=0)`. `0x11c06c` is real, disassembled
  // ARM code, not data -- and its own body is a textbook "process a
  // list of registered objects once per call" shape: dereference a
  // real module-global list head; if empty, return immediately; else
  // walk the list calling a vtable method on each entry. That's
  // exactly what a real per-frame "run one engine tick" function looks
  // like, matching this title's own "generic arcade-core" structure
  // (TASKS.md Phase 8) -- and it's registered here but never actually
  // invoked, since a plain no-op stub just returns success without
  // scheduling it, and nothing else in this run ever calls it.
  //
  // EXPERIMENTAL, and a real step beyond every other generic scaffold
  // in this file: rather than leave slot 7 a no-op, this schedules the
  // given callback through the existing, already-real `IShellHle`
  // timer mechanism (the same one Double Dragon/Peggle's own
  // self-rearming `SetTimer` callbacks run through), on the same
  // 16ms cadence `kTickMs` uses elsewhere in this file -- an inferred
  // interval, not one the real call site actually provides. Marked
  // clearly as an inference rather than confirmed real behavior: the
  // *fact* that a callback gets registered here is directly evidenced;
  // the specific interval chosen to drive it is not.
  //
  // A second override is needed for a real, different reason: that
  // list is a single-entry list whose one entry is this very object
  // (`ppObj` from the `CreateInstance` call above, real address
  // `0x002e28fc` -- confirmed live and stable via a temporary memory
  // watchpoint spanning a full run, TASKS.md Phase 8), and `0x11c06c`'s
  // loop has no exit *except* that address reading back 0. Nothing
  // else in this codebase, real or stubbed, ever writes to it after
  // `CreateInstance` -- a real implementation would presumably do so
  // itself, once whatever real work it represents (almost certainly the
  // romset load this class is tied to) finishes.
  //
  // First attempt put this clear on slot 11 (byte offset 0x2c, real
  // disassembly of `0x11c06c` confirms it's the first of *three* real
  // vtable calls `0x11c06c`'s per-entry body makes on this same object
  // every single pass: slot 0x2c ("tick"), then an unrelated object's
  // slot 0x90 (fed slot 0x2c's return value via r1 -- a real, evidenced
  // status hand-off), then slot 0x28 again on this object, all *before*
  // the loop re-reads the list head to decide whether to exit). Clearing
  // the list head from inside slot 0x2c made the very next same-pass
  // call (slot 0x28, still against the now-null `[r4]`) dereference a
  // null vtable unconditionally -- confirmed via a live register trace
  // at the exact call site (`supbtime.mod` 0x11c0cc): call target
  // resolved to `[0x28]` = 0, jumping to address 0, then (this codebase's
  // already-documented "wander through zeroed memory" behavior) walking
  // 262,144 harmless zero-decoded steps until PC coincidentally lands
  // back on the module's own real load address (`kBase`), re-entering
  // and re-running its one-time ROPI relocation-fixup veneer a second
  // time over a table whose backing storage was already zeroed and
  // reclaimed as scratch after its first, legitimate use -- which
  // self-corrupts real code (module offset `0x9c`) into what eventually
  // decodes as an unimplemented `MRS`/`MSR`-space instruction. Not a new,
  // separate CPU gap: a direct, traced consequence of clearing the list
  // head one real sub-call too early.
  //
  // **Fixed** by moving the clear to slot 10 (byte offset 0x28) instead
  // -- the *last* of the three real per-pass sub-calls, called after
  // slot 0x2c and the other object's slot 0x90 have already run against
  // a still-valid, non-null object. Still an honest, minimal placeholder
  // (not a claim about which slot "really" owns cleanup, just the
  // latest-firing of the three already-being-called real slots, chosen
  // specifically so nothing in the same pass dereferences the entry
  // again afterward) -- not a claim that real loading finishes in one
  // frame, just the simplest choice that doesn't require inventing an
  // arbitrary frame count.
  constexpr uint32_t kSbtTaskListHeadAddress = 0x002e28fc;
  std::vector<zeebulator::HleRuntime::HleFunction> sbt_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  sbt_methods[7] = [&shell_hle](zeebulator::IArmCore& core) {
    constexpr uint32_t kInferredTickMs = 16;
    uint32_t callback = core.GetRegister(zeebulator::kR2);
    uint32_t user_data = core.GetRegister(zeebulator::kR3);
    // r1 at this real call: found live (TASKS.md Phase 8, the Zeebo
    // Sports Tênis/Zeeboids round) to be a real, non-zero, previously-
    // discarded argument -- see IShellHle::ScheduleTimer's own doc
    // comment on `r0_override` for the real evidence this is the
    // callback's own real first argument, not a coincidence.
    uint32_t r1_at_registration = core.GetRegister(zeebulator::kR1);
    shell_hle.ScheduleTimer(kInferredTickMs, callback, user_data, r1_at_registration);
    core.SetRegister(zeebulator::kR0, 0);  // SUCCESS
  };
  sbt_methods[10] = [](zeebulator::IArmCore& core) {
    core.GetMemory().Write32(kSbtTaskListHeadAddress, 0);
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t unknown_0x01001017_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80046000, /*object_address=*/0x80047000,
      sbt_methods);
  shell_hle.RegisterInstance(0x01001017, unknown_0x01001017_obj);
  // Real code fetches "the current app's IShell"/"IDisplay" from an
  // ambient context (the static-base table's offset-0xc0 slot) in many
  // places, not just via the pIShell argument explicitly passed to
  // AEEMod_Load/CreateInstance -- see core/brew/mod_runtime.h.
  mod_runtime.SetShellInstance(shell);
  mod_runtime.SetDisplayInstance(display_obj);
  // The same ambient context struct has a third real field (offset
  // 0x2c) found probing Peggle -- real code there calls through it
  // using ARM RVCT's ROPI relative-vtable convention, unlike every
  // other confirmed interface here. Its real identity is still
  // unknown; wired to a relative-vtable-safe scaffold (see
  // BuildGenericRelativeVtableStubObject's doc comment) purely so the
  // call resolves rather than wandering into unmapped memory.
  uint32_t unknown_context_0x2c_obj = zeebulator::BuildGenericRelativeVtableStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80010000, /*object=*/0x80011000, /*slot_count=*/20);
  mod_runtime.SetThirdContextObject(unknown_context_0x2c_obj);
  // A fifth real field (offset 0x28) found continuing the investigation
  // past the fourth field's arena gate: real code (peggle.mod offset
  // 0x132dfc, called with no null check beforehand) reads it and calls
  // through it using the exact same ROPI relative-vtable convention as
  // the third field above -- see mod_runtime.h's doc comment. Same
  // treatment as the third field: a safe, do-nothing relative-vtable
  // scaffold so the call resolves instead of wandering into unmapped
  // memory.
  uint32_t unknown_context_0x28_obj = zeebulator::BuildGenericRelativeVtableStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80014000, /*object=*/0x80015000, /*slot_count=*/20);
  mod_runtime.SetFifthContextObject(unknown_context_0x28_obj);
  // A fourth real field (offset 0x24) found continuing the Peggle
  // investigation into why its per-tick callback never re-arms its own
  // timer the real self-rearming way: real code there reads and writes
  // this as a plain data struct (not a vtable interface), gating its
  // entire timer-rearming path on offset +20 being non-zero. Real
  // identity unknown -- wired to a real, writable, zeroed memory block
  // with just that one confirmed-load-bearing field pre-set non-zero,
  // an educated, minimal enabling stub (see mod_runtime.h's doc
  // comment), not a confirmed-correct implementation of whatever this
  // struct actually is.
  constexpr uint32_t kFourthContextObject = 0x80020000;
  cpu.GetMemory().Write32(kFourthContextObject + 20, 1);  // rest is already zero
  mod_runtime.SetFourthContextObject(kFourthContextObject);
  // Real code elsewhere (peggle.mod offset 0x989c-0x99f8, reached from
  // the same tick-0 callback once it got past the +20 gate above) reads
  // `context[0x24] + 0x45000 + 0x3d8` as a real, standalone object --
  // confirmed by the call shape that follows it (`ldr r0,[fp]; ldr
  // r3,[r0,#8]; mov r0,fp; bx r3`, i.e. a plain absolute vtable call at
  // slot 2 with `fp` itself as `this`, not the ROPI relative-vtable
  // shape the third context field uses) -- ordinary enough to give a
  // real, generic stub object rather than modeling the rest of this
  // apparent large global arena, which nothing yet requires understood.
  // Written after HandleEvent(EVT_APP_START) below, not here -- real
  // code (confirmed via a live memory watchpoint) writes a real zero
  // to this exact field once during that call, presumably a real
  // "not yet initialized" reset that legitimately precedes whatever
  // real code would normally populate it for real.
  //
  // Real code calls this object's own slot 2 with a shape matching a
  // real QueryInterface-style call (`this`, an id/flag, and a pointer
  // to receive the result) and, without checking the result for
  // failure, immediately dereferences whatever slot 2 wrote there.
  // Live tracing found the exact same shape recur at least twice in a
  // row through freshly-returned objects, with no sign of stopping --
  // so rather than manually re-diagnosing and hand-patching each
  // successive level, every slot of every object below is built the
  // same self-propagating way: succeed (r0=0) and write a fresh object
  // of the same kind into whatever the caller passed as the output
  // pointer (r2), lazily, however deep a real chain of these turns out
  // to go. EXPERIMENTAL and specific to this investigation (TASKS.md
  // Phase 8) -- not yet confirmed as a general real BREW convention,
  // so deliberately kept local to this tool rather than promoted to
  // scaffold_object.h.
  //
  // Initially built every one of the 40 slots this same self-propagating
  // way, which appeared to work for two chained levels before hitting a
  // wall where a real caller read offset 0x30 off what looked like one of
  // these objects directly rather than through its vtable -- but full
  // register-level tracing (temporary, reverted) of that exact call chain
  // (peggle.mod offsets 0x1099e0-0x109aac) showed that was a misdiagnosis:
  // the real bug is that only real slot 2 uses the (this, id, ppOut@r2)
  // shape above. Real slot 3 is a *different*, also-real shape --
  // (this, ppOut@r1), no id argument (confirmed at peggle.mod offset
  // 0x109a98: `ldr r0,[fp]; add r1,sp,#0x24; ldr r2,[r0,#0xc]; mov
  // r0,fp; bx r2`) -- and other real slots (e.g. slot 4, confirmed at
  // offset 0x109a00) are called with no output pointer at all, just
  // leftover garbage sitting in r1/r2 from earlier code. Blindly writing
  // a fresh object into r2 for every slot corrupted whatever r2 happened
  // to hold for those other calls -- including, once, real address 0 --
  // and it was real code later reading back that corrupted memory (not a
  // third real object convention) that produced the offset-0x30 wall.
  // Fixed by only special-casing the two real, evidenced shapes (slot 2
  // via r2, slot 3 via r1, each skipped if the pointer is null -- a real
  // "just checking, don't return anything" pattern also observed at
  // peggle.mod offset 0x109a94's `bl 0x105b50` with r1=r2=0) and leaving
  // every other slot a plain, side-effect-free stub.
  // Real address range chosen to avoid the collision bug found and fixed
  // this round (TASKS.md Phase 8, Peggle): this counter and the slot-4
  // stub counter below used to start at 0x80030000/0x80038000 -- only
  // 8 slots apart -- and neither knew about the other or about this
  // file's many other fixed object addresses (which top out around
  // 0x80066000). With `resources.bar` wired in and real code recursing
  // through more real QueryInterface chains than before, this counter
  // grew far enough to silently overwrite a *different*, fixed-address
  // object's own vtable pointer with an unrelated HLE trap address --
  // confirmed live (a temporary register trace at the real crash site
  // showed the "vtable" read resolving to a trap address, not a real
  // vtable). Fixed by giving each dynamic counter its own large,
  // separated range, well past every fixed address and far short of
  // FileHle's own region at 0x80100000.
  uint32_t next_self_propagating_addr = 0x80070000;
  // Real caller `0x10ac10`'s own real "fetch the real decoded PNG
  // size" call (`abd.mod` 0x10adc8-0x10add8) does not go through the
  // real self-propagating-stub object real slot 3's own real "feed"/
  // "terminate" calls below used -- it goes through `self+408`, a
  // real, separate, already-identified real object this project
  // already builds: real `ISHELL_CreateInstance(ishell, ClsId=
  // 0x01030766, &self[408])` (`abd.mod` 0x10da28-0x10da3c, confirmed
  // live this round -- a real destructor at 0x10d9e8 also real-
  // releases it via real vtable slot 1, confirming a real, genuine
  // ref-counted real interface, not a real coincidental register
  // value). This project's own real `unknown_0x01030766_methods[3]`
  // (below, in `main`) already exists for this real ClsId; shared
  // (not per-self-propagating-object) real state here lets it hand
  // back this real round's own real decoded-size result instead of
  // its real, existing generic "write a new child" default.
  auto pending_png_result = std::make_shared<std::optional<std::pair<uint32_t, uint32_t>>>();
  std::function<uint32_t()> build_self_propagating_stub =
      [&cpu, &hle, &next_self_propagating_addr, &build_self_propagating_stub,
       pending_png_result]() -> uint32_t {
    uint32_t vtable_addr = next_self_propagating_addr;
    uint32_t object_addr = next_self_propagating_addr + 0x800;
    next_self_propagating_addr += 0x1000;
    auto propagate_into = [&cpu, &build_self_propagating_stub](zeebulator::ArmRegister out_reg) {
      return [&cpu, &build_self_propagating_stub, out_reg](zeebulator::IArmCore& core) {
        uint32_t out_ptr = core.GetRegister(out_reg);
        if (out_ptr != 0) {
          cpu.GetMemory().Write32(out_ptr, build_self_propagating_stub());
        }
        core.SetRegister(zeebulator::kR0, 0);
      };
    };
    std::vector<zeebulator::HleRuntime::HleFunction> methods(
        40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
    methods[2] = propagate_into(zeebulator::kR2);
    // Real slot 3, for this real title, is a real image-decode
    // interface (TASKS.md Phase 8, full derivation there): fed a
    // real complete PNG buffer+size, then real-terminated with
    // `(0, 0)`. Detected here by real PNG signature sniffing (the
    // same real convention this project's own texture-draw bridge
    // already uses to pick ATITC vs PNG) rather than by real object
    // identity, since this real scaffold is shared, unidentified
    // real infrastructure -- any real call that doesn't match this
    // real shape falls through to the real, original, Peggle-derived
    // propagate-a-child behavior untouched. The real *fetch* half of
    // this real interface is handled elsewhere -- see
    // `unknown_0x01030766_methods[3]`'s own doc comment in `main`.
    methods[3] = [&cpu, &build_self_propagating_stub,
                  pending_png_result](zeebulator::IArmCore& core) {
      auto& mem = cpu.GetMemory();
      uint32_t r1 = core.GetRegister(zeebulator::kR1);
      uint32_t r2 = core.GetRegister(zeebulator::kR2);
      if (r1 != 0 && r2 >= 24 && static_cast<uint8_t>(mem.Read32(r1)) == 0x89 &&
          static_cast<uint8_t>(mem.Read32(r1) >> 8) == 0x50) {
        uint32_t descriptor = core.GetRegister(zeebulator::kR4);
        if (descriptor != 0) mem.Write32(descriptor + 44, r1);
        // Real PNG IHDR width/height are real 4-byte big-endian
        // fields (bytes 16-19/20-23) -- real low 16 bits at real
        // bytes 18-19/22-23 for any real image under 65536px.
        uint32_t width = (mem.Read8(r1 + 18) << 8) | mem.Read8(r1 + 19);
        uint32_t height = (mem.Read8(r1 + 22) << 8) | mem.Read8(r1 + 23);
        *pending_png_result = std::make_pair(width, height);
        core.SetRegister(zeebulator::kR0, 0);
        return;
      }
      if (r1 != 0) {
        mem.Write32(r1, build_self_propagating_stub());
      }
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Real slot 4 -- confirmed via a real ClsId this object's own slot 2
    // (QueryInterface) is asked for, `0x0101eb0b` -- is called with no
    // output-pointer argument at all (`this` only) and its *return
    // value* is treated as a fresh object in its own right: real code
    // (peggle.mod 0x109a54-0x109a94) immediately calls *that* object's
    // own slot 3 repeatedly with `(this, buffer_ptr, size)`, in 1000-
    // byte chunks, ending with a final `(this, 0, 0)` call -- a real
    // chunked "write a stream of bytes" shape, not the `(this,
    // ppOut@r1)` QueryInterface-child shape slot 3 means on *this*
    // outer object. Confirmed as a real, shared pattern (not a Peggle
    // quirk) by finding `0x0101eb0b` also referenced, via
    // byte-identical statically-linked helper code, in Zuma's
    // Revenge's own real `zumar.mod` (TASKS.md Phase 8 -- the user
    // made the full 61-title dump collection available this round).
    // Real meaning still unidentified (very plausibly telemetry/
    // logging, given the "chunked write, nobody checks the result"
    // shape), so this returns a plain, independent, all-slots-stub
    // object rather than another self-propagating one -- correct
    // *because* its own slot 3 needs (this, buf, size) write semantics
    // (a safe no-op discard is honest and sufficient; nothing reads a
    // return value from those writes), not QI-child semantics.
    methods[4] = [&cpu, &hle](zeebulator::IArmCore& core) {
      // Separated from build_self_propagating_stub's own counter above
      // (and every fixed object address in this file) -- see that
      // counter's own doc comment for the real address-collision bug
      // this avoids.
      static uint32_t next_addr = 0x80090000;
      uint32_t stub_vtable = next_addr;
      uint32_t stub_object = next_addr + 0x800;
      next_addr += 0x1000;
      std::vector<zeebulator::HleRuntime::HleFunction> stub_methods(
          40, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
      uint32_t obj = zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, stub_vtable,
                                                       stub_object, stub_methods);
      core.SetRegister(zeebulator::kR0, obj);
    };
    return zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, vtable_addr, object_addr,
                                             methods);
  };
  uint32_t unknown_arena_0x453d8_obj = build_self_propagating_stub();
  // The interface class itself -- 0x0101eb0b, the real shared
  // PopCap/Zeebo SDK interface documented on `build_self_propagating_stub`
  // above -- was fully reverse-engineered (that stub's own slot 4 was
  // added specifically for it) but never actually registered as a real
  // `ISHELL_CreateInstance` target. Found continuing the Peggle
  // investigation once the app-context fix (TASKS.md Phase 8) let real
  // code run far enough to reach this real call for the first time:
  // `peggle.mod 0x1099f8` calls `ISHELL_CreateInstance(shell, 0x0101eb0b,
  // &ppObj)`, and since it was unregistered, `CreateInstanceImpl` failed
  // without ever writing `*ppObj` -- real code doesn't check the return
  // value, so it dereferenced whatever garbage was already on the stack
  // there, landing on a null function pointer and crashing out of the
  // module entirely.
  shell_hle.RegisterInstance(0x0101eb0b, build_self_propagating_stub());
  // `unknown_0x01030766_obj` (see its own doc comment above, near its
  // real ClsId's other registration): its slot 2 is a real, confirmed
  // `ISHELL_CreateInstance`-shaped call, always requesting `0x0101eb0b`.
  // Its slot 3 (`peggle.mod 0x109a98-0x109aac`, found immediately after
  // getting slot 2 working) is a real, confirmed `(this, &ppOut)`
  // single-out-param call -- the same "QueryInterface-child" shape
  // `build_self_propagating_stub`'s own slots 2/3 already implement --
  // so it gets the same treatment: write a fresh self-propagating child
  // into `*ppOut`. `object_address` kept identical to the previous
  // generic-stub build (0x80045000) so nothing else in this file needs
  // to change.
  std::vector<zeebulator::HleRuntime::HleFunction> unknown_0x01030766_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  unknown_0x01030766_methods[2] = [&cpu, &build_self_propagating_stub](zeebulator::IArmCore& core) {
    uint32_t cls_id = core.GetRegister(zeebulator::kR1);
    uint32_t ppobj = core.GetRegister(zeebulator::kR2);
    if (cls_id != 0x0101eb0b) {
      core.SetRegister(zeebulator::kR0, 1);  // EFAILED-ish: unknown class
      return;
    }
    if (ppobj != 0) cpu.GetMemory().Write32(ppobj, build_self_propagating_stub());
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Real caller `0x10ac10`'s own real "fetch the real decoded PNG
  // size" call (`abd.mod` 0x10adc8-0x10add8) lands here -- confirmed
  // live this round: `self+408` (the real field that real call reads
  // through) is populated by a real, earlier `ISHELL_CreateInstance
  // (ishell, 0x01030766, &self[408])` (`abd.mod` 0x10da28-0x10da3c),
  // and real-released via real vtable slot 1 in a real destructor
  // (`abd.mod` 0x10d9f4-0x10da08) -- a real, genuine ref-counted
  // real interface reference, not a real coincidental register
  // value. `pending_png_result` (see `build_self_propagating_stub`'s
  // own doc comment above) carries the real width/height this real
  // scaffold's own real "feed a PNG" slot-3 branch already staged.
  //
  // The real result this real call hands back isn't a real plain
  // data struct -- confirmed live this round the hard way (a real
  // crash): real caller code immediately does `ldr r1,[r0]; ldr
  // r3,[r1,#48]; blx r3` on it (`abd.mod` 0x10addc-0x10adf0) -- a
  // real virtual call through real vtable slot 12, not a real field
  // read. A first, plain-struct version of this real fix had no real
  // vtable at real offset 0, so that real call jumped through real
  // NULL and crashed. Reusing `build_self_propagating_stub()` itself
  // solves this for real: it already builds a real object with a
  // real, valid, all-40-slots vtable (real slot 12 defaults to a
  // real safe no-op, since only real slots 2/3/4 are ever
  // overridden) via `BuildInterfaceObject` -- which (confirmed by
  // reading its own real implementation) writes only a single real
  // word (the real vtable pointer) at real object offset+0, leaving
  // every real byte from +4 onward real free. Real width/height/
  // pixel-pointer/bpp go into that real free space, at the exact
  // real offsets `abd.mod` 0x10ae34-0x10b168 actually reads -- one
  // real object serves as both a real, virtual-call-safe interface
  // and this real fix's own real data carrier.
  unknown_0x01030766_methods[3] = [&cpu, &build_self_propagating_stub,
                                    pending_png_result](zeebulator::IArmCore& core) {
    auto& mem = cpu.GetMemory();
    uint32_t ppout = core.GetRegister(zeebulator::kR1);
    if (ppout != 0 && pending_png_result->has_value()) {
      uint32_t width = (*pending_png_result)->first;
      uint32_t height = (*pending_png_result)->second;
      uint32_t result_obj = build_self_propagating_stub();
      static uint32_t next_pixel_addr = 0x80200000;
      uint32_t pixel_addr = next_pixel_addr;
      uint32_t pixel_bytes = width * height * 4;
      next_pixel_addr += pixel_bytes + 64;
      for (uint32_t i = 0; i < pixel_bytes; ++i) mem.Write8(pixel_addr + i, 0);
      mem.Write32(result_obj + 8, pixel_addr);
      mem.Write32(result_obj + 12, 0);
      mem.Write16(static_cast<uint32_t>(result_obj + 20), static_cast<uint16_t>(width));
      mem.Write16(static_cast<uint32_t>(result_obj + 22), static_cast<uint16_t>(height));
      mem.Write8(result_obj + 28, 32);  // real bits-per-pixel: real RGBA8888
      mem.Write32(ppout, result_obj);
      pending_png_result->reset();
      core.SetRegister(zeebulator::kR0, 0);
      return;
    }
    if (ppout != 0) mem.Write32(ppout, build_self_propagating_stub());
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t unknown_0x01030766_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80044000, /*object_address=*/0x80045000,
      unknown_0x01030766_methods);
  shell_hle.RegisterInstance(0x01030766, unknown_0x01030766_obj);
  media_hle.Build(/*vtable=*/0x8000B000);
  // AEECLSID_MEDIA, found live via LR-capture (TASKS.md/PHASE8_LOG.md
  // Phase 8, the sound investigation): real code calls
  // `ISHELL_GetHandler(shell, 0x01005500, pszMIME)` -- see ishell.h's
  // own doc comment on GetHandlerImpl for the full derivation -- then
  // immediately CreateInstance()s whatever that returns. A *factory*,
  // not a single shared instance: further disassembly (`ddragonz.mod`
  // 0x10a1e0, the real GetHandler+CreateInstance call site) sits inside
  // the same function that runs once per cached sound.ggz resource
  // activated into a playback slot -- i.e. real code expects a fresh
  // IMedia per sound, not one shared instance every new sound would
  // silently stomp.
  shell_hle.RegisterFactory(/*AEECLSID_MEDIA=*/0x01005500,
                             [&media_hle]() { return media_hle.CreateMediaObject(); });
  // Sibling media class IDs, found live via the same LR-capture as
  // AEECLSID_MEDIA above (PHASE8_LOG.md, "Sound, round twelve"): a
  // second, near-identical real function (`ddragonz.mod` 0x10a0c0)
  // runs the exact same GetHandler-less CreateInstance+SetMediaParm+
  // RegisterNotify sequence as the confirmed AEECLSID_MEDIA one, but
  // takes its class ID as a caller-supplied parameter rather than a
  // hardcoded constant -- and real, repeated `CreateInstance` calls
  // for `0x0100550a` and `0x01005501` were captured live at that exact
  // function's own call site (`lr=0x0010a12c`) from the very first
  // round of this investigation, never followed up on until real
  // gameplay sound was confirmed to work via another real
  // implementation of this same ROM, which ruled out "this build's
  // data never triggers sound" and pointed back at gaps in this
  // project's own class registration instead. Almost certainly
  // separate real sound *channels* (e.g. one or more SFX channels
  // distinct from the confirmed background-music one) that were
  // silently failing `CreateInstance` this entire investigation --
  // registered here with the same generic per-call factory, since
  // this project's own `MediaHle` doesn't dispatch on codec/class
  // identity at all (it sniffs real container magic bytes instead).
  shell_hle.RegisterFactory(0x0100550a,
                             [&media_hle]() { return media_hle.CreateMediaObject(); });
  shell_hle.RegisterFactory(0x01005501,
                             [&media_hle]() { return media_hle.CreateMediaObject(); });

  auto& mem = cpu.GetMemory();
  // A real stack, well past the loaded module -- ArmInterpreter::Reset()
  // zeroes every register including SP, and the real compiled prologue's
  // first instruction is `STR LR,[SP,#-4]!`; without this, that write
  // corrupts memory near address 0 and every stack-relative access after
  // it, matching exactly the convention tools/mod_probe.cpp already uses.
  // Sized relative to the real module (a fixed `kBase + 0x200000` offset
  // silently collided with real module data for Super BurgerTime's own
  // 2.8MB `.mod` -- see PHASE8_LOG.md for the full real evidence: a real
  // ROPI relocation-fixup table computed from real, file-embedded
  // literals landed squarely inside where that fixed offset put SP,
  // making the table read back as zero mid-walk and self-corrupting
  // real code well before any HLE surface was ever reached).
  cpu.SetRegister(zeebulator::kSP, kBase + mod_size + 0x00200000);

  // AEEMod_Load must be the first thing in the module (real BREW
  // requirement, confirmed against AEEModGen.c in Phase 3) -- and
  // Phase 2's real .mod probing already validated file offset 0 as a
  // coherent function prologue for this exact file.
  uint32_t entry = kBase;

  const char* stage = "AEEMod_Load";
  uint32_t applet_ptr = 0;
  uint32_t handle_event_fn = 0;
  bool injected_simulated_download_complete = false;
  try {
    std::printf("Calling AEEMod_Load...\n");
    constexpr uint32_t kPpModAddr = 0x00090000;
    auto load_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, entry, shell, 0,
                                               kPpModAddr, 0, /*trace=*/false, /*hle_trace=*/false,
                                               &display, &backend);
    uint32_t module_ptr = mem.Read32(kPpModAddr);
    if (load_result.wandered_outside_module || load_result.exceeded_step_budget || !module_ptr) {
      std::printf("AEEMod_Load did not produce a trustworthy module pointer -- stopping.\n");
      return 1;
    }
    std::printf("AEEMod_Load OK, module=0x%08x\n", module_ptr);

    stage = "IModule::CreateInstance";
    uint32_t module_vtable = mem.Read32(module_ptr);
    uint32_t create_instance_fn = mem.Read32(module_vtable + 2 * 4);
    std::printf("Calling IModule::CreateInstance(ClsId=%u)...\n", cls_id);
    constexpr uint32_t kPpObjAddr = 0x00090010;
    auto create_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, create_instance_fn,
                                                 module_ptr, shell, cls_id, kPpObjAddr,
                                                 /*trace=*/false, /*hle_trace=*/false, &display, &backend);
    // *ppObj is the IApplet* itself, not a function pointer -- HandleEvent
    // is slot 2 of *its* vtable (AddRef=0, Release=1, HandleEvent=2, per
    // the real AEEAppGen.c reference source's IAppletVtbl init order).
    // Same double-indirection already used above for IModule::CreateInstance.
    applet_ptr = mem.Read32(kPpObjAddr);
    uint32_t applet_vtable = mem.Read32(applet_ptr);
    handle_event_fn = mem.Read32(applet_vtable + 2 * 4);
    if (create_result.wandered_outside_module || create_result.exceeded_step_budget ||
        !applet_ptr) {
      std::printf(
          "CreateInstance did not produce a trustworthy applet pointer -- stopping. "
          "(returned %u, *ppObj=0x%08x, wandered=%d, exceeded=%d)\n",
          create_result.r0, applet_ptr, create_result.wandered_outside_module,
          create_result.exceeded_step_budget);
      return 1;
    }
    std::printf("CreateInstance OK, applet=0x%08x HandleEvent=0x%08x\n", applet_ptr,
                handle_event_fn);
    // Real code reads/writes the third/fourth/fifth "app context"
    // fields (see mod_runtime.h) directly on the real IApplet instance
    // CreateInstance just returned, not on a separate fixed struct --
    // found tracing Peggle (TASKS.md Phase 8): its own real constructor
    // chain (peggle.mod 0x105890/0x107d0c/0x135468/0x10a8e0) runs with
    // `this` == this exact applet_ptr and stores real sub-objects into
    // its +0x24/+0x28/+0x2c fields. GetAppContext needs to expose this
    // same address from here on so later real reads of those fields see
    // what real code itself wrote, instead of an unrelated fixed block.
    mod_runtime.SetContextAddress(applet_ptr);

    // Real AEEAppStart layout, verified against the real AEEAppStart.h/
    // AEERect.h (NOT the same as our own hello_brew/hello_gl test
    // fixtures' simplified struct -- see PHASE8_LOG.md):
    //   int error; AEECLSID clsApp; IDisplay *pDisplay;
    //   struct { int16 x, y, dx, dy; } rc;  // NOT int -- half the size
    //   const char *pszArgs;                // a field our fixtures lack entirely
    constexpr uint32_t kAppStartAddr = 0x00090020;
    mem.Write32(kAppStartAddr + 0, 0);                                // error
    mem.Write32(kAppStartAddr + 4, cls_id);                           // clsApp
    mem.Write32(kAppStartAddr + 8, display_obj);                      // pDisplay
    mem.Write16(kAppStartAddr + 12, 0);                               // rc.x
    mem.Write16(kAppStartAddr + 14, 0);                               // rc.y
    mem.Write16(kAppStartAddr + 16, static_cast<uint16_t>(kWidth));   // rc.dx
    mem.Write16(kAppStartAddr + 18, static_cast<uint16_t>(kHeight));  // rc.dy
    mem.Write32(kAppStartAddr + 20, 0);                               // pszArgs

    stage = "HandleEvent(EVT_APP_START)";
    constexpr uint32_t kEvtAppStart = 0;  // real value, verified against AEEEvent.h
    std::printf("Calling HandleEvent(EVT_APP_START)...\n");
    // boolean HandleEvent(IApplet *po, AEEEvent evt, uint16 wParam, uint32 dwParam)
    auto handle_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, handle_event_fn,
                                                 applet_ptr, kEvtAppStart, 0, kAppStartAddr,
                                                 /*trace=*/false, /*hle_trace=*/false, &display, &backend);
    if (handle_result.wandered_outside_module || handle_result.exceeded_step_budget) {
      std::printf("HandleEvent(EVT_APP_START) did not complete trustworthily -- stopping.\n");
      return 1;
    }
    std::printf("HandleEvent(EVT_APP_START) returned %u\n", handle_result.r0);

    stage = "HandleEvent(EVT_APP_RESUME)";
    constexpr uint32_t kEvtAppResume = 3;
    std::printf("Calling HandleEvent(EVT_APP_RESUME)...\n");
    auto resume_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, handle_event_fn,
                                                 applet_ptr, kEvtAppResume, 0, kAppStartAddr,
                                                 /*trace=*/false, /*hle_trace=*/false, &display, &backend);
    if (resume_result.wandered_outside_module || resume_result.exceeded_step_budget) {
      std::printf("HandleEvent(EVT_APP_RESUME) did not complete trustworthily -- stopping.\n");
      return 1;
    }
    std::printf("HandleEvent(EVT_APP_RESUME) returned %u\n", resume_result.r0);
  } catch (const std::exception& e) {
    std::printf("%s threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n", stage, e.what(),
                cpu.GetRegister(zeebulator::kPC), cpu.GetRegister(zeebulator::kPC) - kBase);
    return 1;
  }

  // Real code inside CreateInstance/HandleEvent(EVT_APP_START) writes a
  // real zero to context[0x24]+0x45000+0x3d8 once, presumably its own
  // "not yet initialized" reset -- confirmed via a live memory
  // watchpoint (temporary, reverted). Writing our placeholder object
  // here, after that real reset instead of before it, is what makes it
  // stick for the real per-tick reads that follow.
  cpu.GetMemory().Write32(kFourthContextObject + 0x45000 + 0x3d8, unknown_arena_0x453d8_obj);
  // A sibling arena field, `+0x45000+0x3dc` (immediately after the one
  // above), found tracing why Peggle's steady-state per-tick loop never
  // varies (TASKS.md Phase 8): real code (`peggle.mod` offset
  // 0x132df0-0x132df4, called from the timer callback every single
  // tick) reads it and passes it, unconditionally and un-null-checked,
  // as `this` into a real subroutine (offset 0x109088) that immediately
  // dereferences it (`ldr r0,[r0,#4]`, `str r0,[r5,#4]`, `ldr
  // r0,[r5,#12]`, then `ldr r0,[r0]`). Left at 0 (this codebase's
  // default), that subroutine operates on a real null pointer every
  // tick -- our emulator's memory model tolerates that silently rather
  // than faulting, but it means every one of those accesses reads or
  // writes real, meaningful low addresses (0, 4, 0xc, ...) instead of
  // this field's own memory, which is real, evidenced address-0
  // pollution risk, not simulation of anything real. This struct's real
  // element layout (offset +4 looks like a call counter; +12 looks like
  // a pointer to a small, up-to-4-element array whose entries are read
  // at large offsets like +0xbc/+0xdc) is not understood well enough to
  // populate meaningfully -- likely Peggle's own internal per-tick game
  // data, not a generic BREW interface, and a materially bigger
  // reverse-engineering task than every other field fixed so far. So,
  // rather than guess at that real layout, this gets the same safe,
  // conservative treatment as the fourth field's own arena allocation
  // itself: a real, writable, zeroed memory block, just enough to stop
  // the real null-pointer accesses from landing on unrelated low
  // addresses. This does NOT change what the real subroutine does (a
  // zeroed block still reads as "empty" at every offset checked, so it
  // still takes the same do-nothing branch) -- it only isolates the
  // read/write pattern safely, and is not expected to unblock further
  // real progress on its own.
  constexpr uint32_t kArena0x3dcBlock = 0x80050000;
  cpu.GetMemory().Write32(kFourthContextObject + 0x45000 + 0x3dc, kArena0x3dcBlock);

  // Everything up to here is the same deterministic boot/setup sequence
  // on every real run -- any real texture it created (menu graphics,
  // ...) would be recreated identically by any other fresh launch too,
  // so it doesn't belong in a save state's own recorded log (see
  // GlTextureRecordingBackend::ClearLog's own doc comment on why
  // replaying it would actually break things, not just be redundant).
  // From here on, gl_recorder's log covers exactly the real texture
  // history an F1 save needs to persist.
  gl_recorder.ClearLog();

  if (auto_load_state) {
    std::ifstream state_in(save_state_path, std::ios::binary);
    bool ok = state_in && zeebulator::LoadState(cpu, state_in);
    // Continues reading the same stream right where LoadState left off
    // (see F1's own comment on why this is written right after the
    // CPU/memory data in the same file) -- replayed through gl_recorder,
    // not backend directly, so its own log ends up populated too (a
    // later F1 save after this load needs the replayed textures in its
    // own recorded history, not just whatever real gameplay creates
    // from here on).
    std::vector<zeebulator::GlTextureLogEntry> gl_log;
    bool gl_ok = ok && zeebulator::DeserializeGlTextureLog(state_in, gl_log) &&
                 zeebulator::ReplayGlTextureLog(gl_log, gl_recorder);
    // Best-effort, not gated into `ok`/`gl_ok`: an older save state made
    // before this section existed simply has no more bytes here, which
    // Deserialize already treats as "nothing to restore" rather than
    // corrupting anything (see Mixer::Serialize's own doc comment) --
    // real audio just stays silent for handles the guest reuses from
    // before the save, exactly the pre-existing gap, not a new failure.
    bool audio_ok = gl_ok && mixer.Deserialize(state_in) && media_hle.Deserialize(state_in);
    std::printf("--load-state: %s %s (GL texture replay: %s, audio state: %s)\n",
                ok ? "loaded" : "FAILED to load", save_state_path.c_str(),
                gl_ok ? "ok" : "FAILED", audio_ok ? "restored" : "not present in this save file");
    backend.ShowStatusMessage(ok && gl_ok ? "STATE LOADED" : "LOAD FAILED");
  }

  // Shared tail of the real HID button-event injection path (see
  // SdlKeyToHidButton's own doc comment) -- feeds `hid_button_uid`'s
  // press/release into the real HID/gamepad mechanism the same way a
  // real fired ISignal would, regardless of which real input source
  // (keyboard event or, below, a polled ZPadState edge) it came from.
  auto InjectHidButtonEvent = [&](uint32_t hid_button_uid, bool pressed) {
    int state = pressed ? 1 : 0;
    // nButtonID (first field) is a don't-care: the real callback's own
    // translation function overwrites it from nButtonUID (see
    // SdlKeyToHidButton's doc comment) before ever reading it back.
    simulated_button_events->push_back({0, state, static_cast<int32_t>(hid_button_uid)});
    // Real-evidenced re-arm, not optional: `*captured_button_context`
    // (the real per-device struct real code passes as `pUser`) has its
    // own first field (offset 0) read by the real translator function
    // (`0x100740`) as a pointer back to the real device object (its
    // vtable slot 9, byte offset 0x24, resolves to a real
    // `GetNextButtonEvent`-shaped trap) -- and real code *clears that
    // field to 0* as part of its own real cleanup once a full
    // press+release cycle finishes (confirmed live via a temporary
    // write-watch, PHASE8_LOG.md: real PCs `ddragonz.mod`
    // 0x10ada4/0x10adb8, inside 0x100740 itself). Nothing re-populates
    // it afterward, because on real hardware that's presumably
    // firmware's job when delivering a genuine new signal -- a step
    // this simulated injection has to do itself, or every button press
    // after the very first press+release cycle null-pointer-crashes the
    // real callback (confirmed live: this is what was happening, for
    // *any* button, not one specific direction).
    cpu.GetMemory().Write32(*captured_button_context, kHidDeviceObject);
    try {
      auto cb_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size,
                                               *captured_button_callback, *captured_button_context,
                                               0, 0, 0,
                                               /*trace=*/false, /*hle_trace=*/false, &display,
                                               &backend);
      std::printf("HID button callback(uid=0x%x, state=%d) ran%s\n", hid_button_uid, state,
                  cb_result.wandered_outside_module ? " (wandered!)" : "");
    } catch (const std::exception& e) {
      std::printf("HID button callback threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n",
                  e.what(), cpu.GetRegister(zeebulator::kPC),
                  cpu.GetRegister(zeebulator::kPC) - kBase);
    }
  };

  std::printf("Reached the event loop with no unhandled instruction! Window will stay open.\n");
  bool running = true;
  zeebulator::ZPadState previous_pad_state;
  SDL_Event event;
  constexpr uint32_t kTickMs = 16;
  uint64_t tick_count = 0;
  while (running) {
    uint32_t loop_start_ms = SDL_GetTicks();
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      // Frontend-only hotkeys (TASKS_TOOLING.md Phase A/D) -- F-keys are
      // never forwarded to the guest by either SdlKeyToAvk or
      // SdlKeyToHidButton below, so these can't collide with any real
      // game input. Consumed here (never falls through to the guest
      // forwarding logic) on key-down only, matching a real button
      // press rather than firing twice per physical press.
      if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        int scale = 0;
        switch (event.key.keysym.sym) {
          case SDLK_F5: scale = 1; break;
          case SDLK_F6: scale = 2; break;
          case SDLK_F7: scale = 3; break;
          case SDLK_F8: scale = 4; break;
          default: break;
        }
        if (scale != 0) {
          backend.SetWindowScale(scale);
          char status[16];
          std::snprintf(status, sizeof(status), "SCALE %dX", scale);
          backend.ShowStatusMessage(status);
          continue;
        }
        if (event.key.keysym.sym == SDLK_F9) {
          backend.SetOverlayVisible(!backend.OverlayVisible());
          continue;
        }
        if (event.key.keysym.sym == SDLK_F11) {
          // Desktop (borderless) fullscreen, not exclusive -- avoids a
          // real display-mode switch, matching what the letterboxed
          // blit (Sdl2UnifiedBackend::PresentFrame) already handles
          // cleanly for any real drawable size, fullscreen included.
          bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
          SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
          backend.ShowStatusMessage(is_fullscreen ? "WINDOWED" : "FULLSCREEN");
          continue;
        }
        // Save states (TASKS_TOOLING.md Phase B). F1 writes CPU
        // registers/CPSR + full guest memory (save_state.h), then the
        // real GL texture upload log recorded so far (gl_texture_log.h)
        // appended to the same file -- the latter is what lets
        // `--load-state` (below main()'s own event loop, see its own
        // comment) restore correctly from a *cold* launch, where the
        // real gameplay that would normally recreate those textures
        // never ran. F2 (same-session interactive load) only restores
        // CPU/memory, deliberately -- the live session's own GL context
        // already has the right textures, so replaying would just
        // create redundant duplicates.
        if (event.key.keysym.sym == SDLK_F1) {
          // Compacted before every save, not just periodically in the
          // tick loop below: guarantees a save taken moments after a
          // long play session started (before the periodic compaction
          // has run even once) still writes a bounded file instead of
          // the whole session's own raw history (see
          // CompactGlTextureLog's own doc comment -- a real 57-minute
          // session's own uncompacted save reached 470MB).
          gl_recorder.CompactLog();
          std::ofstream out(save_state_path, std::ios::binary);
          // Mixer/MediaHle state appended last, same reasoning as the GL
          // texture log: only a cold `--load-state` load actually needs
          // it (see Mixer::Serialize's own doc comment for why, and F2's
          // own comment below for why a same-session load doesn't).
          bool ok = out && zeebulator::SaveState(cpu, out) &&
                    zeebulator::SerializeGlTextureLog(gl_recorder.Log(), out) &&
                    mixer.Serialize(out) && media_hle.Serialize(out);
          backend.ShowStatusMessage(ok ? "STATE SAVED" : "SAVE FAILED");
          continue;
        }
        if (event.key.keysym.sym == SDLK_F2) {
          std::ifstream in(save_state_path, std::ios::binary);
          bool ok = in && zeebulator::LoadState(cpu, in);
          backend.ShowStatusMessage(ok ? "STATE LOADED" : "LOAD FAILED");
          continue;
        }
      }
      if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !event.key.repeat) {
        uint32_t hid_button_uid = SdlKeyToHidButton(event.key.keysym.sym);
        // The classic AVK path only runs for keys with *no* real HID
        // mapping. SdlKeyToAvk's own doc comment already says its
        // codes are invented, "not a claimed-correct real key
        // mapping" -- and this project has direct, live proof they
        // can be actively harmful, not just ineffective: Right's made-
        // up code (0xe02e) makes real HandleEvent code jump through a
        // null function pointer, which wanders hundreds of steps
        // through low, mostly-zero memory before crashing (caught,
        // logged, doesn't halt the loop) -- but during that wander,
        // real non-zero bytes elsewhere in that low range get
        // misinterpreted as real instructions, some of which are real
        // stores, and it corrupts whatever real per-frame sprite/HUD
        // draw loop reads its active-entity list from: every real
        // texture bind+draw call for every sprite/HUD element stops
        // happening, permanently, immediately after (confirmed live,
        // PHASE8_LOG.md -- only the background tilemap keeps
        // rendering). Since every key this project maps has a real,
        // confirmed-correct HID equivalent already, there's no reason
        // to keep taking that risk here.
        uint32_t avk = (hid_button_uid == 0) ? SdlKeyToAvk(event.key.keysym.sym) : 0;
        if (avk != 0 && applet_ptr != 0) {
          // boolean HandleEvent(IApplet *po, AEEEvent evt, uint16 wParam, uint32 dwParam)
          // evt 0x101/0x102 confirmed via real disassembly of Double
          // Dragon's own event dispatcher -- see SdlKeyToAvk's comment
          // and PHASE8_LOG.md.
          constexpr uint32_t kEvtKeyDown = 0x101;
          constexpr uint32_t kEvtKeyUp = 0x102;
          uint32_t evt = (event.type == SDL_KEYDOWN) ? kEvtKeyDown : kEvtKeyUp;
          try {
            auto key_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size,
                                                      handle_event_fn, applet_ptr, evt, avk, 0,
                                                      /*trace=*/false, /*hle_trace=*/false, &display, &backend);
            std::printf("HandleEvent(evt=0x%x, wParam=0x%x) returned %u%s\n", evt, avk,
                        key_result.r0,
                        key_result.wandered_outside_module ? " (wandered!)" : "");
          } catch (const std::exception& e) {
            std::printf("key event threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n", e.what(),
                        cpu.GetRegister(zeebulator::kPC), cpu.GetRegister(zeebulator::kPC) - kBase);
          }
        }
        // Second, separate real input path (see SdlKeyToHidButton's own
        // doc comment): feed the same real keypress into the real HID
        // button-event mechanism too, the same way a real fired
        // ISignal would -- push the event for GetNextButtonEvent to
        // hand out, then invoke the real registered callback directly
        // (real IDLECBFUNC signature: void (*)(void *pUser), confirmed
        // by this callback's own real disassembly taking exactly one
        // incoming argument).
        if (hid_button_uid != 0 && *captured_button_callback != 0) {
          InjectHidButtonEvent(hid_button_uid, event.type == SDL_KEYDOWN);
        }
      }
    }
    // Real gamepad input (TASKS_TOOLING.md Phase C) -- polled once per
    // tick (PollController's real SDL_GameController state is level-
    // triggered, unlike the event-driven keyboard path above) and diffed
    // against the previous tick's state to get the same shape of
    // press/release edges, each fed into the exact same HID injection
    // path keyboard uses via ZPadButtonToHidUid's mirrored mapping. Only
    // runs with a real controller connected (HasController()) so it
    // doesn't also pick up Sdl2UnifiedBackend::PollInput's own separate
    // keyboard-fallback scheme (different keys than SdlKeyToHidButton's
    // own) alongside the real keyboard handling above -- both a real
    // keyboard and a real controller still work simultaneously this way,
    // just never both driven off the same polled ZPadState.
    if (backend.HasController() && *captured_button_callback != 0) {
      zeebulator::ZPadState pad_state = zeebulator::NormalizeZPadState(backend.PollInput());
      for (const zeebulator::ZPadButtonEdge& edge :
           zeebulator::DiffZPadButtonEdges(previous_pad_state.buttons, pad_state.buttons)) {
        uint32_t hid_button_uid = ZPadButtonToHidUid(edge.button);
        if (hid_button_uid != 0) InjectHidButtonEvent(hid_button_uid, edge.pressed);
      }
      previous_pad_state = pad_state;
    }
    // Flushes real save-game writes (see userdata_path's own doc
    // comment above) to a real host file as soon as they happen, not
    // just on a clean exit -- matches how real flash storage commits a
    // real IFILE_Write immediately, and survives this tool being killed
    // rather than closed normally.
    if (file_hle.HasUnsavedWrites()) {
      std::ofstream userdata_out(userdata_path, std::ios::binary);
      if (userdata_out) file_hle.Serialize(userdata_out);
    }
    // Fires real MM_STATUS_DONE notifications for voices that finished
    // since the last tick -- see MediaHle::Tick's own doc comment; real
    // Double Dragon sound-channel bookkeeping depends on this firing.
    media_hle.Tick();
    // Real BREW timers are one-shot -- real game code re-arms its own via
    // ISHELL_SetTimer from inside the callback (see core/brew/ishell.h).
    // Driving these is what actually runs the game's per-frame logic;
    // nothing calls into the module otherwise from here on.
    mod_runtime.Tick(kTickMs);
    // Simulate a truthful "download/install 100% complete" notification
    // once the real callback has been registered (see the class-0x01005511
    // doc comment above) -- a real event struct shape confirmed via
    // disassembly of the real registered callback (`ddragonz.mod`
    // `0x11d020`): only two fields are read, `+8` (must equal 4) and
    // `+16` (must be 2 or 3; both route to the same real success path).
    // One-shot, not held -- this models a discrete real notification, not
    // continuous input state. Confirmed live (PHASE8_LOG.md) that this
    // reaches the real success-path *check* (`0x11f4dc`) without wandering
    // or throwing, but doesn't yet clear it: that check also requires a
    // real byte at `pUser+37` to be nonzero, and nothing this project has
    // triggered so far ever sets it. Kept as a real, evidence-grounded
    // building block for whoever traces that next -- not yet sufficient
    // on its own.
    if (!injected_simulated_download_complete && *captured_download_callback != 0 &&
        tick_count >= 30) {
      injected_simulated_download_complete = true;
      constexpr uint32_t kSimulatedEventStructAddr = 0x80066000;
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 8, 4);
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 16, 2);
      std::printf("  [input] simulating a download-complete notification: invoking callback "
                  "0x%08x\n",
                  *captured_download_callback);
      try {
        CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, *captured_download_callback,
                               *captured_download_context, kSimulatedEventStructAddr, 0, 0,
                               /*trace=*/false, /*hle_trace=*/false, &display, &backend);
      } catch (const std::exception& e) {
        std::printf("  [input] download-complete callback threw: %s\n", e.what());
      }
    }
    // No automatic simulated button press here (a real, held simulated
    // press was tried in earlier rounds of this investigation, from a
    // single tick up through a sustained ~4-second/240-tick hold -- see
    // PHASE8_LOG.md). Removed for real reasons, confirmed directly on
    // the real desktop (TASKS.md Phase 8): the real, correct BREW
    // title-screen transition out of the loading state happens on its
    // own, with zero simulated input at all -- the earlier assumption
    // that it needed a simulated press was wrong, and any sustained
    // simulated hold (even a brief ~250ms/16-tick one) visibly raced the
    // game through several distinct further real screens in one
    // uncontrolled burst (each repeated per-tick "press" event read as a
    // separate, discrete confirm by real game code, not a single sustained
    // hold), never leaving any one of them observable. With real
    // rendering now working (Sdl2UnifiedBackend), the better default is
    // to let the tool settle on whatever real, stable state the game
    // reaches on its own and leave further navigation to a real human's
    // own real keyboard/controller input (already wired up separately,
    // see PollInput/SdlKeyToAvk) rather than an automated guess at how
    // hard or how long to simulate a press. `simulated_button_events`
    // (populated by nothing now) and the real HID `GetNextButtonEvent`
    // plumbing draining it are kept intact as correct, real
    // infrastructure for whoever picks up real controller-driven
    // navigation next.
    for (const auto& timer : shell_hle.Tick(kTickMs)) {
      bool trace_this_tick = tick_count < 10 || persistent_log;
      if (trace_this_tick) std::printf("--- tick %llu ---\n", static_cast<unsigned long long>(tick_count));
      try {
        // Plain ISHELL_SetTimer-registered timers use the real,
        // already-validated-for-3-titles PFNNOTIFY(pUser in r0)
        // contract. Timers scheduled through IShellHle::ScheduleTimer's
        // own experimental path with a real captured `r0_override`
        // instead use that title's own real 2-argument shape -- see
        // that method's own doc comment for the live evidence.
        uint32_t call_r0 = timer.r0_override.value_or(timer.user_data);
        uint32_t call_r1 = timer.r0_override.has_value() ? timer.user_data : 0;
        auto tick_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, timer.callback,
                                                   call_r0, call_r1, 0, 0,
                                                   /*trace=*/false,
                                                   /*hle_trace=*/trace_this_tick, &display, &backend,
                                                   &abd_text_state);
        if (tick_result.wandered_outside_module || tick_result.exceeded_step_budget) {
          std::printf("timer callback did not complete trustworthily -- stopping.\n");
          running = false;
          break;
        }
      } catch (const std::exception& e) {
        std::printf("timer callback threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n",
                    e.what(), cpu.GetRegister(zeebulator::kPC),
                    cpu.GetRegister(zeebulator::kPC) - kBase);
        running = false;
        break;
      }
      ++tick_count;
      // Periodic maintenance, not just right before an F1 save: a long
      // real play session's own GL texture log otherwise grows without
      // bound for the rest of the process's lifetime (see
      // CompactGlTextureLog's own doc comment), which matters for a
      // live session's own memory use even between saves, not only for
      // save-state file size. Roughly once a real minute at this
      // title's own ~31fps real per-tick cadence (TASKS.md Phase 8);
      // cheap enough (one pass over the log) not to worry about
      // running it this often.
      if (tick_count % 1800 == 0) gl_recorder.CompactLog();
      // Presents right after *this* real timer's own real content was
      // drawn, not only once after the whole real due-timer burst
      // below finishes. `shell_hle.Tick(kTickMs)` can return more than
      // one real due, one-shot, self-rearming BREW timer per real
      // outer-loop iteration -- confirmed live this round for real
      // Alien Breaker Deluxe's own real boot sequence, which fires
      // several real timers back to back within a single real outer-
      // loop pass. Presenting only once after the full burst -- the
      // real, pre-existing behavior -- means every real timer in that
      // burst except the real last one never actually reaches the
      // real screen, even though each is its own real, complete,
      // presentable real frame. On real hardware each real timer fire
      // is its own real display moment; presenting once per real timer
      // here matches that instead of only ever showing the real *last*
      // timer in a real burst.
      if (!backend.HasRealGlActivity()) {
        display.RepresentLastFrame();
        if (abd_font_atlas.has_value()) display.PresentLiveFramebuffer();
      }
    }
    mixer.Mix(backend, static_cast<size_t>(kAudioSampleRate * kTickMs / 1000));
    // See IDisplayHle::RepresentLastFrame's own doc comment: keeps the
    // window actually showing whatever was last drawn even on ticks
    // where real app code doesn't call IDISPLAY_Update itself. The
    // in-loop check inside CallArmFunctionChecked covers long individual
    // real calls; this covers the (usually much smaller) gaps between
    // them.
    //
    // Deliberately unthrottled: tried rate-limiting this to ~5/sec
    // (TASKS.md Phase 8, real-desktop testing) on the theory that
    // swapping the same unchanged frame at ~60Hz was stressing the real
    // compositor -- confirmed, directly on the real desktop, to make a
    // separate real compositor quirk (occasional brief black flashes)
    // measurably *worse*, not better: less frequent presenting gave the
    // compositor more, not less, room to lose track of this window's
    // content, consistent with this file's much earlier finding that
    // this same desktop's compositor needs frequent real presents to
    // keep a window's content visible at all.
    //
    // Skipped entirely once the app has started real GL rendering --
    // see Sdl2UnifiedBackend::HasRealGlActivity's own doc comment: real
    // Double Dragon disassembly confirmed the app stops calling
    // IDISPLAY_Update once it does, so this would otherwise keep
    // re-presenting a stale snapshot on top of (and fighting for the
    // same drawable against) the app's own real, current GL content --
    // confirmed on the real desktop that real GL content never actually
    // became visible until this guard was added.
    if (!backend.HasRealGlActivity()) {
      display.RepresentLastFrame();
      // Alien Breaker Deluxe's own real font-atlas glyph bridge
      // (TASKS.md Phase 8) writes real pixels via `BlitRgba` outside
      // any real IDISPLAY_Update call, so `RepresentLastFrame` above
      // stays a permanent no-op for it -- confirmed live, a real SDL
      // window showed nothing but black despite `BlitRgba`
      // demonstrably writing correct real glyph pixels. Gated on
      // `abd_font_atlas` (only ever set for this one real title's own
      // real `data.bar`) so every other title's existing behavior --
      // and `RepresentLastFrame`'s own real "don't show mid-frame,
      // uncommitted content" guarantee -- stays untouched.
      if (abd_font_atlas.has_value()) display.PresentLiveFramebuffer();
    }
    if (std::getenv("ABD_HOLD_BUTTON2") != nullptr) {
      // Diagnostic-only, reused input-injection plumbing (see
      // ZEEBULATOR_AUTOPRESS's own doc comment below): rapidly toggles
      // Button2 every other tick, starting at tick 360, indefinitely --
      // reproduces the real repeated/rapid-Button2 crash (TASKS.md
      // Phase 8, `abd.mod` pc=0x00108d98) instead of relying on real
      // X11 key-repeat timing, which didn't reproduce it under
      // simulated input.
      constexpr uint64_t kStartTick = 360;
      if (tick_count >= kStartTick && *captured_button_callback != 0) {
        static uint64_t last_tick_acted = ~0ull;
        static bool state = false;
        if (tick_count != last_tick_acted) {
          state = !state;
          InjectHidButtonEvent(kHidUidButton2, state);
          last_tick_acted = tick_count;
        }
      }
    }
    if (std::getenv("ZEEBULATOR_AUTOPRESS")) {
      // Temporary, env-gated: no OS-level input-automation tool
      // available in this environment (no xdotool, no passwordless
      // sudo to install one) -- reuses this file's own already-correct
      // internal input-injection plumbing (the same paths a real
      // keypress goes through, same guards included) instead, so
      // bring-up investigations can probe "does pressing the confirm/
      // advance button do anything" without a human at the keyboard.
      // Tries both real input paths every ~2 real seconds, since a new
      // title's own real dispatch isn't known to depend on either one
      // specifically yet: the HID path (guarded on
      // `*captured_button_callback != 0` exactly like the real
      // keyboard handler above -- unguarded once already, live-caught
      // when it called through a still-unset, all-zero callback
      // pointer, that title's own real registration apparently not
      // having run yet) and the AVK path via `HandleEvent` directly
      // (always callable once `applet_ptr` exists, real evt/wParam
      // shape confirmed against Double Dragon).
      // Held, once each, not cyclic within the original candidate scan:
      // a repeated/rapid re-hold of one real button (specifically
      // Button2) used to reproduce two real crashes this project has
      // since found and fixed (ModRuntime table offsets 0x20 and 0xa8,
      // TASKS.md Phase 8) -- both real gaps are fixed now, so the extra
      // caution that originally limited this pass to "reach one real
      // screen past language-select, once" no longer applies. Extended
      // this round to keep going past the original candidate scan,
      // holding real Button2 (confirmed live to be this title's own
      // real menu-confirm button) a further `kExtraButton2Slots` times
      // with the same real pacing -- enough real presses, with real
      // margin, to carry a real run from language-select through the
      // top-level menu, the ARCADE/CHALLENGE/VERSUS/FRONTON submenu, and
      // real zone-select, into real gameplay, unattended. Paced by real
      // `tick_count` (real processed game ticks), not real outer-loop
      // iterations -- confirmed live this round that outer-loop pacing
      // drifts once this project's own added real per-tick decode/scale
      // work changes how many real game ticks land within a given real
      // wall-clock interval, silently misaligning a real loop-count-
      // paced press against real game state.
      static const uint32_t kCandidates[] = {kHidUidDPadDown,  kHidUidDPadUp,
                                              kHidUidDPadLeft,  kHidUidDPadRight,
                                              kHidUidBack,      kHidUidButton1,
                                              kHidUidButton2,   kHidUidButton3,
                                              kHidUidButton4};
      constexpr uint64_t kStartTick = 360;  // safely past the ~300-tick splash->language-select boundary
      constexpr uint64_t kHoldTicks = 10;
      constexpr uint64_t kGapTicks = 30;
      constexpr uint64_t kSlotTicks = kHoldTicks + kGapTicks;
      constexpr uint64_t kNumCandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);
      constexpr uint64_t kExtraButton2Slots = 8;
      constexpr uint64_t kTotalSlots = kNumCandidates + kExtraButton2Slots;
      if (tick_count >= kStartTick && tick_count < kStartTick + kSlotTicks * kTotalSlots) {
        uint64_t rel = tick_count - kStartTick;
        uint64_t which = rel / kSlotTicks;
        uint64_t pos = rel % kSlotTicks;
        uint32_t button = which < kNumCandidates ? kCandidates[which] : kHidUidButton2;
        static uint64_t last_pos_acted = ~0ull;
        if (pos != last_pos_acted && *captured_button_callback != 0) {
          if (pos == 0) {
            InjectHidButtonEvent(button, true);
            last_pos_acted = pos;
          } else if (pos == kHoldTicks) {
            InjectHidButtonEvent(button, false);
            last_pos_acted = pos;
          }
        }
      }
    }
    // Elapsed-aware, not flat: this loop feeds a fixed kTickMs of
    // *simulated* time into Tick() every iteration, but the real
    // wall-clock cost of a single iteration varies a lot -- most do
    // nothing, but the ones landing on a due real self-rearming
    // ISHELL_SetTimer (Double Dragon's own real main-loop timer,
    // confirmed live at a real, disassembly-grounded ms=32 request --
    // see PHASE8_LOG.md) run real ARM code that ends in a real,
    // vsync-blocked eglSwapBuffers. A flat, unconditional
    // `SDL_Delay(kTickMs)` after that (the previous behavior) stacks
    // a second real ~16ms wait on top of a real swap that already
    // spent real wall-clock time blocking on vsync, silently halving
    // real throughput versus the real 32ms/~31fps cadence the game
    // itself is asking for. Confirmed live (PHASE8_LOG.md): removing
    // that double-wait took this tool from ~27fps to matching the
    // real requested cadence.
    uint32_t elapsed_this_iter = SDL_GetTicks() - loop_start_ms;
    if (elapsed_this_iter < kTickMs) SDL_Delay(kTickMs - elapsed_this_iter);
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
