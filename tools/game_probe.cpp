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
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "core/audio/mixer.h"
#include "core/audio/soundfont_synth.h"
#include "core/brew/file_hle.h"
#include "core/brew/gl_hle.h"
#include "core/brew/soft_gl_backend.h"
#include "core/brew/idisplay.h"
#include "core/brew/interface_object.h"
#include "core/brew/ishell.h"
#include "core/brew/media_hle.h"
#include "core/brew/thread_hle.h"
#include "core/brew/heap_hle.h"
#include "core/brew/hash_hle.h"
#include "core/brew/mem_astream_hle.h"
#include "core/brew/unzip_stream_hle.h"
#include "core/brew/mod_runtime.h"
#include "core/brew/scaffold_object.h"
#include "core/brew/sql_hle.h"
#include "core/brew/compat/title_quirks.h"
#include "core/control/control_server.h"
#include "core/control/debug_hooks.h"
#include "core/control/mirror_server.h"
#include "core/brew/virtual_filesystem.h"
#include "core/cpu/arm_interpreter.h"
#include "core/cpu/dynarmic_arm_core.h"
#include "core/gl_texture_log.h"
#include "core/loader/atitc.h"
#include "core/loader/bmp.h"
#include "core/loader/gif.h"
#include "core/loader/png.h"
#include "core/loader/fufs.h"
#include "core/loader/sar.h"
#include "core/loader/ggz.h"
#include "core/loader/mod.h"
#include "core/loader/pakz.h"
#include "core/loader/pkg.h"
#include "core/save_state.h"
#include "frontends/standalone/sdl2_unified_backend.h"
#include "frontends/standalone/zpad_edges.h"
#include "core/brew/draw_stats.h"

namespace {

// Phase 9b/9d: per-slot NID labels for the ABD 200-slot rendering-engine
// scaffold (clsid family 0x0103d8ec, the object the tick-9 wall cycles
// on). Only the slots this project has evidence for are named; the rest
// fall through to the generic "ABD_RENDER_SCAFFOLD::slotN" form. These
// names come straight from tools/game_probe.cpp's own live-traced slot
// handlers and research/sources/2026-09-01_abd-bringup.md -- they are
// OBSERVED roles, not guesses.
inline std::vector<const char*> abd_scaffold_slot_names() {
  std::vector<const char*> names(200, nullptr);
  names[2] = "QueryInterface";           // mints child scaffolds
  names[33] = "SelectTexture/consume-cmdlist";  // wall consumer, cursor r2
  names[40] = "SetFillColor";
  names[48] = "BeginScreen";
  names[64] = "RegisterDescriptor/producer";    // wall producer candidate
  names[54] = "wall-cycle-slot54";       // in the 4-trap tick-9 cycle
  names[100] = "wall-cycle-slot100";     // in the 4-trap tick-9 cycle
  names[107] = "DrawGeometry";
  return names;
}

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

void MergeGamePkgInto(zeebulator::VirtualFilesystem& vfs, const char* path) {
  namespace fs = std::filesystem;
  std::vector<uint8_t> raw = ReadFile(path);
  auto archive = zeebulator::PkgArchive::Parse(raw);
  const std::string stem = fs::path(path).stem().string();
  const std::string pkg_name = stem + ".pkg";
  const std::vector<std::string> roots = {
      ".\\" + stem, "roms\\" + stem, "roms\\neogeo\\" + stem};
  vfs.AddFile(".\\" + pkg_name, raw);
  vfs.AddFile("roms\\" + pkg_name, raw);
  vfs.AddFile("roms\\neogeo\\" + pkg_name, raw);
  for (const auto& entry : archive.Entries()) {
    std::vector<uint8_t> bytes = archive.Extract(entry);
    for (const auto& root : roots) vfs.AddFile(root + "\\" + entry.name, bytes);
  }
  std::printf("loaded game pkg %s (%zu entries), stem=%s\n", path,
              archive.Entries().size(), stem.c_str());
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
  bool yielded = false;
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

CallResult CallArmFunctionChecked(zeebulator::IArmCore& cpu, uint32_t trap_base,
                                   uint32_t mod_base, uint32_t mod_size, uint32_t entry,
                                   uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                                   bool trace = false, bool hle_trace = false,
                                   zeebulator::IDisplayHle* display_for_liveness = nullptr,
                                   zeebulator::Sdl2UnifiedBackend* backend_for_liveness = nullptr,
                                   AbdTextState* abd_text_state = nullptr,
                                   bool resume = false,
                                   const std::function<bool()>& should_yield = {}) {
  // Default per-call step budget. Raised from 5M to 64M (2026-09-02): several
  // real titles (cninja/spinmast/strhoop) run a legitimate inline LZ asset
  // decompressor over their .pkg during the pre-resume timer drain that needs
  // ~40-50M interpreter steps on the first tick -- at 5M it aborted with the
  // misleading "timer callback did not complete trustworthily" even though it
  // was doing real, finite work (confirmed: at 60M cninja finishes the
  // decompressor, clears the drain, and reaches a later real blocker). A
  // genuinely stuck title still aborts, just after a larger bound. Override with
  // ZEEB_MAX_STEPS for tighter/looser probing.
  uint64_t kMaxSteps = 64'000'000;
  if (const char* budget = std::getenv("ZEEB_MAX_STEPS")) {
    uint64_t parsed = std::strtoull(budget, nullptr, 0);
    if (parsed > 0) kMaxSteps = parsed;
  }
  if (!resume) {
    cpu.SetRegister(zeebulator::kR0, r0);
    cpu.SetRegister(zeebulator::kR1, r1);
    cpu.SetRegister(zeebulator::kR2, r2);
    cpu.SetRegister(zeebulator::kR3, r3);
    cpu.SetRegister(zeebulator::kLR, trap_base);
    cpu.SetRegister(zeebulator::kPC, entry);
  }

  CallResult result;
  uint32_t last_in_module_pc = 0;
    bool warned_wander = false;
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
  // Env-gated spin profiler (ZEEB_SPIN_PROFILE=1): samples the guest PC into
  // a histogram and, on budget-exceed, prints the hottest PCs plus the
  // min/max PC of the last window -- names WHERE a non-terminating guest
  // loop spins, without perturbing execution (pure observation).
  const bool spin_profile = std::getenv("ZEEB_SPIN_PROFILE") != nullptr;
  // ZEEB_SEED_63C=guardpc:objoff:objreg:sentreg (hex/dec), e.g. 0x1034f8:0x14c:7:5
  // for cninja (guard reads obj=[r7,#0x14c], sentinel in r5). Diagnostic only.
  bool seed63c_active = false;
  uint32_t seed63c_guardpc = 0, seed63c_objoff = 0;
  int seed63c_objreg = 7, seed63c_sentreg = 5;
  bool seed63c_done = false;
  if (const char* sc = std::getenv("ZEEB_SEED_63C")) {
    unsigned gp = 0, off = 0, orr = 7, sr = 5;
    if (std::sscanf(sc, "%x:%x:%u:%u", &gp, &off, &orr, &sr) >= 2) {
      seed63c_active = true;
      seed63c_guardpc = gp;
      seed63c_objoff = off;
      seed63c_objreg = static_cast<int>(orr);
      seed63c_sentreg = static_cast<int>(sr);
    }
  }
  std::map<uint32_t, uint64_t> pc_hist;
  uint32_t win_lo = 0xffffffffu, win_hi = 0;
  uint32_t last_call_addr = 0;  // most recent BL target (in-module) before spin
  uint64_t trace_step_start = ~0ull;
  uint64_t trace_step_count = 50;
  if (const char* env_ts = std::getenv("ZEEB_TRACE_STEP")) {
    std::sscanf(env_ts, "%llu,%llu",
                reinterpret_cast<unsigned long long*>(&trace_step_start),
                reinterpret_cast<unsigned long long*>(&trace_step_count));
  }
  // Dynarmic only pays its block-compiler cost back through IArmCore::Run().
  // Keep instruction-by-instruction execution as the universal/debuggable default;
  // explicit ZEEB_JIT_BLOCK=1 opts into bounded blocks between HLE traps. Do not
  // batch when any per-instruction diagnostic/PC patch is armed.
  const bool jit_block_mode =
      dynamic_cast<zeebulator::DynarmicArmCore*>(&cpu) != nullptr &&
      std::getenv("ZEEB_JIT_BLOCK") != nullptr && !trace && !hle_trace &&
      !spin_profile && !seed63c_active && std::getenv("ZEEB_TRACE") == nullptr &&
      std::getenv("ZEEB_WWATCH") == nullptr && std::getenv("ZEEB_TRACE_STEP") == nullptr;
  constexpr uint64_t kJitBlockQuantum = 4096;
  // `steps` fora do for: precisa sobreviver ao laco para o relatorio abaixo.
  uint64_t steps = 0;
  for (; cpu.GetRegister(zeebulator::kPC) != trap_base; ++steps) {
    if (steps >= kMaxSteps) {
      std::printf("warning: exceeded %llu steps without returning -- aborting this call\n",
                  static_cast<unsigned long long>(kMaxSteps));
      result.exceeded_step_budget = true;
      if (spin_profile) {
        std::vector<std::pair<uint32_t, uint64_t>> top(pc_hist.begin(), pc_hist.end());
        std::sort(top.begin(), top.end(),
                  [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("  [spin] hottest guest PCs over last window (pc: hits, off=pc-modbase):\n");
        for (size_t i = 0; i < top.size() && i < 16; ++i) {
          std::printf("  [spin]   pc=0x%08x off=0x%08x hits=%llu\n", top[i].first,
                      top[i].first - mod_base,
                      static_cast<unsigned long long>(top[i].second));
        }
        std::printf("  [spin] loop PC span: 0x%08x-0x%08x (off 0x%08x-0x%08x), distinct=%zu\n",
                    win_lo, win_hi, win_lo - mod_base, win_hi - mod_base, pc_hist.size());
        std::printf("  [spin] last in-module BL target before spin: 0x%08x (off 0x%08x)\n",
                    last_call_addr, last_call_addr - mod_base);
      }
      break;
    }
    if (spin_profile) {
      uint32_t spc = cpu.GetRegister(zeebulator::kPC);
      // Env-gated entry watch: log the node struct + walker cursor at each
      // entry of the two wall functions (+0x5ba0 walker, +0x5ddc node loop),
      // first N times, to name why the traversal never terminates.
      static int watch_n = 0;
      if ((spc == mod_base + 0x5ba0 || spc == mod_base + 0x5ddc) &&
          steps + 400000 >= kMaxSteps && watch_n < 80) {
        ++watch_n;
        uint32_t r0v = cpu.GetRegister(zeebulator::kR0);
        uint32_t spv = cpu.GetRegister(zeebulator::kSP);
        uint32_t cnt = (r0v >= 0x80000000u) ? cpu.GetMemory().Read32(r0v + 8) : 0;
        auto& M = cpu.GetMemory();
        std::printf("  [wall] fn=+0x%05x r0=%08x [r0]=%08x [r0+4]=%08x [r0+8]=%08x [r0+c]=%08x [r0+10]=%08x [r0+14]=%08x sp=%08x lr=%08x\n",
                    spc - mod_base, r0v, M.Read32(r0v), M.Read32(r0v + 4), cnt,
                    M.Read32(r0v + 0xc), M.Read32(r0v + 0x10), M.Read32(r0v + 0x14),
                    spv, cpu.GetRegister(zeebulator::kLR));
      }
      // Only profile the tail of the run (the actual spin), keep it bounded.
      if (steps + 200000 >= kMaxSteps) {
        pc_hist[spc]++;
        if (spc < win_lo) win_lo = spc;
        if (spc > win_hi) win_hi = spc;
      }
      if (spc >= mod_base && spc < mod_base + mod_size) {
        uint32_t instr = cpu.GetMemory().Read32(spc);
        if ((instr & 0x0f000000u) == 0x0b000000u) {  // BL
          int32_t off = (instr & 0x00ffffffu);
          if (off & 0x00800000) off |= 0xff000000;
          last_call_addr = spc + 8 + (off << 2);
        }
      }
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
    // EXPERIMENT (ZEEB_SEED_63C=guardpc:objreg:sentinelreg): at the +0x63c
    // guard, if the object's +0x63c callback slot reads back as 0 (impossible
    // on real hardware -- the guard tests !=sentinel, not !=0, so 0 means our
    // register-step never ran), seed the SENTINEL there so the guard's beq is
    // taken and the (evidently optional) callback is skipped instead of
    // executing blx 0. Purely diagnostic: answers "is this callback optional
    // (game proceeds) or required (stalls elsewhere)?". Gated + logged; NOT a
    // shipped fix. cninja: 0x1034f8:0x14c:5 (obj=[r7,#0x14c], sentinel=r5).
    if (seed63c_active && pc == seed63c_guardpc) {
      uint32_t base = cpu.GetRegister(seed63c_objreg);
      uint32_t real_obj = cpu.GetMemory().Read32(base + seed63c_objoff);
      if (real_obj >= 0x1000) {
        uint32_t slot = cpu.GetMemory().Read32(real_obj + 0x63c);
        if (slot == 0) {
          uint32_t sentinel = cpu.GetRegister(seed63c_sentreg);
          cpu.GetMemory().Write32(real_obj + 0x63c, sentinel);
          if (!seed63c_done) {
            std::printf("[seed63c] obj=0x%08x +0x63c was 0 -> seeded sentinel 0x%08x\n",
                        real_obj, sentinel);
            seed63c_done = true;
          }
        }
      }
    }
    bool in_module = pc >= mod_base && pc < mod_base + mod_size;
    bool in_trap_range = pc >= trap_base;
    if (in_module) {
      last_in_module_pc = pc;
      last_lr = cpu.GetRegister(zeebulator::kLR);
    }
    bool step_trace = (steps >= trace_step_start && steps < trace_step_start + trace_step_count);
    if (trace || step_trace) {
      std::printf("[%4llu] pc=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n",
                  static_cast<unsigned long long>(steps), pc,
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3),
                  cpu.GetRegister(zeebulator::kSP), cpu.GetRegister(zeebulator::kLR));
    }
    if (hle_trace && in_trap_range && pc != trap_base) {
      std::printf("  [hle call] trap=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", pc,
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3));
    }

    // A PC outside the module is not automatically fatal. The zero page is
    // mapped to `bx lr` on purpose (see the landing pad near main()'s top), so
    // guest code that calls through a NULL function pointer returns to its own
    // caller and keeps running -- which is what the console does when an
    // optional asset is absent. Example: Z-Wheel opens `fontsize.map`, that
    // file exists nowhere in the NAND, the returned handle is dereferenced
    // unguarded, and control resumes at the instruction after the call.
    //
    // Keep the observation, but make it recoverable: only report a wander as
    // fatal if the guest does NOT come back to the module before the call ends.
    // A real runaway still fails, because it never returns and hits kMaxSteps.
    if (in_module) {
      if (warned_wander) {
        std::printf(
            "note: pc returned to the module at 0x%08x after the out-of-range excursion\n", pc);
        warned_wander = false;
      }
      result.wandered_outside_module = false;
    } else if (!in_trap_range && !warned_wander) {
      std::printf(
          "warning: pc=0x%08x left the loaded module's range (0x%08x-0x%08x) after %llu "
          "steps -- likely a missing loader/runtime-support gap, not real progress (see "
          "PHASE8_LOG.md). Last in-module pc=0x%08x lr=0x%08x -- disassemble there first.\n",
          pc, mod_base, mod_base + mod_size, static_cast<unsigned long long>(steps),
          last_in_module_pc, last_lr);
      if (std::getenv("ZEEB_LOG_OOR") != nullptr) {
        // Diagnostico temporario: qual registrador carregava o destino ruim e o
        // que havia no objeto apontado por ele. Sem isto o aviso so diz ONDE
        // saltou, nao COM QUE PONTEIRO -- e a diferenca entre "o guest tem um
        // ponteiro errado" e "nosso objeto foi sobrescrito".
        std::fprintf(stderr, "[oor] pc=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n", pc,
                     cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                     cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3));
        for (int rn = 4; rn <= 12; ++rn) {
          std::fprintf(stderr, "  r%d=0x%08x", rn,
                       cpu.GetRegister(static_cast<zeebulator::ArmRegister>(
                           static_cast<int>(zeebulator::kR0) + rn)));
        }
        std::fprintf(stderr, "  sp=0x%08x lr=0x%08x\n", cpu.GetRegister(zeebulator::kSP),
                     cpu.GetRegister(zeebulator::kLR));
        const uint32_t r7 = cpu.GetRegister(static_cast<zeebulator::ArmRegister>(
            static_cast<int>(zeebulator::kR0) + 7));
        if (r7 >= 0x1000) {
          std::fprintf(stderr, "[oor] [r7]=0x%08x [r7+0]=0x%08x\n",
                       cpu.GetMemory().Read32(r7), cpu.GetMemory().Read32(r7));
          const uint32_t v0 = cpu.GetMemory().Read32(r7);
          if (v0 >= 0x1000) {
            std::fprintf(stderr, "[oor] [[r7]]=0x%08x (vt[0])\n", cpu.GetMemory().Read32(v0));
          }
        }
      }
      result.wandered_outside_module = true;
      warned_wander = true;
    }
    uint64_t retired = 1;
    if (jit_block_mode) {
      const uint64_t remaining = kMaxSteps - steps;
      retired = cpu.Run(std::min(kJitBlockQuantum, remaining));
      // A backend is not allowed to report zero progress here, but retain the
      // single-step escape hatch so an unusual Dynarmic stop cannot spin host-side.
      if (retired == 0) {
        cpu.Step();
        retired = 1;
      }
      // The `for` increment accounts for one; consume the rest of this bounded block.
      steps += retired - 1;
    } else {
      cpu.Step();
    }
    if (should_yield && should_yield()) {
      result.yielded = true;
      break;
    }
  }
  result.r0 = cpu.GetRegister(zeebulator::kR0);
  // Quantas instrucoes do guest esta chamada consumiu. Sem este numero,
  // "estourou o orcamento" e "estourou por pouco" ficam indistinguiveis -- e e
  // essa diferenca que decide se o orcamento padrao esta apertado ou se o
  // titulo esta realmente travado. Ver kMaxSteps (64M hoje; ja foi subido de
  // 5M em 35dc8df pelo mesmo motivo, copia/descompressao legitima de asset).
  if (std::getenv("ZEEB_LOG_STEPS") != nullptr) {
    std::fprintf(stderr, "[steps] chamada consumiu %llu instrucoes do guest%s\n",
                 static_cast<unsigned long long>(steps),
                 result.exceeded_step_budget ? " (ESTOUROU)" : "");
  }
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
// Maps SDL keys to real Qualcomm BREW virtual key codes (AEEVCodes.h).
// Official Qualcomm SDK standard:
//   AVK_0..AVK_9 = 0xE021..0xE02A
//   AVK_CLR      = 0xE030 (Escape / Backspace)
//   AVK_UP       = 0xE031 (Up arrow / W)
//   AVK_DOWN     = 0xE032 (Down arrow / S)
//   AVK_LEFT     = 0xE033 (Left arrow / A)
//   AVK_RIGHT    = 0xE034 (Right arrow / D)
//   AVK_SELECT   = 0xE035 (Enter / Space)
//   AVK_SOFT1    = 0xE036
//   AVK_SOFT2    = 0xE037
uint32_t SdlKeyToAvk(SDL_Keycode key) {
  if (key >= SDLK_0 && key <= SDLK_9) {
    return 0xE021 + static_cast<uint32_t>(key - SDLK_0);
  }
  switch (key) {
    case SDLK_UP: case SDLK_w: return 0xE031;        // AVK_UP
    case SDLK_DOWN: case SDLK_s: return 0xE032;      // AVK_DOWN
    case SDLK_LEFT: case SDLK_a: return 0xE033;      // AVK_LEFT
    case SDLK_RIGHT: case SDLK_d: return 0xE034;     // AVK_RIGHT
    case SDLK_RETURN: case SDLK_SPACE: return 0xE035;// AVK_SELECT
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return 0xE030; // AVK_CLR
    default: return 0;
  }
}

// Maps a subset of SDL keys to real HID `nButtonUID` values, for the
// *other* real input path this codebase has wired up but never fed
// live input into: the real HID/gamepad button-event mechanism
// (`hid_device_methods[9]`/registered ISignal below), separate
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
// Exact enumeration order from Zeebo hid_devices.cfg, retained with its
// documented duplicate/mislabelled entries. Games identify controls by UID.
constexpr std::array<uint32_t, 18> kHidButtonUids = {
    kHidUidButton2, kHidUidRightShoulderUpper, kHidUidButton4, 0x0106c4d0,
    kHidUidRightShoulderUpper, 0x0106c407, kHidUidLeftShoulderUpper, 0x0106c409,
    0x0106c405, kHidUidBack, 0x0106c404, 0x0106c402,
    kHidUidDPadUp, kHidUidDPadDown, kHidUidDPadLeft, kHidUidDPadRight,
    kHidUidButton1, kHidUidButton3,
};
constexpr uint32_t kHidJoystickDeviceUid = 0x0106c3fd;

uint32_t SdlKeyToHidButton(SDL_Keycode key) {
  switch (key) {
    case SDLK_UP: case SDLK_w: return kHidUidDPadUp;
    case SDLK_DOWN: case SDLK_s: return kHidUidDPadDown;
    case SDLK_LEFT: case SDLK_a: return kHidUidDPadLeft;
    case SDLK_RIGHT: case SDLK_d: return kHidUidDPadRight;
    case SDLK_BACKSPACE: case SDLK_RETURN: case SDLK_SPACE: return kHidUidBack;
    case SDLK_q: case SDLK_1: return kHidUidLeftShoulderUpper;
    case SDLK_e: case SDLK_2: return kHidUidRightShoulderUpper;
    case SDLK_z: case SDLK_j: return kHidUidButton1;
    case SDLK_x: case SDLK_k: return kHidUidButton2;
    case SDLK_c: case SDLK_u: return kHidUidButton3;
    case SDLK_v: case SDLK_i: return kHidUidButton4;
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
  std::vector<std::string> bar_paths;  // ABD-style titles ship a data.bar, no ggz
  std::vector<std::string> pkg_paths;  // arcade ports ship per-game compressed ROM sets
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
      // ABD (and any BAR-only title): register a real `.bar` resource
      // archive's raw bytes under its own basename in the VFS. The game
      // opens it by name via ISHELL_LoadResDataEx (see core/loader/bar.h),
      // exactly like MergeGgzInto's final AddFile does for a ggz -- no
      // per-entry extraction is needed for the file-open path. Repeatable.
      if (std::string(argv[read_i]) == "--bar") {
        if (read_i + 1 >= argc) {
          std::fprintf(stderr, "--bar needs a file path\n");
          return 1;
        }
        bar_paths.emplace_back(argv[++read_i]);
        continue;
      }
      if (std::string(argv[read_i]) == "--pkg") {
        if (read_i + 1 >= argc) {
          std::fprintf(stderr, "--pkg needs a file path\n");
          return 1;
        }
        pkg_paths.emplace_back(argv[++read_i]);
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
                  "usage: %s <game.mod> <data.ggz|-> <sound.ggz|-> <cls_id_decimal> [boot.pkg] "
                  "[resources.bar] [--bar <file>] [--load-state] [--persistent-log]\n"
                  "  Pass '-' for a ggz slot a title doesn't ship (e.g. ABD, which is\n"
                  "  BAR-only: %s abd.mod - - 16975901 --bar data.bar).\n",
                  argv[0], argv[0]);
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
    // Same rule as save data: never write beside the ROM (see data_dir below).
    std::string playlog_path = [&]() -> std::string {
      if (const char* forced = std::getenv("ZEEB_DATA_DIR")) return std::string(forced) + "/" + BaseName(argv[1]) + ".playlog";
      if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/.local/share/zeebulator/" + BaseName(argv[1]) + ".playlog";
      return std::string(argv[1]) + ".playlog";
    }();
    {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(playlog_path).parent_path(), ec);
    }
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
  // Player data does not belong next to the ROM. The corpus lives on removable,
  // often read-only media, and writing there polluted it: a static audit of the
  // NAND dump counted our own `.userdata` files as if they were shipped game
  // content. Default to the XDG data directory, keep reading a legacy file that
  // an older build left beside the module, and allow ZEEB_DATA_DIR to override.
  const std::string module_key = BaseName(argv[1]);
  const std::string data_dir = [&]() -> std::string {
    if (const char* forced = std::getenv("ZEEB_DATA_DIR")) return forced;
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) return std::string(xdg) + "/zeebulator";
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/.local/share/zeebulator";
    // Ambiente sem HOME/XDG: ainda nao escreva no cwd, que pode ser a ROM.
    return (std::filesystem::temp_directory_path() / "zeebulator").string();
  }();
  {
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
  }
  auto pick_load_source = [](const std::string& preferred, const std::string& legacy) {
    std::error_code ec;
    if (!std::filesystem::exists(preferred, ec) && std::filesystem::exists(legacy, ec)) return legacy;
    return preferred;
  };
  // Destino e origem sao separados de proposito: legado ao lado da ROM pode
  // ser IMPORTADO, nunca continuar sendo o destino de escrita.
  const std::string save_state_path = data_dir + "/" + module_key + ".savestate";
  const std::string save_state_load_path =
      pick_load_source(save_state_path, std::string(argv[1]) + ".savestate");
  // Real save-game data (Double Dragon's own "./udata/ddz.sav", written
  // through FileHle's writable_files_ -- see file_hle.h) is a genuinely
  // separate concern from the save STATE above: a player's actual
  // in-game progress/unlocks, meant to survive every cold relaunch
  // unconditionally, not just resumed from a chosen moment. Without
  // this, writable_files_ was purely in-memory and silently reset to
  // empty on every process exit -- indistinguishable from the game "not
  // saving" at all (a real, live-reported bug this fixes).
  const std::string userdata_path = data_dir + "/" + module_key + ".userdata";
  const std::string userdata_load_path =
      pick_load_source(userdata_path, std::string(argv[1]) + ".userdata");

  zeebulator::VirtualFilesystem vfs;
  // A title that doesn't ship a given ggz passes '-' for that slot (ABD is
  // BAR-only). Skip the merge rather than aborting in GgzArchive::Parse.
  // Um arquivo de assets que NAO e um ggz nao pode derrubar o emulador. Medido:
  // passar mod/274214/data.vfs (FUFS, container do Crash Nitro Kart 2 que este
  // projeto ainda nao parseia) fazia GgzArchive::Parse lancar
  // "GGZ: invalid table length", ninguem capturava, e o processo abortava com
  // core dump antes mesmo de carregar o modulo -- comportamento pior que o do
  // caminho BAR/PAKZ, que ja pula com uma mensagem clara.
  //
  // Agora a falha e contida e os bytes crus entram no VFS sob o proprio nome do
  // arquivo, exatamente como MergeGgzInto faz no fim para um ggz valido: um
  // jogo que abra o container por conta propria ainda o encontra, e quem
  // depende do indice descobre a ausencia pelo log em vez de por um sinal.
  //
  // Containers FUFS (".vfs") montados nesta execucao. A tabela do FUFS guarda
  // HASH de nome, nao texto: dois dos seis .vfs reais (cnk2 274214 e 277229)
  // nao trazem lista de nomes nenhuma. Por isso o container fica vivo aqui e e
  // consultado por hash no resolvedor de falta do VFS mais abaixo -- e assim
  // que um caminho pedido por string via IFILEMGR chega ao payload certo mesmo
  // sem nome gravado. Ver core/loader/fufs.h.
  std::vector<std::shared_ptr<zeebulator::FufsArchive>> fufs_archives;
  auto merge_asset_arg = [&vfs, &fufs_archives](const char* path) {
    if (std::string(path) == "-") return;
    try {
      MergeGgzInto(vfs, path);
    } catch (const std::exception& e) {
      std::printf("skipped %s: nao e um GGZ parseavel (%s); bytes crus ainda no "
                  "VFS como %s\n", path, e.what(), BaseName(path).c_str());
      std::vector<uint8_t> raw;
      try {
        raw = ReadFile(path);
      } catch (const std::exception& inner) {
        std::printf("  e nem os bytes crus puderam ser lidos: %s\n", inner.what());
        return;
      }
      // Antes de desistir do indice, tenta o FUFS. Parse devolve optional e
      // nao lanca -- um container desconhecido nao pode derrubar o processo.
      if (auto archive = zeebulator::FufsArchive::Parse(raw)) {
        size_t named = 0;
        for (const auto& entry : archive->Entries()) {
          if (entry.name.empty()) continue;
          vfs.AddFile(entry.name, archive->Extract(entry));
          ++named;
        }
        std::printf("  mounted %s as FUFS: %zu entradas, %zu com nome no VFS%s\n",
                    path, archive->Entries().size(), named,
                    archive->HasNames() ? "" : " (sem lista de nomes: so por hash)");
        fufs_archives.push_back(
            std::make_shared<zeebulator::FufsArchive>(std::move(*archive)));
      } else if (auto sar = zeebulator::SarArchive::Parse(raw)) {
        // Container SAR ("SWVARC") do chessbots. Parse devolve optional e nao
        // lanca, igual ao FUFS. Aqui os nomes sao texto puro, entao as entradas
        // entram direto no VFS -- nao ha resolvedor por hash a instalar.
        //
        // MEDIDO, e vale registrar: o chessbots NAO precisa deste caminho. Ele
        // abre "main.sar" por IFILEMGR, faz Seek para 0x21, le o indice inteiro
        // (0x21 ate o offset declarado em 0x0D) e depois faz Seek/Read direto no
        // offset de cada payload -- 51 dos 52 Seek de uma sessao caem EXATAMENTE
        // no payload_offset de uma entrada, com o tamanho pedido igual ao
        // payload_size gravado. Ou seja: o jogo e o dono do parser do container.
        // Montar as entradas aqui serve para quem passar um .sar como argumento
        // de assets e para diagnostico, nao para destravar o chessbots.
        for (const auto& entry : sar->Entries()) {
          if (auto blob = sar->Extract(entry)) vfs.AddFile(entry.name, std::move(*blob));
        }
        std::printf("  mounted %s as SAR: %zu entradas no VFS\n", path,
                    sar->Entries().size());
      }
      vfs.AddFile(BaseName(path), std::move(raw));
    }
  };
  merge_asset_arg(argv[2]);
  merge_asset_arg(argv[3]);
  if (argc >= 6) MergeBootPkgInto(vfs, argv[5]);
  // Sibling per-game .pkg auto-discovery (2026-09-02): arcade-core ports
  // (spinmast/strhoop/cninja/...) ship their content in a per-game <name>.pkg
  // next to the .mod. Running without it leaves the game's asset parser reading
  // empty data (a "class-C" boot stall). When the caller passed no explicit
  // --pkg, scan the mod's own folder for *.pkg siblings (excluding boot.pkg,
  // which has its own shared-bootstrap discovery below) and merge them. Opt-out
  // via ZEEB_NO_ASSET_AUTODISCOVER=1.
  if (pkg_paths.empty() && std::getenv("ZEEB_NO_ASSET_AUTODISCOVER") == nullptr) {
    namespace fs = std::filesystem;
    try {
      fs::path own_dir = fs::absolute(argv[1]).parent_path();
      std::vector<fs::path> found;
      for (const auto& e : fs::directory_iterator(own_dir)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        if (ext == ".pkg" && e.path().filename() != "boot.pkg")
          found.push_back(e.path());
      }
      std::sort(found.begin(), found.end());
      for (const auto& p : found) {
        std::printf("auto-discovered sibling game pkg %s\n", p.string().c_str());
        try {
          MergeGamePkgInto(vfs, p.string().c_str());
        } catch (const std::exception& e) {
          std::fprintf(stderr, "  (skipped %s: %s)\n", p.string().c_str(), e.what());
        }
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "pkg auto-discovery skipped: %s\n", e.what());
    }
  }
  for (const auto& pkg_path : pkg_paths) MergeGamePkgInto(vfs, pkg_path.c_str());
  // Loose sibling-asset resolution — LAZY, on-demand (2026-09-08, replaces the
  // eager 2026-09-02 pass). Several arcade-core ports (cninja/... — the Data
  // East cluster) open small framework/menu assets that ship as LOOSE files
  // next to the .mod (not packed in <name>.pkg): e.g. `ding.wav`,
  // `menusmall.fnz`, `menu.fnz`, `font.FNZ`, `*.tex`. Their fopen-style wrapper
  // (RE'd at cninja 0x11b754) opens them by BARE name via IFileMgr_OpenFile;
  // with the file absent our OpenFile returns handle 0 and the game derefs the
  // null FILE*, wandering to pc=0.
  //
  // The old fix pre-registered EVERY loose sibling (under 4 path roots) at
  // boot. That polluted the flat namespace + basename fallback and silently
  // regressed titles that merely SHARE a folder with unrelated loose files —
  // Double Dragon (folder 274754) went to a blank white frame (git bisect →
  // 86463ac; ROADMAP UPDATE 20). Fix: install a VFS miss-resolver that reads a
  // loose sibling from the host FS ONLY when the guest actually opens a name
  // that misses every in-VFS lookup. cninja asks for ding.wav → resolved on
  // demand; DD asks for none of them → namespace stays clean. Opt-out (skip the
  // host FS entirely) via ZEEB_NO_ASSET_AUTODISCOVER=1.
  // Arquivos de ESTADO DO APARELHO: quem os cria e o proprio console, na area
  // do usuario. Nenhum pacote os traz, entao um carregamento "solto" de pacote
  // nao tem copia deles. A resposta fiel para um console cuja area de usuario
  // nunca foi populada e "o arquivo existe e esta vazio" — responder "nao
  // existe" desvia o guest para OUTRO ramo: a Z-Wheel faz
  // IFileMgr::Test -> GetInfo -> OpenFile em `preloaded.cfg` e, com a falta do
  // arquivo, sai do caminho da lista de pre-instalados. Ver o documento da roda
  // da Z-Wheel, secao 2.5 (`preloaded.cfg` e estado do aparelho).
  //
  // `fontsize.map` NAO entra aqui: ele nao existe em lugar nenhum do dump, e a
  // Z-Wheel convive com a ausencia (medido; o documento confirma).
  {
    static const char* const kDeviceStateFiles[] = {"preloaded.cfg"};
    for (const char* device_file : kDeviceStateFiles) {
      if (!vfs.Exists(device_file)) vfs.AddFile(device_file, {});
    }
  }

  zeebulator::VirtualFilesystem::MissResolver loose_resolver;
  if (std::getenv("ZEEB_NO_ASSET_AUTODISCOVER") == nullptr) {
    namespace fs = std::filesystem;
    try {
      fs::path own_dir = fs::absolute(argv[1]).parent_path();
      const bool log_file = std::getenv("ZEEB_LOG_FILE") != nullptr;
      loose_resolver = [own_dir, log_file](const std::string& basename,
                                           std::vector<uint8_t>& out) -> bool {
        if (basename.empty() || basename[0] == '/' || basename[0] == '\\') {
          return false;
        }
        // Resolucao de caminhos relativos e irmaos (ex: "./../quake2res/pak0.pakz",
        // "../nfsresources/config.ini", "../preyresources/prey3d.bar"):
        // Normaliza lexicalmente o caminho e assegura que fique contido no
        // diretorio `mod/` pai (limite de seguranca do sistema de arquivos).
        fs::path mod_root = own_dir.parent_path();
        fs::path cand = (own_dir / basename).lexically_normal();
        auto rel_to_mod = cand.lexically_relative(mod_root);
        if (rel_to_mod.empty() || rel_to_mod.string().rfind("..", 0) == 0) {
          return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(cand, ec)) {
          // Se o caminho direto nao existe, resolve cada componente insensivel a caixa
          // a partir de mod_root (ou own_dir), cobrindo pastas irmas e subpastas.
          fs::path cur = mod_root;
          bool walked_all = true;
          for (const auto& part : rel_to_mod) {
            std::string pstr = part.string();
            if (pstr == "." || pstr.empty()) continue;
            if (pstr == "..") { cur = cur.parent_path(); continue; }
            bool matched_part = false;
            for (const auto& entry : fs::directory_iterator(cur, ec)) {
              std::string en = entry.path().filename().string();
              if (en.size() == pstr.size()) {
                bool eq = true;
                for (size_t i = 0; i < en.size(); ++i) {
                  if (std::tolower(static_cast<unsigned char>(en[i])) !=
                      std::tolower(static_cast<unsigned char>(pstr[i]))) {
                    eq = false;
                    break;
                  }
                }
                if (eq) {
                  cur = entry.path();
                  matched_part = true;
                  break;
                }
              }
            }
            if (!matched_part) { walked_all = false; break; }
          }
          if (walked_all && fs::is_regular_file(cur, ec)) {
            cand = cur;
          } else {
            return false;
          }
        }
        std::string lext = cand.extension().string();
        for (auto& c : lext) c = static_cast<char>(std::tolower(c));
        if (lext == ".mod" || lext == ".sig" ||
            lext == ".userdata" || lext == ".savestate" ||
            lext == ".playlog") {
          return false;
        }
        try {
          out = ReadFile(cand.string().c_str());
        } catch (const std::exception&) {
          return false;
        }
        if (log_file) {
          std::fprintf(stderr,
                       "[file] lazily resolved loose sibling '%s' (%zu bytes)\n",
                       basename.c_str(), out.size());
        }
        return true;
      };
    } catch (const std::exception& e) {
      std::fprintf(stderr, "loose-asset resolver not installed: %s\n", e.what());
    }
  }
  // Resolvedor final: FUFS primeiro, solto depois. O .vfs so pode ser servido
  // aqui porque a chave da tabela e o hash do caminho -- o jogo pede
  // "data/carts/chars/crash.pof" por string, o resolvedor calcula o hash e
  // acha a entrada por busca binaria, sem precisar de nome gravado.
  if (!fufs_archives.empty() || loose_resolver) {
    const bool log_file = std::getenv("ZEEB_LOG_FILE") != nullptr;
    vfs.SetMissResolver([fufs_archives, loose_resolver, log_file](
                            const std::string& name, std::vector<uint8_t>& out) -> bool {
      for (const auto& archive : fufs_archives) {
        if (const zeebulator::FufsEntry* entry = archive->Find(name)) {
          out = archive->Extract(*entry);
          if (log_file) {
            std::fprintf(stderr, "[file] FUFS serviu '%s' (hash %08x, %zu bytes)\n",
                         name.c_str(), entry->name_hash, out.size());
          }
          return true;
        }
      }
      return loose_resolver && loose_resolver(name, out);
    });
  }
  // Data East arcade-core ports (cninja/karnovr/supbtime/... — the Wall B
  // cluster, RE'd 2026-09-02) do NOT ship the shared arcade bootstrap in
  // their own download: they busy-wait forever at tick 0 until they can open
  // `boot.pkg` -> `boot.rom` (an 8192-byte 68000-style vector table). On a
  // real device that file is installed system-wide; in this sanctioned local
  // archive it physically ships in exactly one folder (Karnov's Revenge). So
  // when no boot.pkg was passed explicitly, auto-discover one next to the
  // game's own .mod (same dir, or a sibling mod folder). This matches the
  // console's system-wide install and unblocks the whole cluster at once.
  // Restricted to titles that ship no ggz (the pkg-based arcade ports); ggz
  // games (Double Dragon, etc.) never open boot.rom, so we don't pollute
  // their VFS with an unrelated bootstrap.
  bool has_ggz = std::string(argv[2]) != "-" || std::string(argv[3]) != "-";
  if (argc < 6 && !has_ggz) {
    namespace fs = std::filesystem;
    try {
      fs::path mod_path = fs::absolute(argv[1]);
      fs::path own_dir = mod_path.parent_path();
      std::vector<fs::path> search;
      if (fs::exists(own_dir / "boot.pkg")) search.push_back(own_dir / "boot.pkg");
      if (search.empty() && own_dir.has_parent_path()) {
        // Non-throwing iteration: a single unreadable/unrelated sibling directory (e.g. an
        // apt temp dir when the .mod happens to live under /tmp, as with an ad-hoc probe run)
        // must not abort the whole scan and hide a real boot.pkg sitting in another sibling.
        std::error_code iter_ec;
        fs::directory_iterator it(own_dir.parent_path(), iter_ec);
        fs::directory_iterator end;
        for (; !iter_ec && it != end; it.increment(iter_ec)) {
          std::error_code is_dir_ec;
          if (!it->is_directory(is_dir_ec) || is_dir_ec) continue;
          fs::path cand = it->path() / "boot.pkg";
          std::error_code exists_ec;
          if (fs::exists(cand, exists_ec) && !exists_ec) {
            search.push_back(cand);
            break;
          }
        }
      }
      if (!search.empty()) {
        std::printf("auto-discovered shared boot.pkg at %s\n", search.front().c_str());
        MergeBootPkgInto(vfs, search.front().c_str());
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "boot.pkg auto-discovery skipped: %s\n", e.what());
    }
  }
  // ABD-style resource archives arrive either positionally (argv[6],
  // which needs a boot.pkg at argv[5]) or via repeatable `--bar` (no
  // positional constraint). Unify them into one list processed at the
  // shell-registration site below (where shell_hle exists).
  std::vector<std::string> all_bar_paths = bar_paths;
  if (argc >= 7) all_bar_paths.emplace_back(argv[6]);

  // Sibling-asset auto-discovery (2026-09-02): a title's own resource archive
  // (.bar / .pakz) ships next to its .mod, and running WITHOUT it leaves the
  // game's asset parser reading empty data -- the confirmed root cause of the
  // "class-C" boot stalls (e.g. peggle sits in a fill-loop with no resources.bar
  // but reaches a real EVT_APP_RESUME once resources.bar is registered; Rolimaz
  // ships pak0.pakz; ridgeracer ships ridgeracer.bar). When the caller passed no
  // explicit --bar and no positional resources.bar, scan the .mod's own folder
  // for *.bar / *.pakz siblings and register them the same way. Opt-out with
  // ZEEB_NO_ASSET_AUTODISCOVER=1. Non-BAR files register-as-resource gracefully
  // (the loop below already tolerates parser throws, keeping raw bytes in VFS).
  if (all_bar_paths.empty() && std::getenv("ZEEB_NO_ASSET_AUTODISCOVER") == nullptr) {
    namespace fs = std::filesystem;
    try {
      fs::path own_dir = fs::absolute(argv[1]).parent_path();
      std::vector<fs::path> found;
      for (const auto& e : fs::directory_iterator(own_dir)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        if (ext == ".bar" || ext == ".pakz" || ext == ".zip") found.push_back(e.path());
      }
      std::sort(found.begin(), found.end());
      for (const auto& p : found) {
        all_bar_paths.emplace_back(p.string());
        std::printf("auto-discovered sibling asset archive %s\n", p.string().c_str());
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "asset auto-discovery skipped: %s\n", e.what());
    }
  }

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
  // Window title shows the actual game (mod filename stem) instead of a
  // generic "game probe", so multiple concurrent runs are tellable apart
  // and screenshots are self-labeling. e.g. ".../274754/ddragonz.mod" ->
  // "Zeebulator - ddragonz".
  std::string window_title = "Zeebulator - game probe";
  {
    std::filesystem::path mp(argv[1]);
    std::string stem = mp.stem().string();
    if (!stem.empty()) window_title = "Zeebulator - " + stem;
  }
  std::fprintf(stderr, "[title] %s\n", window_title.c_str());
  SDL_Window* window = SDL_CreateWindow(
      window_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
      SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  // CPU backend selectable via ZEEB_CPU (default: interpreter). The probe's
  // per-instruction hooks (DebugHooks trace/wwatch, ABD-PC state, spin/liveness)
  // live only on the interpreter path, so the JIT is opt-in for hot-drain
  // acceleration where no instrumentation is armed. See ROADMAP "JIT DE BLOCO".
  std::unique_ptr<zeebulator::IArmCore> cpu_owner =
      zeebulator::MakeArmCore(zeebulator::SelectCpuBackendFromEnv());
  zeebulator::IArmCore& cpu = *cpu_owner;
  constexpr uint32_t kTrapBase = 0xF0000000;
  zeebulator::HleRuntime hle(cpu, kTrapBase, 0x10000);

  // Safe landing pad at address 0: writes 'bx lr' (0xe12fff1e) across the zero page
  // so that accidental calls to NULL function pointers (or uninitialized vtable slots
  // returning 0) return immediately to the caller instead of wandering into memory faults.
  for (uint32_t a = 0; a < 0x100; a += 4) {
    cpu.GetMemory().Write32(a, 0xe12fff1e);
  }
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
  // ZEEB_GL_SOFT=1: rasterizador de software (SoftGlBackend) compõe o
  // conteúdo GL diretamente no framebuffer RGB565 do IDisplay, para que o
  // screenshot headless enxergue geometria real (destrava UPDATE 19). Fica
  // atrás de env var para não alterar o default (recorder), preservando o
  // oráculo congelado. SwapBuffers marca has_presented_ via
  // PresentLiveFramebuffer.
  std::unique_ptr<zeebulator::SoftGlBackend> soft_gl;
  zeebulator::GlBackend* gl_backend = &gl_recorder;
  if (const char* gs = std::getenv("ZEEB_GL_SOFT"); gs && gs[0] == '1') {
    soft_gl = std::make_unique<zeebulator::SoftGlBackend>(
        display.MutableFramebuffer(), kWidth, kHeight);
    soft_gl->SetPresentCallback(
        [](void* u) { static_cast<zeebulator::IDisplayHle*>(u)->PresentLiveFramebuffer(); },
        &display);
    gl_backend = soft_gl.get();
    std::fprintf(stderr, "[gl] SoftGlBackend ativo (ZEEB_GL_SOFT=1)\n");
  }
  zeebulator::GlHle gl_hle(*gl_backend);
  zeebulator::Mixer mixer(kAudioSampleRate);
  zeebulator::FileHle file_hle(cpu.GetMemory(), hle, vfs, /*object_region=*/0x80100000);
  {
    std::ifstream userdata_in(userdata_load_path, std::ios::binary);
    if (userdata_in && file_hle.Deserialize(userdata_in)) {
      std::printf("loaded real save-game data from %s%s\n", userdata_load_path.c_str(),
                  userdata_load_path == userdata_path ? "" : " (legacy import; future writes use XDG)");
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
  // Phase-1 fix: protect HLE-owned media interface bindings from the
  // game's Release-clear (ddragonz.mod 0x11f424 zeroing media_source+8
  // before the input-gated Play path 0x11d04c reads it). Region matches
  // MediaHle's object_region above (objects are bump-allocated from
  // 0x80200000; kNotifyScratchOffset keeps them well under 0x80300000).
  // Replaces the old +0x28 address-heuristic redirect HACK — see
  // core/memory/memory.{h,cpp} and
  // research/sources/2026-08-31_dd-media-interface-contract.md.
  cpu.GetMemory().SetMediaBindingGuardRegion(0x80200000, 0x80300000);

  // ZEEB_SEED_READ=addr:value (hex) — persistent +0x63c optional-callback
  // seed at the memory layer. Fires on every guard read (interp AND JIT),
  // returning `value` whenever [addr] reads back 0 (impossible on hardware).
  // See core/memory/memory.h SetSeedRead. Inert unless the env var is set.
  if (const char* sr = std::getenv("ZEEB_SEED_READ")) {
    unsigned long addr = 0, val = 0;
    if (std::sscanf(sr, "%lx:%lx", &addr, &val) == 2 && addr != 0) {
      cpu.GetMemory().SetSeedRead(static_cast<uint32_t>(addr),
                                  static_cast<uint32_t>(val));
      std::fprintf(stderr, "[seedread] armed [0x%08lx] 0->0x%08lx\n", addr, val);
    }
  }

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);
  auto mod_size = static_cast<uint32_t>(mod_data.size());

  // ZEEB_FORCE_BRANCH=pc[,pc...] (hex) — force the conditional branch at each
  // PC to UNCONDITIONAL by rewriting the ARM condition field (bits 31..28) to
  // 0xE ("always"). This is the faithful expression of the +0x63c OPTIONAL
  // callback: the guard is `cmp r3,r5; beq skip; blx r3` and the real device,
  // for a title whose (type-gated) register-step never ran, always has the
  // "no callback" sentinel so it ALWAYS takes `beq skip`. Forcing the branch
  // reproduces exactly that skip under BOTH backends (unlike a fixed memory
  // seed, which cannot equal the live r5=r7+0x48). Applied after LoadMod and
  // NotifyCodeChanged'd so the JIT recompiles the patched block. Inert unless
  // the env var is set.
  if (const char* fb = std::getenv("ZEEB_FORCE_BRANCH")) {
    std::string s(fb);
    size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos);
      std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      pos = (comma == std::string::npos) ? s.size() : comma + 1;
      unsigned long bpc = std::strtoul(tok.c_str(), nullptr, 16);
      if (bpc == 0) continue;
      uint32_t instr = cpu.GetMemory().Read32(static_cast<uint32_t>(bpc));
      uint32_t forced = (instr & 0x0FFFFFFFu) | 0xE0000000u;  // cond -> AL
      cpu.GetMemory().Write32(static_cast<uint32_t>(bpc), forced);
      cpu.NotifyCodeChanged(static_cast<uint32_t>(bpc), 4);
      std::fprintf(stderr, "[forcebranch] [0x%08lx] 0x%08x -> 0x%08x (unconditional)\n",
                   bpc, instr, forced);
    }
  }

  // ZEEB_NOP_INSTR=pc[,pc...] (hex) — overwrite each instruction with an ARM
  // NOP (`mov r0,r0` = 0xe1a00000). Used to neutralize the optional-callback
  // INDIRECT CALL directly (`blx r3`) instead of flipping the guard's `beq`.
  // This is the block-terminal-preserving variant of ZEEB_FORCE_BRANCH: an
  // indirect `blx r3` terminates a JIT IR block, and force-branching the beq
  // that guards it made dynarmic set two terminals on one block
  // (`ASSERT !HasTerminal()`); NOP-ing the call leaves all block boundaries
  // intact and just skips the (absent) callback. Inert unless set.
  if (const char* np = std::getenv("ZEEB_NOP_INSTR")) {
    std::string s(np);
    size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos);
      std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      pos = (comma == std::string::npos) ? s.size() : comma + 1;
      unsigned long npc = std::strtoul(tok.c_str(), nullptr, 16);
      if (npc == 0) continue;
      uint32_t instr = cpu.GetMemory().Read32(static_cast<uint32_t>(npc));
      cpu.GetMemory().Write32(static_cast<uint32_t>(npc), 0xE1A00000u);  // NOP
      cpu.NotifyCodeChanged(static_cast<uint32_t>(npc), 4);
      std::fprintf(stderr, "[nopinstr] [0x%08lx] 0x%08x -> 0xe1a00000 (nop)\n",
                   npc, instr);
    }
  }


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

  // Three independently confirmed applet constructors (quake/pacmania/
  // chessbots) read GetAppContext()+0x68 during IModule::CreateInstance and
  // call slot 2 without a null check. The slot's returned pointer is checked
  // and zero is a valid "service has no object" result. Supply only that
  // evidenced empty service object; all methods remain non-mutating stubs.
  uint32_t context_service_0x68 = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80068000, /*object=*/0x80069000,
      /*slot_count=*/8);
  mod_runtime.SetSixthContextObject(context_service_0x68);

  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);
  // Show the console's own start-up panel state (cleared to white) before the
  // title draws anything, instead of an all-black window.
  display.ResetToBlankPanel();
  // Dedicated offscreen-DIB arena for CreateDIBitmap (slot 13), isolated from
  // the ModRuntime heap so app allocations can never collide.
  display.SetDibArena(/*base=*/0x84000000, /*size=*/0x00400000);
  // Real compiled app code obtains IDisplay through
  // ISHELL_CreateInstance(AEECLSID_DISPLAY, ...), not directly -- found
  // via real disassembly of AEEApplet_New's call chain (PHASE8_LOG.md).
  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle, kWidth, kHeight);
  shell_hle.SetAllocator([&mod_runtime](uint32_t sz) { return mod_runtime.Allocate(sz); });
  // Extract folder ID (Item ID) from mod path, e.g. /.../mod/279712/zumar.mod -> 279712
  uint32_t item_id = 0;
  {
    std::filesystem::path p(argv[1]);
    std::string folder_name = p.parent_path().filename().string();
    char* end = nullptr;
    unsigned long parsed = std::strtoul(folder_name.c_str(), &end, 10);
    if (end && *end == '\0' && parsed != 0) item_id = static_cast<uint32_t>(parsed);
  }
  shell_hle.SetAppletClassAndItemId(cls_id, item_id);
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
  const bool is_abd_title = (std::string(argv[1]).find("abd.mod") != std::string::npos);
  for (const std::string& bar_path : all_bar_paths) {
    std::vector<uint8_t> bar_bytes = ReadFile(bar_path.c_str());
    if (is_abd_title && !abd_font_atlas) abd_font_atlas = DecodeAbdFontAtlas(bar_bytes);
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
    std::string base = BaseName(bar_path.c_str());
    vfs.AddFile(base, bar_bytes);
    // A folder may ship several `.bar` files where only some are real BREW
    // resource archives; the rest are the title's own data in another format.
    // The batch harness passes them all via `--bar`, so a non-BAR file must
    // NOT abort the whole run: register-as-resource can throw from the BAR
    // parser (bad magic / inconsistent header). Skip those gracefully -- the
    // raw bytes are still exposed in the VFS above for direct opening.
    try {
      shell_hle.RegisterResourceFile(base, std::move(bar_bytes));
      std::printf("loaded resource archive %s (registered as %s)\n", bar_path.c_str(),
                  base.c_str());
    } catch (const std::exception& e) {
      // Not a BREW BAR archive. Before giving up, try the OTHER real
      // container formats that ship next to a .mod. `PakzArchive` has
      // existed in core/loader/pakz.cpp all along but was only ever
      // reachable from tools/pakz_inspector -- so seven real titles
      // (Rolimaz, AirRacez, Bajaz, Boiaz, JetBoardz, Alice,
      // ActivityCenter) shipped a perfectly parseable pak0.pakz /
      // resources.pakz whose entries never reached the VFS, and every
      // one of them sat in its tick loop with an empty asset set.
      bool mounted = false;
      try {
        auto pakz = zeebulator::PakzArchive::Parse(ReadFile(bar_path.c_str()));
        size_t ok = 0;
        for (const auto& entry : pakz.Entries()) {
          try {
            vfs.AddFile(entry.name, pakz.Extract(entry));
            ++ok;
          } catch (const std::exception&) {
            // One bad member must not lose the rest of the archive.
          }
        }
        if (ok != 0) {
          std::printf("mounted %s as PAKZ: %zu/%zu entries into the VFS\n",
                      bar_path.c_str(), ok, pakz.Entries().size());
          mounted = true;
        }
      } catch (const std::exception&) {
        // Not a PAKZ either -- fall through to the raw-bytes note.
      }
      if (!mounted) {
        std::printf("skipped %s: not a parseable BAR/PAKZ resource archive (%s); "
                    "raw bytes still in VFS as %s\n",
                    bar_path.c_str(), e.what(), base.c_str());
      }
    }
  }
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_obj);
  // AEECLSID_DISPLAY1 (0x010127d4): a second display-interface class some
  // titles (prey3d 276154, pbc 280238) request via ISHELL_CreateInstance
  // right after AEECLSID_DISPLAY. Empirically (ZEEB_LOG_CREATEINSTANCE) these
  // titles abort EVT_APP_START when it comes back EFAILED. It is ABI-
  // compatible enough with IDisplay for the early boot path (the game derefs
  // the returned object's IDisplay-shaped vtable), so hand back the same real
  // IDisplay object; extend with a dedicated vtable if a title is later shown
  // to call a DISPLAY1-only slot.
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY1=*/0x010127d4, display_obj);
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
  // Implementacao do IImage para objetos retornados por ISHELL_LoadResObject (slot 19).
  // Tanto Quake (`splash_title.png`) quanto a Z-Wheel (`opening_low.gif`, em AnimationVideo_Form)
  // chamam IIMAGE_Notify (slot 10 = vtable[10]) com (po, pfn, pUser).
  // No caso da Z-Wheel (0x101d8c), o callback (0x13d484) consulta as dimensoes via GetInfo (slot 4)
  // e define o sinalizador [context+0x2c]=1. Sem disparar a notificacao, a animacao de abertura
  // ficava em espera infinita ("Failure waiting for image load to complete: %d") e nunca alcancava
  // a montagem da roda 3D nem o menu principal.
  constexpr uint32_t kImageInfoAddr = 0x80030800;
  cpu.GetMemory().Write16(kImageInfoAddr + 0, 640); // cx
  cpu.GetMemory().Write16(kImageInfoAddr + 2, 480); // cy
  cpu.GetMemory().Write16(kImageInfoAddr + 4, 0);   // nColors
  cpu.GetMemory().Write8(kImageInfoAddr + 6, 1);    // bAnimated = TRUE
  cpu.GetMemory().Write16(kImageInfoAddr + 8, 640); // cxFrame = 640

  // Slots ainda nao implementados: devolvem SUCCESS, mas agora REGISTRAM quem
  // ----------------------------------------------------------------
  // IImage REAL, um objeto POR RECURSO (ISHELL_LoadResObject, slot 19).
  //
  // Antes: UM objeto falso unico (0x8006B000) devolvido para todo recurso,
  // com GetInfo mentindo 640x480 e Draw que nao desenhava nada. Isso e pior
  // que um stub honesto -- o jogo recebe "sucesso", acredita ter a imagem e
  // segue. E o caso da Z-Wheel: o fundo azul do palco (recurso 5007, um BMP
  // 214x34) e a arte do roller nunca aparecem, sem nenhum erro visivel.
  //
  // Formato do payload no .brf, medido nos arquivos reais do tectoy:
  //   [u16 header_len][mime NUL-terminado][bytes da imagem]
  // com header_len contando o proprio u16 (12 = 2 + "image/bmp\0"). O strip
  // usa o header_len, nao a busca pelo primeiro byte nao-texto, porque um BMP
  // comeca com "BM" (texto) e a heuristica erraria.
  // ----------------------------------------------------------------
  struct ResImage {
    int width = 0;
    int height = 0;
    std::vector<std::vector<uint8_t>> frames;  // RGBA8888, topo primeiro
    size_t current = 0;
  };
  auto images =
      std::make_shared<std::unordered_map<uint32_t, std::shared_ptr<ResImage>>>();
  const bool log_image = std::getenv("ZEEB_LOG_IMAGE") != nullptr;
  // Faixa 0x80030000..0x8003F000 dedicada a IImage.
  // CRUCIAL: 0x8006C000 era kWidgetVtable! Quando opening_low.gif era
  // instanciado la, ele destruia a vtable de TODOS os widgets, fazendo o
  // applet saltar para 0x8006A000 no primeiro evento de UI.
  constexpr uint32_t kResImageVtable = 0x80030000;
  constexpr uint32_t kResImageObjectBase = 0x80031000;

  std::vector<zeebulator::HleRuntime::HleFunction> res_image_methods;
  res_image_methods.reserve(16);
  // 16 slots, nao 12: o jogo chama o slot 12 deste objeto (medido). Com uma
  // vtable menor esse slot cai em memoria nao escrita e o guest salta para lixo.
  for (uint32_t slot = 0; slot < 16; ++slot) {
    res_image_methods.push_back([slot, log_image](zeebulator::IArmCore& core) {
      if (log_image) {
        std::fprintf(stderr,
                     "[image] slot %u NAO IMPLEMENTADO (stub SUCCESS) "
                     "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
                     slot, core.GetRegister(zeebulator::kR0), core.GetRegister(zeebulator::kR1),
                     core.GetRegister(zeebulator::kR2), core.GetRegister(zeebulator::kR3));
      }
      core.SetRegister(zeebulator::kR0, 0); // SUCCESS
    });
  }
  // Busca o estado do objeto pelo `po` que o guest passou em r0. Sem isto os
  // lambdas nao teriam como saber QUAL imagem esta sendo desenhada.
  auto image_of = [images](zeebulator::IArmCore& core) -> std::shared_ptr<ResImage> {
    auto it = images->find(core.GetRegister(zeebulator::kR0));
    return it == images->end() ? nullptr : it->second;
  };
  res_image_methods[0] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); }; // AddRef
  res_image_methods[1] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); }; // Release

  // Desenha RGBA8888 no framebuffer RGB565 do IDisplayHle. Alpha 0 e
  // transparente; nao ha blending (o IImage real faz ROP COPY por padrao,
  // IPARM_ROP=3 default AEE_RO_COPY).
  auto draw_res_image = [&display, log_image](const std::shared_ptr<ResImage>& img, int x, int y,
                                              size_t frame_index, uint32_t lr) -> uint32_t {
    if (img == nullptr || img->frames.empty()) return 0;
    const auto& px = img->frames[std::min(frame_index, img->frames.size() - 1)];
    auto& fb = display.MutableFramebuffer();
    uint32_t drawn = 0;
    for (int row = 0; row < img->height; ++row) {
      const int dy = y + row;
      if (dy < 0 || dy >= display.height()) continue;
      for (int col = 0; col < img->width; ++col) {
        const int dx = x + col;
        if (dx < 0 || dx >= display.width()) continue;
        const size_t o = (static_cast<size_t>(row) * img->width + col) * 4;
        if (px[o + 3] == 0) continue;
        fb[static_cast<size_t>(dy) * display.width() + dx] = static_cast<uint16_t>(
            ((px[o + 0] >> 3) << 11) | ((px[o + 1] >> 2) << 5) | (px[o + 2] >> 3));
        ++drawn;
      }
    }
    if (log_image) {
      // O LR diz QUEM pediu o desenho: sem ele nao da para separar "o jogo
      // desenha a abertura" de "nos chamamos um handler de desenho velho".
      std::fprintf(stderr, "[image] Draw %dx%d em (%d,%d) frame=%zu -> %u pixels lr=0x%08x\n",
                   img->width, img->height, x, y, frame_index, drawn, lr);
    }
    return drawn;
  };

  // Slot 2: Draw(po, x, y)
  res_image_methods[2] = [image_of, draw_res_image](zeebulator::IArmCore& core) {
    const auto img = image_of(core);
    const int x = static_cast<int32_t>(core.GetRegister(zeebulator::kR1));
    const int y = static_cast<int32_t>(core.GetRegister(zeebulator::kR2));
    if (img != nullptr && img->frames.size() > 1) {
      img->current = (img->current + 1) % img->frames.size();
    }
    draw_res_image(img, x, y, img != nullptr ? img->current : 0, core.GetRegister(zeebulator::kLR));
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 3: DrawFrame(po, nFrame, x, y)
  res_image_methods[3] = [image_of, draw_res_image](zeebulator::IArmCore& core) {
    const auto img = image_of(core);
    const size_t frame = core.GetRegister(zeebulator::kR1);
    const int x = static_cast<int32_t>(core.GetRegister(zeebulator::kR2));
    const int y = static_cast<int32_t>(core.GetRegister(zeebulator::kR3));
    draw_res_image(img, x, y, frame, core.GetRegister(zeebulator::kLR));
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 4: GetInfo(po, AEEImageInfo *pi).
  // Layout do SDK: {uint16 cx; uint16 cy; uint16 nColors; boolean bAnimated;
  // uint16 cxFrame} e `boolean` = unsigned char (AEEStdDef.h:39), logo 9
  // bytes uteis + 1 de alinhamento. Antes esta slot mentia 640x480 para
  // qualquer recurso -- inclusive para os BMPs 214x34 do roller.
  res_image_methods[4] = [image_of, log_image](zeebulator::IArmCore& core) {
    const auto img = image_of(core);
    const uint32_t pi = core.GetRegister(zeebulator::kR1);
    if (img == nullptr && pi != 0) {
      // Objeto de fallback (recurso que nao decodificou): mantem exatamente a
      // resposta legada 640x480. Nao e a verdade sobre nenhuma imagem -- e o
      // contrato que os titulos que so querem um ponteiro valido ja assumiam.
      core.GetMemory().Write16(pi + 0, 640);
      core.GetMemory().Write16(pi + 2, 480);
      core.GetMemory().Write16(pi + 4, 0);
      core.GetMemory().Write8(pi + 6, 1);
      core.GetMemory().Write16(pi + 8, 640);
    }
    if (img != nullptr && pi != 0) {
      core.GetMemory().Write16(pi + 0, static_cast<uint16_t>(img->width));
      core.GetMemory().Write16(pi + 2, static_cast<uint16_t>(img->height));
      core.GetMemory().Write16(pi + 4, 0);
      core.GetMemory().Write8(pi + 6, img->frames.size() > 1 ? 1 : 0);
      core.GetMemory().Write16(pi + 8, static_cast<uint16_t>(img->width));
      if (log_image) {
        std::fprintf(stderr, "[image] GetInfo -> %dx%d frames=%zu\n", img->width, img->height,
                     img->frames.size());
      }
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 6: Start(po, x, y) -- o IImage real anima por timer proprio; aqui
  // desenha o primeiro frame e marca o objeto como iniciado.
  res_image_methods[6] = [image_of, draw_res_image](zeebulator::IArmCore& core) {
    const auto img = image_of(core);
    const int x = static_cast<int32_t>(core.GetRegister(zeebulator::kR1));
    const int y = static_cast<int32_t>(core.GetRegister(zeebulator::kR2));
    if (img != nullptr) img->current = 0;
    draw_res_image(img, x, y, 0, core.GetRegister(zeebulator::kLR));
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 10: Notify(po, PFNIMAGEINFO pfn, void *pUser). Sem alteracao de
  // contrato -- so agora a AEEImageInfo entregue e' a real.
  res_image_methods[10] = [&cpu, &hle, image_of](zeebulator::IArmCore& core) {
    const uint32_t po = core.GetRegister(zeebulator::kR0);
    const uint32_t pfn = core.GetRegister(zeebulator::kR1);
    const uint32_t puser = core.GetRegister(zeebulator::kR2);
    if (pfn != 0) {
      const auto img = image_of(core);
      if (img != nullptr) {
        cpu.GetMemory().Write16(kImageInfoAddr + 0, static_cast<uint16_t>(img->width));
        cpu.GetMemory().Write16(kImageInfoAddr + 2, static_cast<uint16_t>(img->height));
        cpu.GetMemory().Write16(kImageInfoAddr + 4, 0);
        cpu.GetMemory().Write8(kImageInfoAddr + 6, img->frames.size() > 1 ? 1 : 0);
        cpu.GetMemory().Write16(kImageInfoAddr + 8, static_cast<uint16_t>(img->width));
      }
      if (std::getenv("ZEEB_LOG_IMAGE") != nullptr) {
        std::fprintf(stderr, "[image] IImage::Notify po=0x%08x pfn=0x%08x pUser=0x%08x\n",
                     po, pfn, puser);
      }
      // PFNIMAGEINFO signature: void (*)(void *pUser, IImage *po, AEEImageInfo *pi, int nErr)
      hle.CallArmFunctionPreservingContext(pfn, puser, po, kImageInfoAddr, 0);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };

  // Desenha a imagem de um objeto pelo ENDERECO dele. E assim que o passe de
  // widgets pinta o conteudo de um ImageWidget: o guest entrega o IImage ao
  // widget por SetIPtr e nunca mais chama Draw -- num BREW real quem desenha e
  // a biblioteca de widgets do aparelho, que aqui somos nos.
  auto draw_image_object = [images, draw_res_image](uint32_t obj, int x, int y) -> uint32_t {
    auto it = images->find(obj);
    if (it == images->end() || it->second == nullptr) return 0;
    return draw_res_image(it->second, x, y, it->second->current, 0);
  };

  // A vtable e uma so para todas as imagens; cada recurso ganha o seu
  // objeto (e o seu buffer), na mesma faixa dos objetos HLE ja usados.
  //
  // A PRIMEIRA faixa (kResImageObjectBase) pertence ao objeto de FALLBACK
  // construido logo abaixo, e os recursos reais comecam DEPOIS dele.
  // Defeito real corrigido aqui: os dois comecavam no mesmo endereco, entao o
  // objeto de fallback E o primeiro recurso decodificado eram o MESMO objeto.
  // Medido na Z-Wheel: o recurso 5029 nao existe em nenhum .brf, LoadResObject
  // caia no fallback e devolvia o objeto do opening_low.gif -- e o roller
  // desenhava o GIF de abertura 640x480 por cima da tela inteira, 69 vezes,
  // sempre a partir de DrawRollerExt (lr=0x0011ff28), escondendo o palco 3D.
  uint32_t next_image_object = kResImageObjectBase + 0x100;
  uint32_t load_res_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kResImageVtable, kResImageObjectBase, res_image_methods);

  // Mantido: quando a fabrica nao consegue decodificar, LoadResObject volta a
  // devolver ESTE objeto em vez de 0. Sem isto, titulos que so dereferenciam o
  // retorno (Quake em EVT_APP_START) voltam a saltar para o endereco zero.
  shell_hle.SetLoadResObjectReturn(load_res_obj);

  // Ligada por padrao; ZEEB_NO_RES_IMAGE=1 desliga (interruptor de bisseccao).
  //
  // Historico honesto desta chave, tudo medido na Z-Wheel em execucoes de 27 s:
  //  1. Ligar a fabrica sem mais nada: 256 cores, 263287 px brancos, palco 3D
  //     sumido. A culpa NAO era da decodificacao: o objeto de fallback e o
  //     primeiro recurso decodificado nasciam no MESMO endereco, entao todo
  //     recurso que nao decodificava devolvia o opening_low.gif, e o roller
  //     pintava esse GIF 640x480 por cima de tudo 69 vezes (lr=0x0011ff28,
  //     dentro de DrawRollerExt).
  //  2. Com as faixas separadas: 742 cores, palco 3D de volta (laranja da roda
  //     9543 px, preto 81409 px), tick 114, nenhum desenho destrutivo.
  // Um objeto unico e mentiroso para TODO recurso e um defeito por si so; o
  // fallback continua existindo so para quem precisa de um ponteiro valido.
  const bool res_image_disabled = std::getenv("ZEEB_NO_RES_IMAGE") != nullptr;

  // Fabrica real do slot 19: resolve o payload (do .brf por id, ou arquivo
  // solto quando o id e 0), decodifica BMP/PNG/GIF e devolve um objeto
  // proprio. Devolve 0 quando nao da para decodificar, para o chamador cair
  // no objeto injetado (que segue existindo: alguns titulos so querem um
  // ponteiro valido, sem usar pixel nenhum).
  shell_hle.SetLoadResObjectFactory(
      [&cpu, &hle, &display, &vfs, &shell_hle, images, &next_image_object, log_image,
       res_image_disabled](
          const std::string& file, uint16_t id, uint32_t cls_id) -> uint32_t {
    if (res_image_disabled) return 0;
    std::vector<uint8_t> payload;
    if (id != 0) {
      auto raw = shell_hle.ReadBrewResource(file, id);
      if (!raw.has_value()) return 0;
      payload = std::move(*raw);
    } else if (const std::vector<uint8_t>* f = vfs.Find(file); f != nullptr) {
      payload = *f;
    } else {
      return 0;
    }
    if (payload.size() < 8) return 0;

    // Strip do cabecalho MIME do .brf.
    size_t off = 0;
    std::string mime;
    const uint16_t hlen = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
    if (hlen >= 2 && hlen <= 64 && payload.size() > hlen) {
      std::string cand(reinterpret_cast<const char*>(payload.data() + 2), hlen - 2);
      while (!cand.empty() && cand.back() == '\0') cand.pop_back();
      bool printable = !cand.empty();
      for (char c : cand) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
          printable = false;
        }
      }
      if (printable && cand.rfind("image/", 0) == 0) {
        mime = cand;
        off = hlen;
      }
    }

    auto img = std::make_shared<ResImage>();
    const uint8_t* data = payload.data() + off;
    const size_t size = payload.size() - off;
    if (size >= 6 && std::memcmp(data, "GIF8", 4) == 0) {
      // GIF: os frames do decoder vem no retangulo do PROPRIO frame (com
      // disposal por GCE), entao compor a tela logica e necessario antes de
      // desenhar -- um GIF animado exige isso, e o opening_low.gif do
      // tectoy e 1 frame de tela cheia.
      auto gif = zeebulator::DecodeGif(data, size);
      if (!gif.has_value()) return 0;
      img->width = gif->width;
      img->height = gif->height;
      std::vector<uint8_t> canvas(static_cast<size_t>(gif->width) * gif->height * 4, 0);
      std::vector<uint8_t> previous;
      for (const auto& fr : gif->frames) {
        // O frame sai do decoder no retangulo DELE (com x/y proprios), nao na
        // tela logica -- compor e obrigacao do chamador. Alpha 0 significa
        // transparencia (o decoder zera o alpha do indice transparente em vez
        // de usar cor magica), entao pixel transparente nao e copiado.
        if (fr.disposal_method == 3) previous = canvas;  // 3 = restaurar o anterior
        for (int row = 0; row < fr.height; ++row) {
          const int dy = fr.y + row;
          if (dy < 0 || dy >= gif->height) continue;
          for (int col = 0; col < fr.width; ++col) {
            const int dx = fr.x + col;
            if (dx < 0 || dx >= gif->width) continue;
            const size_t s = (static_cast<size_t>(row) * fr.width + col) * 4;
            if (fr.rgba[s + 3] == 0) continue;
            const size_t d = (static_cast<size_t>(dy) * gif->width + dx) * 4;
            canvas[d + 0] = fr.rgba[s + 0];
            canvas[d + 1] = fr.rgba[s + 1];
            canvas[d + 2] = fr.rgba[s + 2];
            canvas[d + 3] = 255;
          }
        }
        // O que a tela logica mostra NESTE instante e o canvas ja composto.
        img->frames.push_back(canvas);
        if (fr.disposal_method == 2) {
          // 2 = restaurar a cor de fundo: limpa o retangulo deste frame.
          for (int row = 0; row < fr.height; ++row) {
            const int dy = fr.y + row;
            if (dy < 0 || dy >= gif->height) continue;
            for (int col = 0; col < fr.width; ++col) {
              const int dx = fr.x + col;
              if (dx < 0 || dx >= gif->width) continue;
              const size_t d = (static_cast<size_t>(dy) * gif->width + dx) * 4;
              canvas[d + 0] = canvas[d + 1] = canvas[d + 2] = canvas[d + 3] = 0;
            }
          }
        } else if (fr.disposal_method == 3 && !previous.empty()) {
          canvas = previous;
        }
      }
    } else if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
      int w = 0, h = 0;
      auto px = zeebulator::DecodePng(data, size, w, h);
      if (!px.has_value()) return 0;
      img->width = w;
      img->height = h;
      img->frames.push_back(std::move(*px));
    } else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
      int w = 0, h = 0;
      auto px = zeebulator::DecodeBmp(data, size, w, h);
      if (!px.has_value()) return 0;
      img->width = w;
      img->height = h;
      img->frames.push_back(std::move(*px));
    } else {
      return 0;
    }

    const uint32_t obj = next_image_object;
    next_image_object += 0x100;
    if (next_image_object > kResImageObjectBase + 0x8000) return 0;  // guarda de faixa
    cpu.GetMemory().Write32(obj, kResImageVtable);
    (*images)[obj] = img;
    if (log_image || std::getenv("ZEEB_LOG_RES") != nullptr) {
      std::fprintf(stderr,
                   "[res] IImage real '%s' id=%u cls=0x%08x mime='%s' %dx%d frames=%zu -> obj=0x%08x\n",
                   file.c_str(), id, cls_id, mime.c_str(), img->width, img->height,
                   img->frames.size(), obj);
    }
    return obj;
  });
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
  // AEECLSID_DIB (0x01001045) / IDIB: Device Independent Bitmap format (AEEIDIB.h).
  // A struct with public fields that games read directly from guest memory
  // without going through vtable methods (AEEIDIB.h lines 40-55):
  //   +00 vtable
  //   +04 pPaletteMap (u32)
  //   +08 pBmp (u32)
  //   +12 pRGB (u32)
  //   +16 ncTransparent (u32)
  //   +20 cx (u16) = 640
  //   +22 cy (u16) = 480
  //   +24 nPitch (i16) = 1280 (640 * 2)
  //   +26 cntRGB (u16) = 0
  //   +28 nDepth (u8) = 16 (bits per pixel)
  //   +29 nColorScheme (u8) = 16 (IDIB_COLORSCHEME_565, 5-6-5 RGB)
  //
  // Root cause of failure in zumar and bjt: both games call
  // `IBITMAP_QueryInterface(pDeviceBmp, AEECLSID_DIB, &pDIB)` and immediately
  // inspect byte [pDIB + 29] (nColorScheme). They compare against 16 (RGB565).
  // If not 16, they abort with "INITIALIZATION FAILED!" and return 2 from the
  // subsystem factory, leaving game members null and causing a crash on EVT_APP_START.
  uint32_t unknown_0x01001045_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80018000, /*object=*/0x80019000, /*slot_count=*/20);
  // pBmp (+8): o buffer de pixels de verdade.
  //
  // Ficava NULO. A struct tinha geometria correta (640x480, pitch 1280,
  // RGB565) mas nenhum lugar onde escrever -- e e exatamente por aqui que
  // os jogos comerciais desenham: `IBITMAP_QueryInterface(devbmp,
  // AEECLSID_DIB, &pDIB)` e depois escrita direta em `pDIB->pBmp`, sem
  // passar por vtable nenhuma. Confirma o oraculo zeebx ("o IDIB entrega
  // ao jogo o ponteiro do buffer para ele desenhar direto -- e assim que
  // os jogos comerciais escrevem na tela").
  //
  // Evidencia que nomeou este gap: ddragonz chama IDISPLAY_Update 1000
  // vezes em 35s com ZERO draw calls contabilizados (DrawRect/BitBlt/
  // DrawText todos em 0). Ou seja, ele compoe o quadro inteiro sozinho e
  // so pede a apresentacao -- com pBmp nulo, nao tinha onde compor.
  //
  // Acima do heap do modulo (0x80300000 + 0x04000000) para nao colidir
  // com nada; a memoria e paginada sob demanda, entao reservar aqui nao
  // custa nada ate ser tocado.
  constexpr uint32_t kDibPixelBuffer = 0x88000000;
  constexpr uint32_t kDibPixelBytes = 640u * 480u * 2u;
  for (uint32_t off = 0; off < kDibPixelBytes; off += 4) {
    cpu.GetMemory().Write32(kDibPixelBuffer + off, 0);
  }
  cpu.GetMemory().Write32(0x80019000 + 4, 0);
  cpu.GetMemory().Write32(0x80019000 + 8, kDibPixelBuffer);
  cpu.GetMemory().Write32(0x80019000 + 12, 0);
  cpu.GetMemory().Write32(0x80019000 + 16, 0);
  cpu.GetMemory().Write16(0x80019000 + 20, 640);
  cpu.GetMemory().Write16(0x80019000 + 22, 480);
  cpu.GetMemory().Write16(0x80019000 + 24, 1280);
  cpu.GetMemory().Write16(0x80019000 + 26, 0);
  cpu.GetMemory().Write8(0x80019000 + 28, 16);
  cpu.GetMemory().Write8(0x80019000 + 29, 16); // IDIB_COLORSCHEME_565

  // State for compatible bitmaps created by CreateCompatibleBitmap (slot 13)
  struct CompatBitmapState {
    uint32_t width = 320;
    uint32_t height = 240;
  };
  auto compat_state = std::make_shared<CompatBitmapState>();
  // Bump allocators for per-surface compatible bitmaps (see slot 13 below).
  auto compat_next_object = std::make_shared<uint32_t>(0);
  auto compat_next_pixels = std::make_shared<uint32_t>(0);
  // Zenonia's WIPI engine writes its 320x240 RGB565 surface directly through
  // the concrete compatible-IBitmap DIB fields (measured at 0x85000000).
  // Other titles use this generic scaffold differently, so scope the real DIB
  // to the proven module. ZEEB_COMPAT_DIB remains an explicit diagnostic force.
  const bool is_zenonia_title =
      std::string(argv[1]).find("zenonia.mod") != std::string::npos;
  // CreateCompatibleBitmap e contrato geral de IBitmap, nao quirk de Zenonia.
  // Devolver objeto vazio para outros titulos fazia BitBlt ler vtable como pixels.
  const bool compat_dib_enabled = true;
  constexpr uint32_t kCompatBitmapPixels = 0x85000000;
  constexpr uint32_t kCompatBitmapPixelEnd = 0x86000000;
  constexpr uint32_t kCompatBitmapObjectBase = 0x87000000;
  constexpr uint32_t kCompatBitmapObjectEnd = 0x87100000;
  constexpr uint32_t kCompatBitmapBytes = 640u * 480u * 2u;
  if (compat_dib_enabled) {
    // Same start-up panel state as the screen itself: a surface the title has
    // not painted yet reads as cleared white, not black. Gamevil's intro draws
    // its light-green logo over an unpainted background.
    for (uint32_t off = 0; off < kCompatBitmapBytes; off += 4) {
      cpu.GetMemory().Write32(kCompatBitmapPixels + off, 0xFFFFFFFFu);
    }
  }

  std::vector<zeebulator::HleRuntime::HleFunction> compat_bitmap_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  compat_bitmap_methods[0] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); };
  compat_bitmap_methods[1] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); };
  compat_bitmap_methods[2] = [&cpu](zeebulator::IArmCore& core) {
    const uint32_t cls = core.GetRegister(zeebulator::kR1);
    const uint32_t out = core.GetRegister(zeebulator::kR2);
    const bool supported = cls == 0x01001045u || cls == 0x01001021u || cls == 0x0100102cu;
    if (out != 0) cpu.GetMemory().Write32(out, supported ? core.GetRegister(zeebulator::kR0) : 0);
    core.SetRegister(zeebulator::kR0, supported ? 0 : 3);
  };
  // IBitmap slot 3: NativeColor RGBToNative(IBitmap*, RGBVAL).
  // Zeebo's display is RGB565 and the game calls this before every source blit.
  compat_bitmap_methods[3] = [](zeebulator::IArmCore& core) {
    uint32_t rgb = core.GetRegister(zeebulator::kR1);
    uint32_t r = (rgb >> 8) & 0xff;
    uint32_t g = (rgb >> 16) & 0xff;
    uint32_t b = (rgb >> 24) & 0xff;
    core.SetRegister(zeebulator::kR0, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  };
  // IBitmap slot 4: RGBVAL NativeToRGB(IBitmap*, NativeColor).
  compat_bitmap_methods[4] = [](zeebulator::IArmCore& core) {
    uint16_t pixel = static_cast<uint16_t>(core.GetRegister(zeebulator::kR1));
    uint32_t r5 = (pixel >> 11) & 0x1f;
    uint32_t g6 = (pixel >> 5) & 0x3f;
    uint32_t b5 = pixel & 0x1f;
    core.SetRegister(zeebulator::kR0,
                     ((r5 << 3 | r5 >> 2) << 8) |
                     ((g6 << 2 | g6 >> 4) << 16) |
                     ((b5 << 3 | b5 >> 2) << 24));
  };

  // Slots 14/15: Set/GetTransparencyColor(IBitmap*, NativeColor).
  // IDisplay::BitBlt reads the transparent key from this object's own field
  // (offset 16) when the raster op is AEE_RO_TRANSPARENT, so ignoring these
  // left every sprite blit using whatever key the surface was created with.
  compat_bitmap_methods[14] = [&cpu](zeebulator::IArmCore& core) {
    cpu.GetMemory().Write32(core.GetRegister(zeebulator::kR0) + 16,
                            core.GetRegister(zeebulator::kR1));
    core.SetRegister(zeebulator::kR0, 0);
  };
  compat_bitmap_methods[15] = [&cpu](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0,
                     cpu.GetMemory().Read32(core.GetRegister(zeebulator::kR0) + 16));
  };
  // Slot 12: GetInfo(IBitmap*, AEEBitmapInfo *pinfo, int nSize)
  // Reads the geometry back from this specific bitmap object: every
  // CreateCompatibleBitmap call owns its own surface, so shared state would
  // report one bitmap's size for all of them.
  compat_bitmap_methods[12] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t self = core.GetRegister(zeebulator::kR0);
    uint32_t out = core.GetRegister(zeebulator::kR1);
    if (out != 0) {
      cpu.GetMemory().Write32(out + 0, cpu.GetMemory().Read16(self + 20));
      cpu.GetMemory().Write32(out + 4, cpu.GetMemory().Read16(self + 22));
      cpu.GetMemory().Write32(out + 8, 16); // 16-bit color depth (RGB565)
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Real IBitmap drawing operates on this surface's own pixel storage. These
  // were blind stubs, so anything a title drew through IBitmap (a white clear
  // before a logo, sprite blits, scanlines) silently produced nothing and the
  // surface stayed black.
  auto compat_geometry = [&cpu](uint32_t self, uint32_t* w, uint32_t* h, uint32_t* pitch,
                                uint32_t* pixels) {
    auto& m = cpu.GetMemory();
    *pixels = m.Read32(self + 8);
    *w = m.Read16(self + 20);
    *h = m.Read16(self + 22);
    *pitch = m.Read16(self + 24);
    if (*pitch == 0) *pitch = *w * 2;
    return *pixels != 0 && *w != 0 && *h != 0;
  };
  // Slot 5: DrawPixel(IBitmap*, int x, int y, NativeColor color)
  compat_bitmap_methods[5] = [&cpu, compat_geometry](zeebulator::IArmCore& core) {
    uint32_t w, h, pitch, pixels;
    if (compat_geometry(core.GetRegister(zeebulator::kR0), &w, &h, &pitch, &pixels)) {
      int32_t x = static_cast<int32_t>(core.GetRegister(zeebulator::kR1));
      int32_t y = static_cast<int32_t>(core.GetRegister(zeebulator::kR2));
      if (x >= 0 && y >= 0 && x < static_cast<int32_t>(w) && y < static_cast<int32_t>(h)) {
        cpu.GetMemory().Write16(pixels + y * pitch + x * 2,
                                static_cast<uint16_t>(core.GetRegister(zeebulator::kR3)));
      }
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 6: GetPixel(IBitmap*, int x, int y, NativeColor *pColor)
  compat_bitmap_methods[6] = [&cpu, compat_geometry](zeebulator::IArmCore& core) {
    uint32_t w, h, pitch, pixels;
    uint32_t out = core.GetRegister(zeebulator::kR3);
    if (out != 0 && compat_geometry(core.GetRegister(zeebulator::kR0), &w, &h, &pitch, &pixels)) {
      int32_t x = static_cast<int32_t>(core.GetRegister(zeebulator::kR1));
      int32_t y = static_cast<int32_t>(core.GetRegister(zeebulator::kR2));
      uint16_t value = 0;
      if (x >= 0 && y >= 0 && x < static_cast<int32_t>(w) && y < static_cast<int32_t>(h)) {
        value = cpu.GetMemory().Read16(pixels + y * pitch + x * 2);
      }
      cpu.GetMemory().Write32(out, value);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 9: FillRect(IBitmap*, const AEERect *pRect, NativeColor color, AEERasterOp)
  compat_bitmap_methods[9] = [&cpu, compat_geometry](zeebulator::IArmCore& core) {
    uint32_t w, h, pitch, pixels;
    if (compat_geometry(core.GetRegister(zeebulator::kR0), &w, &h, &pitch, &pixels)) {
      auto& m = cpu.GetMemory();
      uint32_t prect = core.GetRegister(zeebulator::kR1);
      int32_t x0 = 0, y0 = 0, rw = static_cast<int32_t>(w), rh = static_cast<int32_t>(h);
      if (prect != 0) {
        x0 = static_cast<int16_t>(m.Read16(prect + 0));
        y0 = static_cast<int16_t>(m.Read16(prect + 2));
        rw = static_cast<int16_t>(m.Read16(prect + 4));
        rh = static_cast<int16_t>(m.Read16(prect + 6));
      }
      uint16_t color = static_cast<uint16_t>(core.GetRegister(zeebulator::kR2));
      for (int32_t y = std::max(0, y0); y < std::min<int32_t>(h, y0 + rh); ++y) {
        for (int32_t x = std::max(0, x0); x < std::min<int32_t>(w, x0 + rw); ++x) {
          m.Write16(pixels + y * pitch + x * 2, color);
        }
      }
      if (std::getenv("ZEEB_LOG_DRAW")) {
        std::fprintf(stderr, "[draw] IBitmap FillRect (%d,%d %dx%d) color=0x%04x\n", x0, y0, rw, rh,
                     color);
      }
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t compat_bitmap_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8008C000, /*object=*/0x8008D000, compat_bitmap_methods);
  if (compat_dib_enabled) {
    auto& compat_mem = cpu.GetMemory();
    compat_mem.Write32(compat_bitmap_obj + 8, kCompatBitmapPixels);
    compat_mem.Write32(compat_bitmap_obj + 16, 0xffffffffu);
    // Zenonia writes exactly 320*240 RGB565 bytes into this surface.
    compat_mem.Write16(compat_bitmap_obj + 20, 320);
    compat_mem.Write16(compat_bitmap_obj + 22, 240);
    compat_mem.Write16(compat_bitmap_obj + 24, 640);
    compat_mem.Write8(compat_bitmap_obj + 28, 16);
  }

  std::vector<zeebulator::HleRuntime::HleFunction> device_bitmap_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  // Slot 2: QueryInterface(IBitmap*, AEECLSID cls, void **ppo)
  device_bitmap_methods[2] = [&cpu, unknown_0x01001045_obj](zeebulator::IArmCore& core) {
    uint32_t requested_cls = core.GetRegister(zeebulator::kR1);
    uint32_t ppo = core.GetRegister(zeebulator::kR2);
    if (requested_cls == 0x01001045) {
      if (ppo != 0) cpu.GetMemory().Write32(ppo, unknown_0x01001045_obj);
      core.SetRegister(zeebulator::kR0, 0);
    } else {
      if (ppo != 0) cpu.GetMemory().Write32(ppo, 0);
      core.SetRegister(zeebulator::kR0, 3); // ECLASSNOTSUPPORT per AEEError.h
    }
  };
  // Slot 12: GetInfo(IBitmap*, AEEBitmapInfo *pinfo, int nSize)
  device_bitmap_methods[12] = [&cpu, kWidth, kHeight](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR1);
    if (out != 0) {
      cpu.GetMemory().Write32(out + 0, kWidth);
      cpu.GetMemory().Write32(out + 4, kHeight);
      cpu.GetMemory().Write32(out + 8, 16); // 16-bit color depth (RGB565)
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 13: CreateCompatibleBitmap(IBitmap*, IBitmap **ppIBitmap, uint16 w, uint16 h)
  // Each call returns its own object with its own pixel storage. Handing the
  // same object and the same buffer to every caller makes concurrently live
  // surfaces (map tiles, sprites, UI) overwrite one another, which shows up as
  // smearing and black frames once a title keeps several of them at once.
  device_bitmap_methods[13] = [&cpu, compat_bitmap_obj, compat_state, compat_dib_enabled,
                                compat_next_object, compat_next_pixels](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR1);
    uint32_t w = core.GetRegister(zeebulator::kR2) & 0xffff;
    uint32_t h = core.GetRegister(zeebulator::kR3) & 0xffff;
    uint32_t width = w != 0 ? std::min(w, 640u) : compat_state->width;
    uint32_t height = h != 0 ? std::min(h, 480u) : compat_state->height;
    compat_state->width = width;
    compat_state->height = height;
    auto& m = cpu.GetMemory();
    uint32_t obj = compat_bitmap_obj;
    if (!compat_dib_enabled) {
      if (out != 0) m.Write32(out, obj);
      core.SetRegister(zeebulator::kR0, 0);
      return;
    }
    if (*compat_next_object == 0) {
      // Primeiro objeto preserva o endereco historico; os seguintes usam arena
      // dedicada, sem atravessar vtables HLE vizinhas.
      *compat_next_object = kCompatBitmapObjectBase;
      *compat_next_pixels = kCompatBitmapPixels + kCompatBitmapBytes;
    } else {
      const uint32_t pixel_bytes = width * height * 2u;
      const uint32_t pixel_step = (pixel_bytes + 0xfffu) & ~0xfffu;
      // Ring buffer continuo: superficies temporarias sao criadas a cada quadro (ex: pelo DrawRollerExt)
      // e liberadas logo apos o desenho. Reutilizar a arena quando atinge o teto previne esgotamento de memoria.
      if (*compat_next_object > kCompatBitmapObjectEnd - 0x100u) {
        *compat_next_object = kCompatBitmapObjectBase;
      }
      if (static_cast<uint64_t>(*compat_next_pixels) + pixel_step > kCompatBitmapPixelEnd) {
        *compat_next_pixels = kCompatBitmapPixels + kCompatBitmapBytes;
      }
      obj = *compat_next_object;
      *compat_next_object += 0x100;
      m.Write32(obj, 0x8008C000);  // vtable compartilhada, objeto independente
    }
    uint32_t pixels = kCompatBitmapPixels;
    if (obj != compat_bitmap_obj) {
      pixels = *compat_next_pixels;
      *compat_next_pixels += ((width * height * 2u) + 0xfffu) & ~0xfffu;
    }
    const uint32_t bytes = width * height * 2u;
    for (uint32_t off = 0; off < bytes; off += 4) m.Write32(pixels + off, 0xFFFFFFFFu);
    m.Write32(obj + 8, pixels);
    m.Write32(obj + 16, 0xffffffffu);
    m.Write16(obj + 20, static_cast<uint16_t>(width));
    m.Write16(obj + 22, static_cast<uint16_t>(height));
    m.Write16(obj + 24, static_cast<uint16_t>(width * 2));
    m.Write8(obj + 28, 16);
    m.Write8(obj + 29, 16);
    if (std::getenv("ZEEB_LOG_DRAW")) {
      std::fprintf(stderr, "[draw] CreateCompatibleBitmap %ux%u -> obj=0x%08x pixels=0x%08x\n",
                   width, height, obj, pixels);
    }
    if (out != 0) m.Write32(out, obj);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };

  uint32_t device_bitmap_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8000E000, /*object=*/0x8000F000, device_bitmap_methods);
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
  file_hle.BuildLastOpenedFileProxy(/*vtable=*/0x80012000, /*object=*/0x80013000);

  // ClsId 0x01001014: AEECLSID_UNZIPSTREAM (IUnzipAStream) from Qualcomm BREW SDK.
  // Decompresses a compressed IAStream (deflate / gzip / zlib) into uncompressed bytes.
  zeebulator::UnzipStreamHle unzip_stream_hle(cpu.GetMemory(), hle, /*object_region_start=*/0x80087000);
  unzip_stream_hle.Build(/*vtable=*/0x80086000);
  shell_hle.RegisterFactory(zeebulator::UnzipStreamHle::kClsidUnzipStream,
                            [&unzip_stream_hle]() { return unzip_stream_hle.AllocateStream(); });
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

  // AEECLSID_SQLMGR (0x0102c4e8) -- o gerenciador de bancos do console.
  // Identificado pela propria saida de debug do jogo, nao por adivinhacao:
  // tectoy.mod chama CreateInstance(0x0102c4e8), recebe ECLASSNOTSUPPORT e
  // imprime literalmente "No SQLMGR: %d" via dbgprintf, e entao TENTA DE NOVO
  // -- 50230 vezes numa varredura de 25M passos, sem nunca chegar a
  // EVT_APP_START. Nao e um titulo que ignora a classe ausente: ele queima o
  // orcamento inteiro de instrucoes repetindo o mesmo pedido.
  //
  // Scaffold generico por enquanto: dar um objeto valido remove o laco de
  // repeticao e deixa o jogo avancar ate o primeiro slot que ele realmente
  // chame, que e o que revela a forma de verdade da interface. As chamadas de
  // slot ficam visiveis em ZEEB_HLE_PROFILE, entao o proximo passo e medido,
  // nao adivinhado.
  // ISQLMgr de verdade, sobre SQLite (core/brew/sql_hle.h). O scaffold
  // generico que estava aqui foi instrumentado antes de ser substituido:
  // logando indice de slot, r0-r3 e LR, o tectoy.mod mostrou chamar
  // exatamente slot 3 do ISQLMgr com "tt_prefs.db" + ponteiro de saida,
  // e depois slot 3 do objeto devolvido com "PRAGMA integrity_check" e
  // "SELECT version, subversion FROM DBINFO". Com o stub devolvendo
  // sempre 0 e nenhuma linha, o jogo imprimia "Invalid database
  // version..." e "Failed to init Preferences database". Os bancos que
  // ele quer sao arquivos SQLite reais que vem no proprio pacote do
  // jogo (tt_prefs.db, 4096 bytes, comeca com "SQLite format 3").
  // Faixa dedicada 0x800D0000..0x800E0000 para ISQLMgr / ISQLDatabase.
  // IMPORTANTE: os enderecos anteriores (0x8006C000 / 0x8006E000) colidiam
  // frontalmente com kWidgetVtable (0x8006C000) e com sound_obj (0x8006E000).
  // A cada banco aberto, sql_hle sobrescrevia a vtable do widget, fazendo a
  // Z-Wheel saltar para um endereco invalido no EVT_APP_START.
  zeebulator::SqlHle sql_hle(cpu.GetMemory(), hle, /*db_object_region_start=*/0x800D3000,
                             /*scratch_address=*/0x800D8000, /*scratch_size=*/0x4000);
  {
    // Copia-para-gravavel: o banco original que veio com o jogo nunca e
    // aberto direto, porque o SQLite grava nele (journal, PRAGMA,
    // INSERT) e o pacote do jogo e material de pesquisa que precisa
    // continuar intacto. A copia vai para o diretorio de dados do usuario,
    // nao para a midia da ROM: um strace da Z-Wheel mostrou este processo
    // abrindo tt_prefs.db e asset_cache com O_RDWR|O_CREAT dentro de
    // /media/.../debug_nand, que e removivel e pode estar so para leitura.
    namespace fs = std::filesystem;
    std::string mod_path = argv[1];
    const fs::path db_dir = fs::path(data_dir) / (module_key + ".sqldb");
    const fs::path legacy_db_dir = fs::path(mod_path + ".sqldb");
    fs::path mod_dir = fs::absolute(mod_path).parent_path();
    sql_hle.SetPathResolver([db_dir, legacy_db_dir, mod_dir, &vfs](const std::string& name) -> std::string {
      std::error_code ec;
      fs::create_directories(db_dir, ec);
      std::string base = fs::path(name).filename().string();
      if (base.empty()) return std::string();
      fs::path target = db_dir / base;
      if (!fs::exists(target)) {
        // Semente: primeiro o VFS do proprio jogo, depois o arquivo solto
        // ao lado do .mod. Se nao houver nenhum dos dois, o SQLite cria
        // um banco vazio -- que e o que o console faria na primeira vez.
        // Convencao antiga: copie como semente, mas jamais abra/grave o DB
        // ao lado do .mod. Isso preserva progresso sem voltar a poluir a ROM.
        if (fs::exists(legacy_db_dir / base, ec)) {
          fs::copy_file(legacy_db_dir / base, target, fs::copy_options::overwrite_existing, ec);
        } else if (const std::vector<uint8_t>* data = vfs.Find(base)) {
          std::ofstream out(target, std::ios::binary);
          out.write(reinterpret_cast<const char*>(data->data()),
                    static_cast<std::streamsize>(data->size()));
        } else if (fs::exists(mod_dir / base)) {
          fs::copy_file(mod_dir / base, target, ec);
        }
      }
      return target.string();
    });
  }
  uint32_t sqlmgr_obj = sql_hle.BuildManager(/*mgr_vtable=*/0x800D0000, /*mgr_object=*/0x800D1000,
                                             /*db_vtable=*/0x800D2000);
  shell_hle.RegisterInstance(/*AEECLSID_SQLMGR=*/0x0102c4e8, sqlmgr_obj);

  // 0x01028e51 -- o widget da interface da Z-Wheel, inclusive o formulario raiz.
  //
  // O nome "ROOTFORM" que este projeto usava vem da mensagem de erro do proprio
  // jogo ("Could not create root form"), nao de um header: e um apelido, nao a
  // identidade da classe.
  //
  // O QUE IMPORTA AQUI E A CONVENCAO DE RETORNO, QUE E INVERTIDA. Medido no
  // guest: tectoy.mod chama o slot 3 com r1=0x800, e os dois inv�lucros que o
  // envolvem fazem `cmp r0,#0; moveq r0,#3`, ou seja, transformam ZERO em
  // EBADCLASS. Neste acessador, portanto, DIFERENTE DE ZERO E SUCESSO -- o
  // oposto do resto do BREW. O scaffold generico devolvia 0, que aqui significa
  // "falhou", e era exatamente isso que fazia tectoymain.c imprimir
  // "Could not create root form(20)" e morrer logo depois num ponteiro nulo.
  //
  // A inversao vale SO para o acessador. O slot 2 e um QueryInterface comum, e
  // ali zero e sucesso (o jogo faz `movs r5,r0; bne <erro>`). O slot 12 segue a
  // convencao do 2. Misturar as duas seria facil, por isso estao lado a lado.
  //
  // Seletores do acessador `slot3(this, seletor, id, valor)`:
  //   0x800 -> PEGA O FILHO de numero `id` e escreve o ponteiro em [valor]
  //   0x801 -> GRAVA a propriedade `id` com `valor`
  constexpr uint32_t kWidgetVtable = 0x8006C000;
  constexpr uint32_t kWidgetObject = 0x8006D000;  // prototipo; factories usam objetos unicos
  constexpr uint32_t kWidgetInstanceBase = 0x86000000;
  constexpr uint32_t kWidgetInstanceEnd = 0x87000000;
  constexpr uint32_t kWidgetInstanceStride = 0x100;
  // ATENCAO (bug corrigido): as duas tabelas sao indexadas por (this, id), NAO
  // so por id. O codigo antigo usava `id` puro, como se existisse UM widget no
  // sistema. Existem varios: a instrumentacao mostrou o guest falando com
  // 0x8006d000, 0x8006d100, 0x8006d140 e 0x8006c000 na MESMA execucao, e o
  // item 0x5000 e justamente o que cada formulario usa para pendurar o seu
  // conteudo. Com a tabela compartilhada, pendurar o item 0x5000 de um widget
  // sobrescrevia o do outro e as propriedades numericas vazavam entre eles --
  // exatamente o tipo de colisao que o documento da roda descreve ao separar
  // "propriedade numerica" de "objeto pendurado".
  auto widget_props = std::make_shared<std::map<uint64_t, uint32_t>>();
  auto widget_children = std::make_shared<std::map<uint64_t, uint32_t>>();
  auto widget_ref_counts = std::make_shared<std::map<uint32_t, uint32_t>>();
  auto widget_classes = std::make_shared<std::map<uint32_t, uint32_t>>();
  auto widget_parents = std::make_shared<std::map<uint32_t, uint32_t>>();
  auto widget_visibility = std::make_shared<std::map<uint32_t, bool>>();
  auto widget_extents = std::make_shared<std::map<uint32_t, std::array<uint32_t, 2>>>();
  auto next_widget_object = std::make_shared<uint32_t>(kWidgetInstanceBase);
  // Estrutura para armazenar tratadores de eventos de widget (slot 4) e desenho (slot 16)
  struct WidgetHandler {
    uint32_t function = 0;
    uint32_t context = 0;
    uint32_t destructor = 0;
  };
  auto widget_handlers = std::make_shared<std::map<uint32_t, WidgetHandler>>();
  auto widget_draw_callbacks = std::make_shared<std::map<uint32_t, WidgetHandler>>();
  struct RegisteredWidgetHandler { uint32_t object = 0; WidgetHandler handler; };
  auto registered_widget_handlers = std::make_shared<std::vector<RegisteredWidgetHandler>>();

  std::vector<zeebulator::HleRuntime::HleFunction> widget_methods(
      24, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 20); });
  widget_methods[0] = [widget_ref_counts](zeebulator::IArmCore& core) {
    uint32_t& refs = (*widget_ref_counts)[core.GetRegister(zeebulator::kR0)];
    if (refs == 0) refs = 1;
    if (refs != 0xffffffffu) ++refs;
    core.SetRegister(zeebulator::kR0, refs);
  };
  widget_methods[1] = [widget_ref_counts](zeebulator::IArmCore& core) {
    auto it = widget_ref_counts->find(core.GetRegister(zeebulator::kR0));
    if (it == widget_ref_counts->end()) { core.SetRegister(zeebulator::kR0, 0); return; }
    if (it->second > 1) --it->second;
    core.SetRegister(zeebulator::kR0, it->second);
  };
  // Slot 2 / 12: QueryInterface / PegarInterface.
  // Conforme o documento da roda §5.2 ("QueryInterface de widget permissivo"):
  // A familia inteira de widgets (forms, containers, ownerdraw, stage) compartilha
  // a mesma interface. Devolver `this` para qualquer IID com sucesso (0) e o que
  // permite que classes irmas consultem umas as outras (ex: 0x0101593c em 0x13d4c0)
  // sem abortar a cadeia de decodificacao de imagens e layout.
  widget_methods[2] = [&cpu, widget_ref_counts](zeebulator::IArmCore& core) {
    const uint32_t self = core.GetRegister(zeebulator::kR0);
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) {
      cpu.GetMemory().Write32(out, self);
    }
    (*widget_ref_counts)[self]++;
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  widget_methods[12] = widget_methods[2]; // Slot 12: PegarInterface

  // Slot 3: Acessador (le/grava propriedades e filhos)
  widget_methods[3] = [&cpu, widget_props, widget_children, widget_ref_counts,
                       next_widget_object](zeebulator::IArmCore& core) {
    const uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    const uint32_t selector = core.GetRegister(zeebulator::kR1);
    const uint32_t id = core.GetRegister(zeebulator::kR2);
    const uint32_t value = core.GetRegister(zeebulator::kR3);
    // Chave por (this, id): ver o comentario das duas tabelas acima.
    const uint64_t key = (static_cast<uint64_t>(this_obj) << 32) | id;
    constexpr uint32_t kPrimeiroObjeto = 0x5000;
    if (selector == 0x800) {
      // Itens tipados (comprovado no Zeebx e disassembly de AnimationVideo_Form):
      // id >= 0x5000 sao objetos/widgets; abaixo de 0x5000 sao numeros/propriedades!
      if (id >= kPrimeiroObjeto) {
        auto it = widget_children->find(key);
        if (it == widget_children->end()) {
          if (*next_widget_object > kWidgetInstanceEnd - kWidgetInstanceStride) {
            core.SetRegister(zeebulator::kR0, 0);
            return;
          }
          const uint32_t child = *next_widget_object;
          *next_widget_object += kWidgetInstanceStride;
          cpu.GetMemory().Write32(child, kWidgetVtable);
          (*widget_ref_counts)[child] = 1;
          it = widget_children->emplace(key, child).first;
        }
        if (value != 0) cpu.GetMemory().Write32(value, it->second);
      } else {
        auto it = widget_props->find(key);
        uint32_t val = (it != widget_props->end()) ? it->second : 0;
        if (value != 0) cpu.GetMemory().Write32(value, val);
      }
      core.SetRegister(zeebulator::kR0, 1);  // != 0 = sucesso NESTE acessador
      return;
    }
    if (selector == 0x801) {
      if (id >= kPrimeiroObjeto) (*widget_children)[key] = value;
      else (*widget_props)[key] = value;
      core.SetRegister(zeebulator::kR0, 1);
      return;
    }
    // Seletor 0x711: grava propriedade
    if (selector == 0x711) {
      (*widget_props)[key] = value;
      core.SetRegister(zeebulator::kR0, 1);
      return;
    }
    // Consultas de estado/evento no root widget (0x101, 0x7b0a, 0x7b0f):
    // devolver 0 (FALSE) permite que o tratador do proprio applet as resolva.
    if (selector == 0x101 || selector == 0x7b0a || selector == 0x7b0e || selector == 0x7b0f) {
      core.SetRegister(zeebulator::kR0, 0);
      return;
    }
    core.SetRegister(zeebulator::kR0, 1);
  };
  // Slot 4: DefinirTratador (SetHandler / SetCallback)
  // Slot 4 SetHandler(this, &{fn, ctx, dtor}).
  //
  // BREW chains handlers: the previous registration must be written BACK into
  // the caller's own struct, because that struct is what the new handler reads
  // to tail-jump when it does not consume an event. Leaving the struct
  // describing the handler that just registered makes that tail-jump recurse
  // into itself: measured in the reference implementation as 250 million
  // instructions in a single frame with no error logged, because there is no
  // error, only a loop. With no previous handler, BREW writes zero and the
  // caller checks for exactly that.
  widget_methods[4] = [&cpu, widget_handlers, registered_widget_handlers](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    uint32_t ptr = core.GetRegister(zeebulator::kR1);
    if (ptr != 0) {
      uint32_t fn = cpu.GetMemory().Read32(ptr + 0);
      uint32_t ctx = cpu.GetMemory().Read32(ptr + 4);
      uint32_t dtor = cpu.GetMemory().Read32(ptr + 8);
      WidgetHandler previous{};
      auto it = widget_handlers->find(this_obj);
      if (it != widget_handlers->end()) previous = it->second;
      cpu.GetMemory().Write32(ptr + 0, previous.function);
      cpu.GetMemory().Write32(ptr + 4, previous.context);
      cpu.GetMemory().Write32(ptr + 8, previous.destructor);
      (*widget_handlers)[this_obj] = WidgetHandler{fn, ctx, dtor};
      registered_widget_handlers->push_back(
          RegisteredWidgetHandler{this_obj, WidgetHandler{fn, ctx, dtor}});
      if (std::getenv("ZEEB_LOG_WIDGET")) {
        std::fprintf(stderr,
                     "[widget] SetHandler obj=0x%08x fn=0x%08x ctx=0x%08x prev=0x%08x/0x%08x\n",
                     this_obj, fn, ctx, previous.function, previous.context);
      }
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 5: AdicionarFilho
  // Geometria por (pai, filho): o bloco de seis palavras do slot 5.
  // Ver o comentario dentro do handler sobre por que a leitura e VALIDADA em
  // vez de assumida.
  auto widget_geometry = std::make_shared<std::map<uint64_t, std::array<uint32_t, 6>>>();
  // Imagem associada a cada widget por IInterfaceModel::SetIPtr. Medido na
  // Z-Wheel: o guest pede o modelo ao widget (slot 12, AEEIID_IInterfaceModel =
  // 0x0101593c) e, no que recebe, chama o slot 5 com
  // (piBase = o IImage, clsidType = AEEIID_IImage = 0x01013110). Como o nosso
  // slot 12 devolve o proprio widget, essa chamada cai aqui.
  auto widget_images = std::make_shared<std::map<uint32_t, uint32_t>>();
  widget_methods[5] = [&cpu, widget_geometry, widget_parents, widget_images,
                       log_image](zeebulator::IArmCore& core) {
    const uint32_t parent = core.GetRegister(zeebulator::kR0);
    const uint32_t child = core.GetRegister(zeebulator::kR1);
    // SetIPtr(pif, piBase, AEEIID_IImage): isto NAO e adicionar um filho. Sem
    // este desvio a imagem do widget virava "geometria" e nada era desenhado.
    if (core.GetRegister(zeebulator::kR2) == 0x01013110u) {
      (*widget_images)[parent] = child;
      if (log_image) {
        std::fprintf(stderr, "[image] SetIPtr widget=0x%08x imagem=0x%08x\n", parent, child);
      }
      core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
      return;
    }
    if (child != 0) (*widget_parents)[child] = parent;
    uint32_t r2 = core.GetRegister(zeebulator::kR2);
    // Bloco de posicao (documento da roda, secao 6.2): quando o terceiro
    // argumento e um PONTEIRO, ele aponta seis palavras:
    //   {x, y, sinalizador, largura, altura, objeto}
    // O mesmo slot atende cinco classes e nem toda chamada tem essa forma
    // (algumas passam funcao em r2 e objeto em r3), entao a leitura e
    // VALIDADA antes de virar estado: alinhado, endereco de guest plausivel, e
    // geometria dentro de 0..4096 com x+w e y+h dentro da tela. Sem essa
    // validacao, um valor pequeno como 4 (o getter de passo de lista, logo
    // abaixo) ou um ponteiro de funcao viraria "posicao" e o desenho iria para
    // a origem -- que e exatamente o sintoma que o documento descreve
    // ("tudo era pintado na origem e a tela era um amontoado no canto").
    // FORMA REAL DA CHAMADA (medida com ZEEB_LOG_WIDGET_ALL no tectoy.mod, seis
    // chamadas em 50 s passivos):
    //   r2=0, r3=0x0038ffdc  (lr=0x101cd8)  -> bloco de posicao em r3
    //   r2=0, r3=0x0038ffa0  (lr=0x1753e8)  -> idem
    //   r2=1, r3=0xf00006b4  (lr=0x16df7c)  -> outra forma (objeto/ponteiro de trap)
    //   r2=trap, r3=0x8007c000 (lr=0x1752b4) -> outra forma (r3 e o objeto de fonte)
    // Ou seja: o ponteiro do bloco e o TERCEIRO argumento (r3) e aparece quando
    // o segundo (r2) e zero -- que e a mesma regra que o documento da roda
    // enuncia ("so leia a posicao quando r2 == 0"), so que com o ponteiro no
    // argumento seguinte. A leitura e validada de qualquer forma: uma forma de
    // chamada que nao seja geometria nao pode virar estado de desenho.
    const uint32_t position_ptr = (r2 == 0) ? core.GetRegister(zeebulator::kR3) : 0;
    if (r2 == 0 && (position_ptr & 3u) == 0 && position_ptr >= 0x1000 &&
        position_ptr < 0x90000000u) {
      r2 = position_ptr;
      auto& m = cpu.GetMemory();
      std::array<uint32_t, 6> block{};
      for (int i = 0; i < 6; ++i) block[i] = m.Read32(r2 + i * 4);
      const uint32_t x = block[0], y = block[1], w = block[3], h = block[4];
      // Largura/altura ZERADAS sao legitimas: o documento da roda registra que
      // o widget que se mede sozinho pendura {x, y, 1, 0, 0, ...}, e que gravar
      // zero por cima do que o slot 7 (SetExtent) ja disse seria o erro. A
      // primeira versao desta validacao exigia h != 0 e RECUSOU dois blocos
      // reais (x=148 y=20 e x=0 y=0, ambos com w=h=0) -- comparar com o
      // documento e que mostrou que o errado era o filtro, nao o guest.
      const bool plausible = x <= 4096 && y <= 4096 && w <= 4096 && h <= 4096 &&
                             (w == 0 || x + w <= 4096) && (h == 0 || y + h <= 4096);
      if (plausible) {
        (*widget_geometry)[(static_cast<uint64_t>(parent) << 32) | child] = block;
      }
      if (std::getenv("ZEEB_LOG_WIDGET_ALL")) {
        std::fprintf(stderr,
                     "[wgeom] pai=0x%08x filho=0x%08x ptr=0x%08x -> x=%u y=%u flag=0x%x w=%u h=%u "
                     "obj=0x%08x %s\n",
                     parent, child, r2, x, y, block[2], w, h, block[5],
                     plausible ? "ACEITO" : "RECUSADO (nao parece geometria)");
      }
      core.SetRegister(zeebulator::kR0, 0);  // SUCCESS
      return;
    }
    if (r2 == 4) {
      // Passo de lista pedido pelo slot 5 (medido em 0x8fa04 do tectoy.mod)
      uint32_t out = core.GetRegister(zeebulator::kR1);
      if (out != 0) {
        constexpr int16_t kPasso = 18;
        cpu.GetMemory().Write16(out + 0, static_cast<uint16_t>(kPasso));
        cpu.GetMemory().Write16(out + 2, 0);
      }
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 6: DefinirVisivel
  widget_methods[6] = [widget_visibility](zeebulator::IArmCore& core) {
    (*widget_visibility)[core.GetRegister(zeebulator::kR0)] = core.GetRegister(zeebulator::kR1) != 0;
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 7: DefinirTamanho
  widget_methods[7] = [widget_extents](zeebulator::IArmCore& core) {
    (*widget_extents)[core.GetRegister(zeebulator::kR0)] =
        {core.GetRegister(zeebulator::kR1), core.GetRegister(zeebulator::kR2)};
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 8: SetPos / Layout (chamado em 0x13d58c com this, child, 0, &pos).
  // NUNCA escreva em r1! r1 e o widget filho, nao um ponteiro de saida. O codigo
  // antigo interpretava r1 como &out_parent e sobrescrevia o ponteiro de vtable do filho!
  widget_methods[8] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 13: mesma ABI de IBitmap::CreateCompatibleBitmap. Reusar a factory
  // real acima: cada chamada ganha objeto, pixels RGB565 e geometria proprios.
  widget_methods[13] = device_bitmap_methods[13];
  // Slot 14: Anexar / Attach (associa widget ou modelo)
  widget_methods[14] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 15: Devolve o proprio widget (`this`) como fabrica de superficies / bitmap.
  // Medido em DrawRollerExt (tectoy.mod 0x11fdf4): chama slot 15 e no ponteiro devolvido
  // chama slot 13 (CreateCompatibleBitmap) para alocar as superficies de 214x34 e 440x49.
  widget_methods[15] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, core.GetRegister(zeebulator::kR0));
  };
  // Slot 16: OwnerDraw / RegisterDrawCallback (tectoy.mod 0x22d58)
  // Slot 16 SetDrawHandler(this, &{draw, ctx, dtor}) -- same chaining rule as
  // slot 4: the trio's first two words receive the PREVIOUS entry. The drawer
  // installed by CreateOwnerDrawWidget is a chain link that calls
  // [ctx+0x14]([ctx+0x18], ...) first, so without the write-back it calls
  // itself. The third word (the destructor) is not tracked here.
  widget_methods[16] = [&cpu, widget_draw_callbacks](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    uint32_t ptr = core.GetRegister(zeebulator::kR1);
    if (ptr != 0) {
      uint32_t fn = cpu.GetMemory().Read32(ptr + 0);
      uint32_t ctx = cpu.GetMemory().Read32(ptr + 4);
      uint32_t dtor = cpu.GetMemory().Read32(ptr + 8);
      WidgetHandler previous{};
      auto it = widget_draw_callbacks->find(this_obj);
      if (it != widget_draw_callbacks->end()) previous = it->second;
      cpu.GetMemory().Write32(ptr + 0, previous.function);
      cpu.GetMemory().Write32(ptr + 4, previous.context);
      cpu.GetMemory().Write32(ptr + 8, previous.destructor);
      (*widget_draw_callbacks)[this_obj] = WidgetHandler{fn, ctx, dtor};
      if (std::getenv("ZEEB_LOG_WIDGET")) {
        std::fprintf(stderr,
                     "[widget] SetDrawHandler obj=0x%08x fn=0x%08x ctx=0x%08x prev=0x%08x/0x%08x\n",
                     this_obj, fn, ctx, previous.function, previous.context);
      }
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 17: SetModel / SetFont (tectoy.mod 0x23860 e 0x23d0c: associa modelo 0x8000 / fonte ao roller)
  widget_methods[17] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Instrumentacao pedida pelo documento da roda (secao 5.1): "por callback,
  // entrou / voltou / abortou, e por que". Recusar um slot aborta o callback do
  // jogo em SILENCIO -- quem abortou foi o emulador, nao o guest, entao nao ha
  // nada no log do jogo. Com ZEEB_LOG_WIDGET_ALL=1 cada chamada de cada slot
  // sai com argumentos, chamador e valor devolvido. Envolve ANTES do
  // BuildInterfaceObject para que a vtable aponte para o envoltorio.
  if (std::getenv("ZEEB_LOG_WIDGET_ALL") != nullptr) {
    for (size_t i = 0; i < widget_methods.size(); ++i) {
      zeebulator::HleRuntime::HleFunction inner = widget_methods[i];
      widget_methods[i] = [i, inner](zeebulator::IArmCore& core) {
        const uint32_t a0 = core.GetRegister(zeebulator::kR0);
        const uint32_t a1 = core.GetRegister(zeebulator::kR1);
        const uint32_t a2 = core.GetRegister(zeebulator::kR2);
        const uint32_t a3 = core.GetRegister(zeebulator::kR3);
        const uint32_t lr = core.GetRegister(zeebulator::kLR);
        inner(core);
        std::fprintf(stderr,
                     "[wslot] slot=%zu this=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x -> r0=0x%08x\n",
                     i, a0, a1, a2, a3, lr, core.GetRegister(zeebulator::kR0));
      };
    }
  }

  zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kWidgetVtable, kWidgetObject, widget_methods);
  // O codigo antigo registrava kWidgetObject como singleton para NOVE classes.
  // Agora cada CreateInstance recebe identidade propria e estado por `this`.
  auto allocate_widget = [&cpu, widget_ref_counts, widget_classes, next_widget_object](uint32_t cls) {
    if (*next_widget_object > kWidgetInstanceEnd - kWidgetInstanceStride) return 0u;
    const uint32_t obj = *next_widget_object;
    *next_widget_object += kWidgetInstanceStride;
    cpu.GetMemory().Write32(obj, kWidgetVtable);
    (*widget_ref_counts)[obj] = 1;
    (*widget_classes)[obj] = cls;
    return obj;
  };
  // Registra toda a familia de widgets da Z-Wheel
  const uint32_t kZWheelWidgetClasses[] = {
    0x01028e51, // root form / widget principal
    0x01028e05, // widget do palco (StageWidget)
    0x01028e14, // OwnerDrawWidget do roller inferior
    0x01028e19, // widget de imagem
    0x01028e26, // widget de cor/fundo
    0x01028e2a, // widget de texto / rotulo
    0x01028e36, // widget de instrucoes do z-pad
    0x01028e3f, // container visual da barra e formulários
    0x01028e47, // formulario visual
  };
  for (uint32_t cls : kZWheelWidgetClasses) {
    shell_hle.RegisterFactory(cls, [allocate_widget, cls]() { return allocate_widget(cls); });
  }

  // 0x0100104f -- a colecao generica da Z-Wheel: guarda itens e e percorrida.
  // Registrada com scaffold generico por enquanto; nenhum slot dela foi medido
  // ainda, entao qualquer forma especifica seria adivinhacao. O objetivo aqui e
  // so parar de recusar a classe, ja que uma recusa faz o jogo desistir.
  // 0x0100104f -- a colecao generica da Z-Wheel: guarda itens e e percorrida.
  //
  // ATENCAO AO ENDERECO: a primeira tentativa registrou esta classe em
  // vtable=0x8006E000/object=0x8006F000, que sao EXATAMENTE os enderecos do
  // AEECLSID_SOUND alguns blocos abaixo. O resultado foi EVT_APP_START estourar
  // em pc=0x8006e008 -- e a leitura obvia ("o stub generico da colecao quebra o
  // jogo") estava ERRADA: o que quebrava era a vtable do ISOUND sendo
  // sobrescrita. Registrada aqui numa faixa livre, 0x80071000/0x80072000.
  //
  // Continua um scaffold: nenhum slot desta classe foi medido ainda. O objetivo
  // e so parar de recusar a classe, ja que a recusa faz o jogo desistir. Se um
  // slot for exercitado, aparece no ZEEB_HLE_PROFILE.
  // 0x0100104f -- a colecao generica da Z-Wheel (ICollection).
  // Metodos: 0=AddRef, 1=Release, 4=AtEnd, 5=Reset, 7=GetCurrent, 10=Definir.
  struct CollectionState {
    std::vector<uint32_t> items;
    size_t cursor = 0;
  };
  auto collection_instances = std::make_shared<std::map<uint32_t, CollectionState>>();
  constexpr uint32_t kCollectionVtable = 0x80071000;
  static uint32_t next_collection_obj = 0x80072000;

  std::vector<zeebulator::HleRuntime::HleFunction> collection_methods(
      24, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
  collection_methods[0] = [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 1); };
  collection_methods[1] = [collection_instances](zeebulator::IArmCore& c) {
    collection_instances->erase(c.GetRegister(zeebulator::kR0));
    c.SetRegister(zeebulator::kR0, 0);
  };
  collection_methods[4] = [collection_instances](zeebulator::IArmCore& c) {
    uint32_t this_obj = c.GetRegister(zeebulator::kR0);
    auto it = collection_instances->find(this_obj);
    bool at_end = (it == collection_instances->end()) || (it->second.cursor >= it->second.items.size());
    c.SetRegister(zeebulator::kR0, at_end ? 1u : 0u);
  };
  collection_methods[5] = [collection_instances](zeebulator::IArmCore& c) {
    uint32_t this_obj = c.GetRegister(zeebulator::kR0);
    auto it = collection_instances->find(this_obj);
    if (it != collection_instances->end()) it->second.cursor = 0;
    c.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  collection_methods[7] = [&cpu, collection_instances](zeebulator::IArmCore& c) {
    uint32_t this_obj = c.GetRegister(zeebulator::kR0);
    uint32_t out = c.GetRegister(zeebulator::kR1);
    auto it = collection_instances->find(this_obj);
    if (it != collection_instances->end() && it->second.cursor < it->second.items.size()) {
      if (out != 0) cpu.GetMemory().Write32(out, it->second.items[it->second.cursor]);
      it->second.cursor++;
      c.SetRegister(zeebulator::kR0, 0); // SUCCESS
    } else {
      c.SetRegister(zeebulator::kR0, 1); // EFAILED
    }
  };
  shell_hle.RegisterFactory(/*AEECLSID_COLLECTION=*/0x0100104f, [&cpu, &hle, collection_methods, collection_instances]() {
    uint32_t obj = next_collection_obj;
    next_collection_obj += 0x100;
    (*collection_instances)[obj] = CollectionState{};
    return zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, kCollectionVtable, obj, collection_methods);
  });

  // Extensoes da Z-Wheel (menu principal do console / tectoy.mod):
  // 1) 0x01028e3c: Classe28e3c (AEECLSID_28E3C).
  // tectoymain.c cria duas instancias e guarda em +0x354 e +0x358.
  // Conforme medido no Zeebx (machine.rs:2950-2980):
  // - slot 3: chamado em 0x8588c e 0x858a8 com r1=&saida. Escreve 0x18 bytes zerados!
  // - slot 5: chamado em 0x86238 com r1=&saida. Escreve uint16 0!
  // Recusar o slot 3 abortava a cadeia inteira em silêncio (1722 callbacks).
  std::vector<zeebulator::HleRuntime::HleFunction> class_28e3c_methods(
      16, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  class_28e3c_methods[0] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); };
  class_28e3c_methods[1] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); };
  class_28e3c_methods[3] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t saida = core.GetRegister(zeebulator::kR1);
    if (saida != 0) {
      for (uint32_t off = 0; off < 0x18; off += 4) {
        cpu.GetMemory().Write32(saida + off, 0);
      }
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  class_28e3c_methods[5] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t saida = core.GetRegister(zeebulator::kR1);
    if (saida != 0) {
      cpu.GetMemory().Write16(saida, 0);
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  constexpr uint32_t kClass28e3cVtable = 0x80073000;
  static uint32_t next_28e3c_obj = 0x80074000;
  shell_hle.RegisterFactory(0x01028e3c, [&cpu, &hle, class_28e3c_methods]() {
    uint32_t obj = next_28e3c_obj;
    next_28e3c_obj += 0x100;
    return zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, kClass28e3cVtable, obj, class_28e3c_methods);
  });

  // 2) 0x01011810: ICM (Call Manager / AEECLSID_CM). tectoymain.c:1037
  //    chama o slot 28 (GetPhInfo) com buffer de 0x340 bytes e espera a palavra
  //    em +0x0c igual a 5 (SYS_OPRT_MODE_ONLINE, radio online).
  std::vector<zeebulator::HleRuntime::HleFunction> cm_methods(
      32, [](zeebulator::IArmCore& c) { c.SetRegister(zeebulator::kR0, 0); });
  cm_methods[28] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      // oprt_mode = SYS_OPRT_MODE_ONLINE (5)
      cpu.GetMemory().Write32(pinfo + 0x0c, 5);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t cm_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80075000, /*object=*/0x80076000, cm_methods);
  shell_hle.RegisterInstance(0x01011810, cm_obj);

  // 3) 0x01006c02: OEM_LCTSystemCtl (controle do sistema/luzes). tectoymain.c:1759.
  //    Slot 6 chamado em laco; 0 significa 'sucesso/siga'.
  uint32_t sysctl_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80077000, /*object=*/0x80078000, /*slot_count=*/10);
  shell_hle.RegisterInstance(0x01006c02, sysctl_obj);

  // 4) Fonte e Typeface TrueType da Z-Wheel (AEECLSID_TYPEFACE = 0x01035156, AEECLSID_ROLLER_FONT = 0x0102f67c)
  // Objeto de Fonte concreto (vtable 0x8007B000 / object 0x8007C000):
  std::vector<zeebulator::HleRuntime::HleFunction> font_methods(
      16, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  // Slot 4: GetTextExtent(IFont *po, const AECHAR *pcText, int nChars, int nMaxWidth, int *pnFits)
  // No roller (tectoy.mod 0x238dc), chama Slot 4 com r1=&extent { width: int16, height: int16 }
  font_methods[4] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR1);
    if (out != 0) {
      cpu.GetMemory().Write16(out, 640);
      cpu.GetMemory().Write16(out + 2, 50);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t font_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8007B000, /*object=*/0x8007C000, font_methods);
  shell_hle.RegisterInstance(/*AEECLSID_ROLLER_FONT=*/0x0102f67c, font_obj);

  // Typeface TrueType (vtable 0x80079000 / object 0x8007A000):
  // Slot 4: CriarFonte(ITypeface*, const char *face, int size, int style, IFont **ppFont)
  // O ponteiro de saida ppFont vem no primeiro argumento da pilha (sp[0])!
  std::vector<zeebulator::HleRuntime::HleFunction> typeface_methods(
      10, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  typeface_methods[4] = [&cpu, font_obj](zeebulator::IArmCore& core) {
    uint32_t sp = core.GetRegister(zeebulator::kSP);
    uint32_t out_ptr = cpu.GetMemory().Read32(sp);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, font_obj);
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  uint32_t typeface_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80079000, /*object=*/0x8007A000, typeface_methods);
  shell_hle.RegisterInstance(/*AEECLSID_TYPEFACE=*/0x01035156, typeface_obj);

  // 5) 0x01028e35: IVectorModel (lista generica da Z-Wheel / PREFSDB_GetRecords).
  // Registrado como FACTORY independente para que os diversos chamadores (GLDB,
  // DLQueue, PREFSDB) nao compartilhem nem limpem a lista uns dos outros!
  struct VectorState {
    std::vector<uint32_t> items;
    uint32_t free_fn = 0;
  };
  auto vector_instances = std::make_shared<std::map<uint32_t, VectorState>>();
  constexpr uint32_t kVectorVtable = 0x8007D000;
  static uint32_t next_vector_obj = 0x8007E000;

  std::vector<zeebulator::HleRuntime::HleFunction> vector_methods(
      16, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  vector_methods[0] = [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 1); };
  vector_methods[1] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    // Na liberacao, limpa itens
    auto it = vector_instances->find(this_obj);
    if (it != vector_instances->end()) {
      it->second.items.clear();
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 5: Tamanho (retorna quantidade de itens)
  vector_methods[5] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    auto it = vector_instances->find(this_obj);
    uint32_t sz = (it != vector_instances->end()) ? static_cast<uint32_t>(it->second.items.size()) : 0u;
    core.SetRegister(zeebulator::kR0, sz);
  };
  // Slot 6: PegarEm(uint32_t index, uint32_t *out)
  vector_methods[6] = [&cpu, vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    uint32_t index = core.GetRegister(zeebulator::kR1);
    uint32_t out = core.GetRegister(zeebulator::kR2);
    auto it = vector_instances->find(this_obj);
    if (it != vector_instances->end() && index < it->second.items.size()) {
      if (out != 0) cpu.GetMemory().Write32(out, it->second.items[index]);
      core.SetRegister(zeebulator::kR0, 0); // SUCCESS
    } else {
      core.SetRegister(zeebulator::kR0, 1); // EBADPARM
    }
  };
  // Slot 8: InserirEm(uint32_t index, uint32_t item)
  vector_methods[8] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    uint32_t index = core.GetRegister(zeebulator::kR1);
    uint32_t item = core.GetRegister(zeebulator::kR2);
    auto& state = (*vector_instances)[this_obj];
    if (index == ~0u || index >= state.items.size()) {
      state.items.push_back(item);
    } else {
      state.items.insert(state.items.begin() + index, item);
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 9: RemoverEm(uint32_t index)
  vector_methods[9] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    uint32_t index = core.GetRegister(zeebulator::kR1);
    auto it = vector_instances->find(this_obj);
    if (it != vector_instances->end() && index < it->second.items.size()) {
      it->second.items.erase(it->second.items.begin() + index);
      core.SetRegister(zeebulator::kR0, 0); // SUCCESS
    } else {
      core.SetRegister(zeebulator::kR0, 1);
    }
  };
  // Slot 10: Esvaziar / Clear
  vector_methods[10] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    auto it = vector_instances->find(this_obj);
    if (it != vector_instances->end()) {
      it->second.items.clear();
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 12: DefinirLiberador
  vector_methods[12] = [vector_instances](zeebulator::IArmCore& core) {
    uint32_t this_obj = core.GetRegister(zeebulator::kR0);
    (*vector_instances)[this_obj].free_fn = core.GetRegister(zeebulator::kR1);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  shell_hle.RegisterFactory(/*AEECLSID_VETOR=*/0x01028e35, [&cpu, &hle, vector_methods, vector_instances]() {
    uint32_t obj = next_vector_obj;
    next_vector_obj += 0x100;
    (*vector_instances)[obj] = VectorState{};
    return zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, kVectorVtable, obj, vector_methods);
  });

  // 6) 0x01006c05: ZEEBOMCP
  uint32_t zeebomcp_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8009A000, /*object=*/0x8009B000, /*slot_count=*/10);
  shell_hle.RegisterInstance(/*AEECLSID_ZEEBOMCP=*/0x01006c05, zeebomcp_obj);

  // 7) 0x01001027: IConfig (GetItem / SetItem)
  uint32_t config_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8009C000, /*object=*/0x8009D000, /*slot_count=*/10);
  shell_hle.RegisterInstance(/*AEECLSID_CONFIG=*/0x01001027, config_obj);

  // 8) 0x01001011: ISourceUtil + ISource + IGetLine (IPeek)
  // Essential for Z-Wheel: reads tectoy.cfg line by line (functions 0x178338 & 0x17a2f8).
  // Without this, tectoymain.c aborts reading system preferences and halts before the boot animation.
  constexpr uint32_t kSourceVtable = 0x800A2000;
  constexpr uint32_t kSourceObj = 0x800A3000;
  constexpr uint32_t kGetLineVtable = 0x800A4000;
  constexpr uint32_t kGetLineObj = 0x800A5000;
  constexpr uint32_t kLineBufferAddr = 0x800A6000; // Buffer de linha em memória guest

  struct ActivePeekSource {
    std::string content;
    size_t cursor = 0;
  };
  auto active_source = std::make_shared<ActivePeekSource>();

  // Tabela de métodos de ISource (5 slots: AddRef, Release, QueryInterface, Read, Readable)
  std::vector<zeebulator::HleRuntime::HleFunction> source_methods(
      10, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  source_methods[2] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) cpu.GetMemory().Write32(out, core.GetRegister(zeebulator::kR0));
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t source_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kSourceVtable, kSourceObj, source_methods);

  // Tabela de métodos de IGetLine / IPeek (10 slots)
  // Slot 8: int32 GetLine(IGetLine *po, struct GetLine *pgl, int32 nTypeEOL)
  std::vector<zeebulator::HleRuntime::HleFunction> getline_methods(
      12, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  getline_methods[2] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) cpu.GetMemory().Write32(out, core.GetRegister(zeebulator::kR0));
    core.SetRegister(zeebulator::kR0, 0);
  };
  getline_methods[8] = [&cpu, active_source](zeebulator::IArmCore& core) {
    uint32_t pgl = core.GetRegister(zeebulator::kR1);
    if (active_source->cursor >= active_source->content.size()) {
      // Fim do arquivo: retorna -1 (ISOURCE_END / IGETLINE_END) e nLen = 0
      if (pgl != 0) {
        cpu.GetMemory().Write32(pgl + 0, 0); // psz = NULL
        cpu.GetMemory().Write32(pgl + 4, 0); // nLen = 0
      }
      core.SetRegister(zeebulator::kR0, static_cast<uint32_t>(-1));
      return;
    }
    // Extrai a próxima linha
    size_t next_nl = active_source->content.find('\n', active_source->cursor);
    std::string line;
    if (next_nl == std::string::npos) {
      line = active_source->content.substr(active_source->cursor);
      active_source->cursor = active_source->content.size();
    } else {
      line = active_source->content.substr(active_source->cursor, next_nl - active_source->cursor);
      active_source->cursor = next_nl + 1;
    }
    // Remove eventual \r no final
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Grava a linha no buffer guest
    for (size_t i = 0; i < line.size(); ++i) {
      cpu.GetMemory().Write8(kLineBufferAddr + static_cast<uint32_t>(i), static_cast<uint8_t>(line[i]));
    }
    cpu.GetMemory().Write8(kLineBufferAddr + static_cast<uint32_t>(line.size()), 0); // null terminator

    if (pgl != 0) {
      cpu.GetMemory().Write32(pgl + 0, kLineBufferAddr);
      cpu.GetMemory().Write32(pgl + 4, static_cast<uint32_t>(line.size()));
      cpu.GetMemory().Write8(pgl + 8, 0); // bLeftover = FALSE
      cpu.GetMemory().Write8(pgl + 9, 0); // bTruncated = FALSE
    }
    constexpr uint32_t kIgetLineLf = 3;
    core.SetRegister(zeebulator::kR0, kIgetLineLf); // Sucesso: linha lida
  };
  uint32_t getline_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kGetLineVtable, kGetLineObj, getline_methods);

  // Tabela de métodos de ISourceUtil (10 slots)
  std::vector<zeebulator::HleRuntime::HleFunction> source_util_methods(
      12, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  source_util_methods[2] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) cpu.GetMemory().Write32(out, core.GetRegister(zeebulator::kR0));
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 3: GetLineFromSource(po, pis, nBufSize, ppigl)
  source_util_methods[3] = [&cpu, getline_obj](zeebulator::IArmCore& core) {
    uint32_t out_pp = core.GetRegister(zeebulator::kR3);
    if (out_pp != 0) cpu.GetMemory().Write32(out_pp, getline_obj);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 5 & 6: SourceFromFile / SourceFromMemory
  auto make_source_from_file = [&cpu, source_obj, active_source, &file_hle](zeebulator::IArmCore& core) {
    // SourceFromFile receives the guest IFile object in R1 and creates an
    // independent source over THAT file. Do not substitute tectoy.cfg: Z-Wheel
    // uses this same API for a font map, and returning config text made its
    // parser report "missing colon" then wander at pc=0.
    uint32_t file = core.GetRegister(zeebulator::kR1);
    uint32_t out_pp = core.GetRegister(zeebulator::kR2);
    auto bytes = file_hle.SnapshotOpenFile(file);
    if (!bytes.has_value()) {
      if (out_pp != 0) cpu.GetMemory().Write32(out_pp, 0);
      core.SetRegister(zeebulator::kR0, 1);  // EFAILED
      return;
    }
    active_source->content.assign(bytes->begin(), bytes->end());
    active_source->cursor = 0;
    if (out_pp != 0) cpu.GetMemory().Write32(out_pp, source_obj);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  source_util_methods[5] = make_source_from_file;
  source_util_methods[6] = make_source_from_file;

  uint32_t source_util_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8009E000, /*object=*/0x8009F000, source_util_methods);
  shell_hle.RegisterInstance(/*AEECLSID_SOURCE_UTIL=*/0x01001011, source_util_obj);
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
  gl_hle.SetGlObject(gl_obj);
  shell_hle.RegisterInstance(/*AEECLSID_GL=*/0x01014bc3, gl_obj);
  uint32_t egl_obj = gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80009000, /*object=*/0x8000A000);
  gl_hle.SetEglObject(egl_obj);
  shell_hle.RegisterInstance(/*AEECLSID_EGL=*/0x01014bc4, egl_obj);
  // AEECLSID_QEGL (0x0103d8ec): Qualcomm EGL/GLES unified class per zeebx machine.rs
  shell_hle.RegisterInstance(/*AEECLSID_QEGL=*/0x0103d8ec, egl_obj);

  uint32_t gles11_obj = gl_hle.BuildGles11(cpu.GetMemory(), hle, /*vtable=*/0x80088000, /*object=*/0x80089000);
  gl_hle.SetGles11Object(gles11_obj);

  // Surface manipulation / Imageon extension interfaces queried via EGL QueryInterface
  uint32_t surface_manip_obj = gl_hle.BuildSurfaceManip(
      cpu.GetMemory(), hle, /*vtable=*/0x8008A000, /*object=*/0x8008B000);
  (void)surface_manip_obj;

  // Image decoder / ForceFeed scaffolding for AEECLSID_PNG (0x01004004),
  // AEECLSID_WINBMP (0x01004001), AEECLSID_JPEG (0x01004005), AEECLSID_GIF (0x01004003),
  // and AEECLSID_PNGDECODER (0x01026e23) / AEECLSID_PNGDECODER_BREW (0x01030766).
  // Sized with 20 slots each so IImage (11 slots), IImageDecoder (5 slots),
  // and IForceFeed (5 slots) are fully covered.
  constexpr uint32_t kAeeIidForceFeed = 0x0101eb0b;
  uint32_t force_feed_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80052000, /*object=*/0x80053000, /*slot_count=*/20);

  std::vector<zeebulator::HleRuntime::HleFunction> image_decoder_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  // Slot 2: QueryInterface(IImageDecoder*, AEEIID iid, void **ppo)
  image_decoder_methods[2] = [&cpu, force_feed_obj](zeebulator::IArmCore& core) {
    uint32_t iid = core.GetRegister(zeebulator::kR1);
    uint32_t ppo = core.GetRegister(zeebulator::kR2);
    if (iid == kAeeIidForceFeed) {
      if (ppo != 0) cpu.GetMemory().Write32(ppo, force_feed_obj);
      core.SetRegister(zeebulator::kR0, 0); // SUCCESS
    } else {
      if (ppo != 0) cpu.GetMemory().Write32(ppo, 0);
      core.SetRegister(zeebulator::kR0, 3);  // ECLASSNOTSUPPORT (AEEError.h: 3; 20 is EUNSUPPORTED)
    }
  };
  // Slot 3: GetBitmap(IImageDecoder*, IBitmap **ppiBitmap)
  image_decoder_methods[3] = [&cpu, compat_bitmap_obj](zeebulator::IArmCore& core) {
    uint32_t ppi = core.GetRegister(zeebulator::kR1);
    if (ppi != 0) cpu.GetMemory().Write32(ppi, compat_bitmap_obj);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };

  uint32_t image_decoder_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80054000, /*object=*/0x80055000, image_decoder_methods);

  // IImage object for AEECLSID_PNG / AEECLSID_WINBMP
  std::vector<zeebulator::HleRuntime::HleFunction> image_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  // Slot 4: GetInfo(IImage*, AEEImageInfo *pi)
  res_image_methods[4] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t pi = core.GetRegister(zeebulator::kR1);
    if (pi != 0) {
      cpu.GetMemory().Write16(pi + 0, 320); // cx
      cpu.GetMemory().Write16(pi + 2, 240); // cy
      cpu.GetMemory().Write16(pi + 4, 0);   // nColors (>65535)
      cpu.GetMemory().Write8(pi + 6, 0);    // bAnimated
      cpu.GetMemory().Write16(pi + 8, 320); // cxFrame
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  uint32_t image_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80056000, /*object=*/0x80057000, image_methods);

  shell_hle.RegisterInstance(0x01004004, image_obj); // AEECLSID_PNG
  shell_hle.RegisterInstance(0x01004001, image_obj); // AEECLSID_WINBMP
  shell_hle.RegisterInstance(0x01004003, image_obj); // AEECLSID_GIF
  shell_hle.RegisterInstance(0x01004005, image_obj); // AEECLSID_JPEG
  shell_hle.RegisterInstance(0x01026e23, image_decoder_obj); // AEECLSID_PNGDECODER
  shell_hle.RegisterInstance(0x01030766, image_decoder_obj); // AEECLSID_PNGDECODER_BREW

  // AEECLSID_LICENSE (0x0100100f): ILicense interface from Qualcomm BREW (6 slots).
  // Matches zeebx machine.rs:6567 (purchased module, no expiration).
  std::vector<zeebulator::HleRuntime::HleFunction> license_methods(
      10, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  // Slot 2: boolean IsExpired(ILicense*)
  license_methods[2] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // FALSE
  };
  // Slot 3: AEELicenseType GetInfo(ILicense*, uint32 *pdwExpire)
  license_methods[3] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t pdw_expire = core.GetRegister(zeebulator::kR1);
    if (pdw_expire != 0) cpu.GetMemory().Write32(pdw_expire, 0xFFFFFFFFu); // BV_UNLIMITED
    core.SetRegister(zeebulator::kR0, 0); // LT_NONE
  };
  // Slot 4: int SetUsesRemaining(ILicense*, uint32 nUses)
  license_methods[4] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 1); // EFAILED
  };
  // Slot 5: AEEPriceType GetPurchaseInfo(ILicense*, AEELicenseType *plt, uint32 *pdwExpire, uint32 *pdSeq)
  license_methods[5] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t plt = core.GetRegister(zeebulator::kR1);
    uint32_t pdw_expire = core.GetRegister(zeebulator::kR2);
    uint32_t pd_seq = core.GetRegister(zeebulator::kR3);
    if (plt != 0) cpu.GetMemory().Write8(plt, 0); // LT_NONE
    if (pdw_expire != 0) cpu.GetMemory().Write32(pdw_expire, 0xFFFFFFFFu); // BV_UNLIMITED
    if (pd_seq != 0) cpu.GetMemory().Write32(pd_seq, 0);
    core.SetRegister(zeebulator::kR0, 2); // PT_PURCHASE
  };
  uint32_t license_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80058000, /*object=*/0x80059000, license_methods);
  shell_hle.RegisterInstance(0x0100100f, license_obj); // AEECLSID_LICENSE

  // AEECLSID_SOUND (0x01001056): ISound interface from Qualcomm BREW (15 slots:
  // AddRef, Release, RegisterNotify, Set, Get, SetDevice, PlayTone, PlayToneList,
  // PlayFreqTone, StopTone, Vibrate, StopVibrate, SetVolume, GetVolume, GetResourceCtl).
  // Matches zeebx machine.rs: Set/Get preserve 5 bytes of AEESoundInfo.
  struct SoundInfoState {
    uint8_t info[5] = {0, 0, 0, 0, 0};
  };
  auto sound_state = std::make_shared<SoundInfoState>();
  std::vector<zeebulator::HleRuntime::HleFunction> sound_methods(
      15, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  sound_methods[3] = [sound_state, &cpu](zeebulator::IArmCore& core) {
    // int Set(ISound*, const AEESoundInfo *pInfo)
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      for (int i = 0; i < 5; ++i) sound_state->info[i] = cpu.GetMemory().Read8(pinfo + i);
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  sound_methods[4] = [sound_state, &cpu](zeebulator::IArmCore& core) {
    // int Get(ISound*, AEESoundInfo *pInfo)
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      for (int i = 0; i < 5; ++i) cpu.GetMemory().Write8(pinfo + i, sound_state->info[i]);
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  uint32_t sound_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8006E000, /*object=*/0x8006F000, sound_methods);
  shell_hle.RegisterInstance(/*AEECLSID_SOUND=*/0x01001056, sound_obj);

  // AEECLSID_TEXTCTL_10 (0x01003109) / AEECLSID_TEXTCTL (0x01003209) -- ITextCtl.
  // Interface de controle de texto do BREW (AEEText.h / AEEControls.h, 28 slots).
  // Medido no Zenonia (277455): a biblioteca WBLText cria 0x01003109 via
  // ISHELL_CreateInstance e guarda em m_pITextCtl. Se a classe nao for encontrada
  // (ECLASSNOTSUPPORT), dispara a assercao interna na linha 241 de WBLText.c
  // (":AF![%s:%d]:%s(%d):") e entra num laco infinito (b 0x174634).
  uint32_t textctl_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x800A0000, /*object=*/0x800A1000, /*slot_count=*/28);
  shell_hle.RegisterInstance(0x01003109, textctl_obj);
  shell_hle.RegisterInstance(0x01003209, textctl_obj);
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
  // Input follows the BREW shape: state lives in the HID device, each edge
  // is queued as AEEHIDButtonInfo, and the registered ISignal schedules the
  // guest callback. Directly calling a guessed callback bypassed signal
  // ownership and made several titles ignore otherwise valid keyboard input.
  struct HidButtonEvent { int32_t id; int32_t state; int32_t uid; };
  struct HidSignal { uint32_t callback; uint32_t context; uint32_t device_slot; };
  auto simulated_button_events = std::make_shared<std::deque<HidButtonEvent>>();
  auto hid_signals = std::make_shared<std::unordered_map<uint32_t, HidSignal>>();
  auto pending_hid_signals = std::make_shared<std::deque<uint32_t>>();
  auto hid_button_state = std::make_shared<std::array<bool, kHidButtonUids.size()>>();
  auto registered_button_signal = std::make_shared<uint32_t>(0);
  auto next_signal_object = std::make_shared<uint32_t>(0x80065100);
  auto next_signal_ctl_object = std::make_shared<uint32_t>(0x80065300);
  std::vector<zeebulator::HleRuntime::HleFunction> hid_device_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  hid_device_methods[3] = [](zeebulator::IArmCore& core) {
    // GetDeviceInfo(IHIDDevice*, AEEHIDDeviceInfo*): gamepad, PID, VID, wired.
    uint32_t out = core.GetRegister(zeebulator::kR1);
    if (out != 0) {
      core.GetMemory().Write32(out, 1);       // HID_TYPE_GAMEPAD
      core.GetMemory().Write16(out + 4, 0x0135);
      core.GetMemory().Write16(out + 6, 0x1eaa);
      core.GetMemory().Write32(out + 8, 0);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_device_methods[4] = [](zeebulator::IArmCore& core) {
    uint32_t pstatus = core.GetRegister(zeebulator::kR1);
    if (pstatus != 0) core.GetMemory().Write32(pstatus, 1);
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_device_methods[6] = [hid_button_state](zeebulator::IArmCore& core) {
    // Accept both the real UID and a numerical enumeration index.
    uint32_t requested = core.GetRegister(zeebulator::kR1);
    uint32_t index = 0;
    for (; index < kHidButtonUids.size(); ++index) {
      if (kHidButtonUids[index] == requested) break;
    }
    if (index == kHidButtonUids.size() && requested < kHidButtonUids.size()) index = requested;
    if (index == kHidButtonUids.size()) { core.SetRegister(zeebulator::kR0, 2); return; }
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) {
      core.GetMemory().Write32(out, index);
      core.GetMemory().Write32(out + 4, (*hid_button_state)[index] ? 1 : 0);
      core.GetMemory().Write32(out + 8, kHidButtonUids[index]);
      core.GetMemory().Write32(out + 12, 0);
      core.GetMemory().Write32(out + 16, 1);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_device_methods[7] = [](zeebulator::IArmCore& core) {
    uint32_t pbuttons = core.GetRegister(zeebulator::kR1);
    if (pbuttons != 0) core.GetMemory().Write32(pbuttons, kHidButtonUids.size());
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_device_methods[8] = [registered_button_signal](zeebulator::IArmCore& core) {
    *registered_button_signal = core.GetRegister(zeebulator::kR1);
    if (std::getenv("ZEEB_LOG_HID")) {
      std::fprintf(stderr, "[hid] RegisterForButtonEvent signal=0x%08x\n",
                   *registered_button_signal);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_device_methods[9] = [simulated_button_events](zeebulator::IArmCore& core) {
    // AEEResult GetNextButtonEvent(IHIDDevice*, AEEHIDButtonInfo *pnButtonInfo,
    //   uint32 *pdwTimestamp, boolean *pbDroppedEvents)
    if (simulated_button_events->empty()) {
      uint32_t info_addr = core.GetRegister(zeebulator::kR1);
      if (info_addr != 0) {
        for (uint32_t off = 0; off < 20; off += 4) core.GetMemory().Write32(info_addr + off, 0);
      }
      uint32_t timestamp_addr = core.GetRegister(zeebulator::kR2);
      if (timestamp_addr != 0) core.GetMemory().Write32(timestamp_addr, SDL_GetTicks());
      uint32_t dropped_addr = core.GetRegister(zeebulator::kR3);
      if (dropped_addr != 0) core.GetMemory().Write32(dropped_addr, 0);
      core.SetRegister(zeebulator::kR0, 1);  // no more events (AEE_EFAILED-ish)
      return;
    }
    HidButtonEvent event = simulated_button_events->front();
    simulated_button_events->pop_front();
    int32_t button_id = event.id;
    int32_t state = event.state;
    int32_t button_uid = event.uid;
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
    if (timestamp_addr != 0) core.GetMemory().Write32(timestamp_addr, SDL_GetTicks());
    uint32_t dropped_addr = core.GetRegister(zeebulator::kR3);
    if (dropped_addr != 0) core.GetMemory().Write32(dropped_addr, 0);
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  // Slot 10: GetPositionState(IHIDDevice*, AEEHIDPositionInfo *pPositionInfo)
  hid_device_methods[10] = [](zeebulator::IArmCore& core) {
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      for (uint32_t i = 0; i < 25; ++i) core.GetMemory().Write32(pinfo + i * 4, 0);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 11: GetMinPositionInfo(IHIDDevice*, AEEHIDPositionInfo *pMinInfo)
  hid_device_methods[11] = [](zeebulator::IArmCore& core) {
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      core.GetMemory().Write32(pinfo, 0);  // bRelativeAxes = false
      for (uint32_t i = 1; i < 25; ++i) core.GetMemory().Write32(pinfo + i * 4, static_cast<uint32_t>(-32768));
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 12: GetMaxPositionInfo(IHIDDevice*, AEEHIDPositionInfo *pMaxInfo)
  hid_device_methods[12] = [](zeebulator::IArmCore& core) {
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      core.GetMemory().Write32(pinfo, 0);  // bRelativeAxes = false
      for (uint32_t i = 1; i < 25; ++i) core.GetMemory().Write32(pinfo + i * 4, 32767);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 13: GetAxesInfo(IHIDDevice*, AEEHIDPositionInfo *pAxesInfo)
  hid_device_methods[13] = [](zeebulator::IArmCore& core) {
    uint32_t pinfo = core.GetRegister(zeebulator::kR1);
    if (pinfo != 0) {
      for (uint32_t i = 0; i < 25; ++i) core.GetMemory().Write32(pinfo + i * 4, 0);
      core.GetMemory().Write32(pinfo + 1 * 4, 0x0106c40c);  // X
      core.GetMemory().Write32(pinfo + 2 * 4, 0x0106c4d1);  // Y
      core.GetMemory().Write32(pinfo + 3 * 4, 0x0106c4ce);  // Z
      core.GetMemory().Write32(pinfo + 6 * 4, 0x0106c4cf);  // RZ
    }
    core.SetRegister(zeebulator::kR0, 0);
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
  hid_methods[4] = [](zeebulator::IArmCore& core) {
    // IHID_GetDeviceInfo(handle, AEEHIDDeviceInfo*), used while enumerating.
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) {
      core.GetMemory().Write32(out, 1);
      core.GetMemory().Write16(out + 4, 0x0135);
      core.GetMemory().Write16(out + 6, 0x1eaa);
      core.GetMemory().Write32(out + 8, 0);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  hid_methods[7] = [](zeebulator::IArmCore& core) {
    // GetConnectedDevices reports the simulated pad only to joystick queries;
    // keyboard events use BREW EVT_KEY, not an invented HID keyboard device.
    uint32_t wanted = core.GetRegister(zeebulator::kR1);
    uint32_t device_handles_addr = core.GetRegister(zeebulator::kR2);
    uint32_t device_handles_len = core.GetRegister(zeebulator::kR3);
    uint32_t num_handles_req_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
    bool match = wanted == kHidJoystickDeviceUid;
    if (match && device_handles_addr != 0 && device_handles_len >= 1) {
      core.GetMemory().Write32(device_handles_addr, kSimulatedDeviceHandle);
    }
    if (num_handles_req_addr != 0) core.GetMemory().Write32(num_handles_req_addr, match ? 1 : 0);
    core.SetRegister(zeebulator::kR0, 0);
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
  // ISignal objects own the guest callback. Unlike the old fixed-object
  // shortcut, each CreateSignal returns a distinct object, so connect/button/
  // position registrations cannot alias each other.
  std::vector<zeebulator::HleRuntime::HleFunction> signal_methods(
      4, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  signal_methods[2] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR2);
    if (out != 0) cpu.GetMemory().Write32(out, core.GetRegister(zeebulator::kR0));
    core.SetRegister(zeebulator::kR0, 0);
  };
  signal_methods[3] = [hid_signals, pending_hid_signals](zeebulator::IArmCore& core) {
    uint32_t signal = core.GetRegister(zeebulator::kR0);
    if (hid_signals->count(signal) != 0) pending_hid_signals->push_back(signal);
    core.SetRegister(zeebulator::kR0, 0);
  };
  constexpr uint32_t kSignalVtable = 0x80065000;
  constexpr uint32_t kSignalCtlVtable = 0x80065200;
  // Install shared method tables once. Runtime instances below only need a
  // four-byte object header pointing at the appropriate shared vtable.
  zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, kSignalVtable,
                                   /*object=*/0x80065100, signal_methods);
  std::vector<zeebulator::HleRuntime::HleFunction> signal_ctl_methods(
      6, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  signal_ctl_methods[3] = signal_methods[3];  // ISignalCtl_Set
  signal_ctl_methods[4] = [hid_signals](zeebulator::IArmCore& core) {
    hid_signals->erase(core.GetRegister(zeebulator::kR0));
    core.SetRegister(zeebulator::kR0, 0);
  };
  zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, kSignalCtlVtable,
                                   /*object=*/0x80065300, signal_ctl_methods);

  std::vector<zeebulator::HleRuntime::HleFunction> signal_cb_factory_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  signal_cb_factory_methods[3] = [&cpu, hid_signals, next_signal_object,
                                   next_signal_ctl_object](zeebulator::IArmCore& core) {
    // CreateSignal(factory, IDLECBFUNC, pUser, ISignal**, ISignalCtl**).
    uint32_t callback = core.GetRegister(zeebulator::kR1);
    uint32_t context = core.GetRegister(zeebulator::kR2);
    uint32_t out_signal = core.GetRegister(zeebulator::kR3);
    uint32_t out_ctl = zeebulator::HleRuntime::ReadStackArg(core, 0);
    uint32_t device_slot = 0;
    // pUser layout belongs to the title. Record the member that initially
    // contains our IHIDDevice rather than assuming pUser itself is a device.
    for (uint32_t offset = 0; context != 0 && offset < 0x400; offset += 4) {
      uint32_t member = context + offset;
      if (cpu.GetMemory().Read32(member) == kHidDeviceObject) {
        device_slot = member;
        break;
      }
    }
    uint32_t signal = *next_signal_object;
    *next_signal_object += 4;
    uint32_t control = *next_signal_ctl_object;
    *next_signal_ctl_object += 4;
    cpu.GetMemory().Write32(signal, kSignalVtable);
    cpu.GetMemory().Write32(control, kSignalCtlVtable);
    HidSignal registration{callback, context, device_slot};
    (*hid_signals)[signal] = registration;
    (*hid_signals)[control] = registration;
    if (out_signal != 0) cpu.GetMemory().Write32(out_signal, signal);
    if (out_ctl != 0) cpu.GetMemory().Write32(out_ctl, control);
    core.SetRegister(zeebulator::kR0, 0);
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
  // Give this object the media-style guest layout the notify callback
  // expects (decoded from ddragonz.mod's notify chain): +8 = media
  // source (self-reference) and the rest zeroed. The callback's
  // dispatcher NULL-checks +8 (passes with self) and requires the
  // +0x25 ready byte nonzero to take the vtable[11] path (0 → clean
  // reset path), and the Play helper's vtable[6] call resolves to
  // this object's own stub (success) instead of reading garbage →
  // this eliminates the pc=0x00000000 BX NULL wander.
  {
    auto& iface_mem = cpu.GetMemory();
    iface_mem.Write32(0x80061000 + 8, 0x80061000);
    for (uint32_t off = 0x0c; off < 0x40; off += 4) {
      iface_mem.Write32(0x80061000 + off, 0);
    }
  }
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
      200, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  unknown_0x0103d8ec_methods[2] = [&cpu, &hle, &display, &backend, &abd_font_atlas, &abd_text_state,
                                    kHeight, gles11_obj, is_abd_title](zeebulator::IArmCore& core) {
    // int QueryInterface(iname* _me, AEECLSID clsID, void** ppo) -- real
    uint32_t req_cls = core.GetRegister(zeebulator::kR1);
    uint32_t out_ptr_qi = core.GetRegister(zeebulator::kR2);
    // For non-ABD titles (e.g. Zuma's Revenge, FIFA 09, Ridge Racer, Pac-Mania, Peggle),
    // AEEIID_GLES10 (0x0103d8dd) and AEEIID_GLES11 (0x0103d8ea) return
    // the real OpenGL ES 1.1 interface object (gles11_obj).
    if (!is_abd_title && (req_cls == 0x0103d8dd || req_cls == 0x0103d8ea)) {
      if (out_ptr_qi != 0) {
        cpu.GetMemory().Write32(out_ptr_qi, gles11_obj);
      }
      core.SetRegister(zeebulator::kR0, 0); // SUCCESS
      return;
    }
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
    // Slot 65 (offset 0x104): query status / pending queue. Callers like Alpine Racer EX
    // pass r1 = &status, polling until *r1 == 0 before proceeding with graphics setup.
    stub_methods[65] = [](zeebulator::IArmCore& core) {
      uint32_t out_status_addr = core.GetRegister(zeebulator::kR1);
      if (out_status_addr != 0) {
        core.GetMemory().Write32(out_status_addr, 0);
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
      // Phase 9d producer-stall dump (env-gated ZEEB_WALL_DUMP, stderr-only,
      // non-perturbing: reads memory, changes NO state/returns). Dumps the
      // command-list node structure around the cursor so the successor-link
      // offset can be located. Emits at most a few times per distinct cursor.
      if (std::getenv("ZEEB_WALL_DUMP") != nullptr) {
        static std::map<uint32_t, int> seen;
        static uint32_t last_cur = 0xdeadbeef;
        static int call_n = 0;
        uint32_t cur = core.GetRegister(zeebulator::kR2);
        if (cur != last_cur) {
          std::fprintf(stderr, "[wallseq] call#%d slot33 cursor -> 0x%08x\n", call_n, cur);
          last_cur = cur;
        }
        ++call_n;
        if (cur != 0 && seen[cur]++ < 1) {
          uint32_t desc = cur - 0x30;  // node = descriptor_base + 0x30
          std::fprintf(stderr, "[walldump] slot33 cursor=0x%08x desc=0x%08x\n", cur, desc);
          // Scan desc..desc+0x100 for words that equal any of the 3 known
          // cycle cursors -> locates the successor-link field (node3->node1).
          for (uint32_t o = 0; o < 0x100; o += 4) {
            uint32_t w = core.GetMemory().Read32(desc + o);
            if (w == 0x80324374 || w == 0x80364820 || w == 0x80310a7c) {
              std::fprintf(stderr, "  LINK desc+0x%02x = 0x%08x\n", o, w);
            }
          }
        }
      }
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
    // Env-gated generic render bridge (ZEEB_GENERIC_RENDER, default OFF so ABD
    // and ctest are untouched): routes non-ABD titles' own real slot-107 draw
    // callers through the existing texture-decode/blit path. Confirmed live for
    // torkandkral (folder 280463): its real callers 0x104eb4 (full-screen ATITC
    // background), 0x104c94 (PNG logo/title art), and 0x105150 (cmdlist text
    // cells) pass the exact same real texture signatures (0xccc40002 ATITC /
    // 0x474e5089 PNG) and the same real 16.16 {x0,y0,x1,y1@+8,height@+20}
    // top-down geometry the ABD background caller already handles -- the only
    // reason nothing rasterized was the ABD-specific caller-PC gate below.
    bool generic_render = std::getenv("ZEEB_GENERIC_RENDER") != nullptr;
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
                          bound_texture, pending_fill_color, generic_render,
                          decoded_texture_cache](zeebulator::IArmCore& core) {
      core.SetRegister(zeebulator::kR0, 0);
      // Env-gated geometry probe (ZEEB_GEOM_DUMP): stderr-only, reads memory
      // only, changes NO state -- fires BEFORE the ABD font-atlas gate so it
      // can characterize non-ABD titles (e.g. torkandkral) that reach this
      // slot. Dumps the caller PC, the {x0,y0,x1,y1} geometry struct, and the
      // currently bound texture's signature, deduped by caller PC.
      if (std::getenv("ZEEB_GEOM_DUMP") != nullptr) {
        static std::map<uint32_t, int> seen_caller;
        uint32_t caller = zeebulator::HleRuntime::ReadStackArg(core, 1);
        uint32_t sa = zeebulator::HleRuntime::ReadStackArg(core, 0);
        if (seen_caller[caller]++ < 3) {
          uint32_t tex = *bound_texture;
          uint32_t sig = (tex != 0) ? core.GetMemory().Read32(tex) : 0;
          std::fprintf(stderr,
              "[geom] caller=0x%08x struct=0x%08x x0=%d y0=%d x1=%d y1=%d "
              "far20=%d tex=0x%08x sig=0x%08x fill=%d\n",
              caller, sa,
              sa ? static_cast<int32_t>(core.GetMemory().Read32(sa + 0)) / 65536 : 0,
              sa ? static_cast<int32_t>(core.GetMemory().Read32(sa + 4)) / 65536 : 0,
              sa ? static_cast<int32_t>(core.GetMemory().Read32(sa + 8)) / 65536 : 0,
              sa ? static_cast<int32_t>(core.GetMemory().Read32(sa + 12)) / 65536 : 0,
              sa ? static_cast<int32_t>(core.GetMemory().Read32(sa + 20)) / 65536 : 0,
              tex, sig, pending_fill_color->has_value() ? 1 : 0);
        }
      }
      if (!abd_font_atlas.has_value()) {
        // Generic fill-quad bridge (ZEEB_GENERIC_RENDER) for non-ABD titles
        // that never set an ABD font atlas but DO drive slot 107 with real
        // 16.16 {x0,y0,x1@+8,far@+20} geometry and a real pending fill color
        // (slot 40) and no bound texture -- confirmed live for peggle
        // (folder 278962): its own real slot-107 callers 0x1262a0 (640x480
        // full-screen background quad) and 0x11d000 (smaller UI quads at
        // e.g. 320,240 / 182,278) both arrive with tex=0 fill=1. Without
        // this, slot 107 returned early for every non-ABD title, so nothing
        // ever reached the framebuffer even though the game's own real draw
        // loop was running. Draws the solid-color quad through the same
        // BlitRgba path the ABD 0x1054bc fill branch already uses. Default
        // OFF, so ABD and ctest are untouched.
        if (generic_render) {
          uint32_t sa = zeebulator::HleRuntime::ReadStackArg(core, 0);
          if (sa != 0 && *bound_texture == 0 && pending_fill_color->has_value()) {
            int32_t gx0 = static_cast<int32_t>(core.GetMemory().Read32(sa + 0));
            int32_t gy0 = static_cast<int32_t>(core.GetMemory().Read32(sa + 4));
            int32_t gx1 = static_cast<int32_t>(core.GetMemory().Read32(sa + 8));
            int32_t gfar = static_cast<int32_t>(core.GetMemory().Read32(sa + 20));
            int px = gx0 / 65536;
            int py = gy0 / 65536;
            int pw = std::max(1, (gx1 - gx0) / 65536);
            int ph = std::max(1, (gfar - gy0) / 65536);
            // Sanity clamp: ignore degenerate/oversized quads (cmdlist nodes
            // misread as geometry) -- keep within a generous display bound.
            if (pw <= 4096 && ph <= 4096 && px >= -2048 && py >= -2048) {
              const auto& color = **pending_fill_color;
              if (std::getenv("ZEEB_FILL_DEBUG") != nullptr) {
                std::fprintf(stderr,
                    "[fill] x=%d y=%d w=%d h=%d rgb=%02x%02x%02x\n",
                    px, py, pw, ph, color[0], color[1], color[2]);
              }
              std::vector<uint8_t> fill(static_cast<size_t>(pw) * ph * 4);
              for (size_t i = 0; i < fill.size(); i += 4) {
                fill[i + 0] = color[0];
                fill[i + 1] = color[1];
                fill[i + 2] = color[2];
                fill[i + 3] = 255;
              }
              display.BlitRgba(px, py, pw, ph, fill.data());
            }
          }
        }
        return;
      }
      uint32_t struct_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
      if (struct_addr == 0) return;
      uint32_t real_caller = zeebulator::HleRuntime::ReadStackArg(core, 1);
      // Generic render bridge: a non-ABD title's own real draw callers aren't
      // any of the three ABD-specific caller PCs the texture path below gates
      // on, so map them onto the plainest one (0x104f84 = full-texture,
      // top-down, no crop, no Y-flip) -- confirmed live to match torkandkral's
      // own real {x0,y0,x1@+8,height@+20} background/logo geometry. Its own real
      // 0xccc40002 ATITC / 0x474e5089 PNG textures then decode through the exact
      // same real path ABD backgrounds already use. Any draw whose bound "texture"
      // is really a cmdlist node (text cells) fails the width/height sanity guard
      // below and safely falls through to the shape path, unchanged.
      if (generic_render && real_caller != 0x104f84 && real_caller != 0x1054bc &&
          real_caller != 0x105744) {
        real_caller = 0x104f84;
      }
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
    stub_methods[65] = [](zeebulator::IArmCore& core) {
      // Slot 65: glGetError(IGLES11*, int *pError) -- out-param in R1 must be written 0 (GL_NO_ERROR)
      uint32_t out = core.GetRegister(zeebulator::kR1);
      if (out != 0) core.GetMemory().Write32(out, 0);
      core.SetRegister(zeebulator::kR0, 0);
    };
    uint32_t obj = zeebulator::BuildInterfaceObjectLabeled(
        cpu.GetMemory(), hle, stub_vtable, stub_object, stub_methods,
        "ABD_RENDER_SCAFFOLD", abd_scaffold_slot_names());
    uint32_t out_ptr = core.GetRegister(zeebulator::kR2);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, obj);
    }
    core.SetRegister(zeebulator::kR0, 0);
  };
  // Slot 3: eglGetError(QEGL*, EGLint *pError)
  // No Qualcomm QEGL (AEECLSID_QEGL, 0x0103d8ec), o slot 3 e eglGetError.
  // Medido no Alpine Racer EX (0x1165d4): chama slot 3 com r1 = &sp e compara
  // o valor retornado em [sp] com 0x3000 (EGL_SUCCESS).
  // Sem este slot implementado, o stub retornava 0 sem escrever em [sp],
  // fazendo a checagem de erro falhar e o jogo abortar os graficos (CleanupGraphics).
  unknown_0x0103d8ec_methods[3] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out_err = core.GetRegister(zeebulator::kR1);
    constexpr uint32_t kEglSuccess = 0x3000;
    if (out_err != 0) {
      cpu.GetMemory().Write32(out_err, kEglSuccess);
    }
    core.SetRegister(zeebulator::kR0, kEglSuccess);
  };
  unknown_0x0103d8ec_methods[4] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t interface_ptr = core.GetRegister(zeebulator::kR1);
    uint32_t out_ptr = core.GetRegister(zeebulator::kR2);
    if (out_ptr != 0) {
      cpu.GetMemory().Write32(out_ptr, (interface_ptr != 0) ? interface_ptr : 1);
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
  // Slot 6: Terminate(this, dpy)
  unknown_0x0103d8ec_methods[6] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 9: ChooseConfig(this, dpy, attribs, configs, config_size, &num_config)
  unknown_0x0103d8ec_methods[9] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t configs = core.GetRegister(zeebulator::kR3);
    uint32_t num_config_addr = zeebulator::HleRuntime::ReadStackArg(core, 1);
    if (configs != 0) cpu.GetMemory().Write32(configs, 1); // config handle 1
    if (num_config_addr != 0) cpu.GetMemory().Write32(num_config_addr, 1);
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 11: CreateWindowSurface(this, dpy, config, win, attribs, &out_surface)
  unknown_0x0103d8ec_methods[11] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out_surface = zeebulator::HleRuntime::ReadStackArg(core, 1);
    if (out_surface != 0) {
      cpu.GetMemory().Write32(out_surface, 1); // surface handle 1
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 14: DestroySurface(this, dpy, surface)
  unknown_0x0103d8ec_methods[14] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 16: CreateContext(this, dpy, config, share_ctx, attribs, &out_context)
  unknown_0x0103d8ec_methods[16] = [&cpu](zeebulator::IArmCore& core) {
    uint32_t out_context = zeebulator::HleRuntime::ReadStackArg(core, 1);
    if (out_context != 0) {
      cpu.GetMemory().Write32(out_context, 1); // context handle 1
    }
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 17: DestroyContext(this, dpy, ctx)
  unknown_0x0103d8ec_methods[17] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 18: MakeCurrent(this, dpy, draw, read, ctx)
  unknown_0x0103d8ec_methods[18] = [](zeebulator::IArmCore& core) {
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  // Slot 25: SwapBuffers(this, dpy, surface)
  unknown_0x0103d8ec_methods[25] = [&backend](zeebulator::IArmCore& core) {
    backend.SwapBuffers();
    core.SetRegister(zeebulator::kR0, 0); // SUCCESS
  };
  unknown_0x0103d8ec_methods[65] = [](zeebulator::IArmCore& core) {
    uint32_t out = core.GetRegister(zeebulator::kR1);
    if (out != 0) core.GetMemory().Write32(out, 0);
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
  // IThread (0x01001017) real implementation
  zeebulator::ThreadHle thread_hle(
      cpu.GetMemory(), hle,
      [&mod_runtime](uint32_t sz) { return mod_runtime.Allocate(sz); },
      nullptr);
  thread_hle.SetYieldCallback([&mod_runtime]() { mod_runtime.RequestYield(); });
  shell_hle.SetThreadHle(&thread_hle);
  shell_hle.RegisterFactory(0x01001017, [&thread_hle]() {
    return thread_hle.CreateThreadObject();
  });

  // IHeap (0x01001002) real implementation
  zeebulator::HeapHle heap_hle(
      cpu.GetMemory(), hle,
      [&mod_runtime](uint32_t sz) { return mod_runtime.Allocate(sz); },
      [&mod_runtime](uint32_t ptr, uint32_t sz) { return mod_runtime.Reallocate(ptr, sz); },
      nullptr,
      [&mod_runtime]() { return mod_runtime.GetHeapAvailBytes(); },
      [&mod_runtime]() { return mod_runtime.GetHeapUsedBytes(); });
  uint32_t heap_obj = heap_hle.Build(/*vtable=*/0x80080000, /*object=*/0x80081000);
  shell_hle.RegisterInstance(zeebulator::HeapHle::kClsidHeap, heap_obj);

  // IHash / AEECLSID_MD5 (0x01001015) real implementation. Last missing API
  // across the full 61-title zeebx reference corpus (unblocks Zeeboids).
  zeebulator::HashHle hash_hle(
      cpu.GetMemory(), hle,
      [&mod_runtime](uint32_t sz) { return mod_runtime.Allocate(sz); });
  uint32_t hash_obj = hash_hle.Build(/*vtable=*/0x80082000, /*object=*/0x80083000);
  shell_hle.RegisterInstance(zeebulator::HashHle::kClsidMd5, hash_obj);

  // IMemAStream / AEECLSID_MEMASTREAM (0x0100100c) real implementation.
  // Qualcomm BREW SDK stream over guest memory blocks (image/audio decoders).
  zeebulator::MemAStreamHle mem_astream_hle(cpu.GetMemory(), hle, /*stream_object_region_start=*/0x80085000);
  mem_astream_hle.Build(/*vtable=*/0x80084000);
  shell_hle.RegisterFactory(zeebulator::MemAStreamHle::kClsidMemAStream,
                            [&mem_astream_hle]() { return mem_astream_hle.AllocateStream(); });

  auto run_pending_threads_fn = [&](const char* phase_tag) -> size_t {
    std::printf("[run_pending_threads] %s check: has=%d\n", phase_tag, thread_hle.HasPendingThreads());
    if (!thread_hle.HasPendingThreads()) return 0;
    size_t ran_count = 0;
    for (uint32_t th_obj : thread_hle.TakePendingThreads()) {
      auto* state = thread_hle.GetThreadState(th_obj);
      if (!state || state->finished) continue;
      ++ran_count;

      std::printf("[run_pending_threads] %s running thread 0x%08x pc=0x%08x sp=0x%08x\n",
                  phase_tag, th_obj, state->resume_pc, state->context[13]);

      // Save the main caller's registers and CPSR
      uint32_t saved_regs[16];
      for (int r = 0; r < 16; ++r) saved_regs[r] = cpu.GetRegister(r);
      uint32_t saved_cpsr = cpu.GetCpsr();

      // Load thread registers R0-R12, SP
      for (int r = 0; r <= 12; ++r) cpu.SetRegister(r, state->context[r]);
      cpu.SetRegister(zeebulator::kSP, state->context[13]);
      cpu.SetRegister(zeebulator::kLR, kTrapBase);
      cpu.SetRegister(zeebulator::kPC, state->resume_pc);
      state->suspended = false;

      mod_runtime.ConsumeYieldRequest();
      try {
        auto res = CallArmFunctionChecked(
            cpu, kTrapBase, kBase, mod_size, /*entry=*/0, 0, 0, 0, 0,
            /*trace=*/false, /*hle_trace=*/false, &display, &backend,
            &abd_text_state, /*resume=*/true,
            [&mod_runtime]() { return mod_runtime.ConsumeYieldRequest(); });

        std::printf("  [%s thread] thread 0x%08x returned: r0=%d yielded=%d wandered=%d exceeded=%d suspended=%d finished=%d pc=0x%08x\n",
                    phase_tag, th_obj, res.r0, res.yielded, res.wandered_outside_module, res.exceeded_step_budget,
                    state->suspended, state->finished, cpu.GetRegister(zeebulator::kPC));
        if (res.yielded && !state->finished) {
          if (!state->suspended) {
            // Cooperative yield / sleep: save current PC and state
            for (int r = 0; r <= 12; ++r) state->context[r] = cpu.GetRegister(r);
            state->context[13] = cpu.GetRegister(zeebulator::kSP);
            state->resume_pc = cpu.GetRegister(zeebulator::kPC);
            state->suspended = true;
            thread_hle.EnqueueThread(th_obj);
          }
          // Note: If state->suspended is already true, it was explicitly suspended via
          // IThread::Suspend() which already saved registers and resume_pc correctly.
          // It must wait for a real ISHELL_Resume(resume_cb) before running again!
        } else if (!state->suspended && !state->finished) {
          thread_hle.FinishThread(th_obj, res.r0);
        }
      } catch (const std::exception& e) {
        std::printf("  [%s thread] thread 0x%08x threw: %s (pc=0x%08x)\n",
                    phase_tag, th_obj, e.what(), cpu.GetRegister(zeebulator::kPC));
      }

      // Restore the main caller's registers and CPSR
      for (int r = 0; r < 16; ++r) cpu.SetRegister(r, saved_regs[r]);
      cpu.SetCpsr(saved_cpsr);
    }
    return ran_count;
  };

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
  // Faixa dinamica para build_self_propagating_stub.
  // IMPORTANTE: 0x80070000 colidia com 0x80080000..0x8008D000 (IHash, IMemAStream, etc.)
  // apos apenas 16 objetos (cada objeto avanca 0x1000). Titulos da PopCap como
  // heavyweaponbrew chamam mais de 16 vezes e sobrescreviam 0x80082000 (IHash),
  // gerando aviso de colisao e corrompendo a vtable.
  // A faixa 0x800C0000..0x800F0000 (192 KB) esta completamente desocupada.
  uint32_t next_self_propagating_addr = 0x800C0000;
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
      // Regiao dedicada a pixels de texturas decodificadas.
      // IMPORTANTE: 0x80200000 colidia diretamente com a regiao de objetos de
      // media (0x80200000..0x80280000) e com a tabela estatica/helpers (0x80280000).
      // Cada textura PNG (ex. 943x44x4 = ~162 KB) avancava next_pixel_addr.
      // Apos apenas 3 texturas, next_pixel_addr ultrapassava 0x80280000 e
      // sobrescrevia o ponteiro `free` em 0x8028006c com zeros de pixels,
      // causando o salto fatal para pc=0 no heavyweaponbrew e outros titulos PopCap.
      // A regiao 0x88000000 fica bem acima do heap (0x80300000..0x84300000).
      static uint32_t next_pixel_addr = 0x88000000;
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
  // Zenonia requests MediaPCM directly (0x01005511). Double Dragon uses the
  // same legacy class ID for its download-notification scaffold, so preserve
  // that path and bind real PCM only for the proven WIPI caller.
  if (is_zenonia_title) {
    shell_hle.RegisterFactory(0x01005511,
                               [&media_hle]() { return media_hle.CreateMediaObject(); });
  }
  // Familia AEECLSID_MULTIMEDIA completa, com os nomes vindos do proprio SDK
  // BREW (testkit/shadow_inc/AEEClassIDs.h), nao de adivinhacao:
  //   #define AEECLSID_MULTIMEDIA (QVERSION + 0x5500)   -> 0x01005500
  //   +0 MEDIA  +1 MIDI  +2 MP3  +3 QCP  +4 PMD  +5 MIDIOUTMSG  +6 MIDIOUTQCP
  //   +7 MPEG4  +8 MMF  +9 PHR  +10 ADPCM  +11 AAC  +12 IMELODY  +13 UTIL
  //   +14 AMR  +15 HVS  +16 SAF  +17 PCM  +18 XMF  +19 DLS  +20 SVG
  //
  // Correcao de um erro anterior deste mesmo arquivo: 0x01005502 e 0x01005503
  // tinham sido registradas como "as variantes MIDI/MP3". O SDK mostra que sao
  // MP3 e QCP. O efeito medido estava certo, o nome estava errado -- e o nome
  // importa, porque foi ele que revelou o codec faltante: chessbots.mod pede
  // 0x01005503 (QCP) e passa um buffer cujo cabecalho e
  // "RIFF....QLCMfmt " -- QCP (Qualcomm PureVoice), container RIFF com voz em
  // QCELP/EVRC, que este projeto ainda nao decodifica. Ver as variantes em
  // AEEMediaFormats.h (MM_QCP_FORMAT_FIXED_FULL_13K, _EVRC, _AMR, ...).
  //
  // Registrar toda a familia e melhor que recusar: com ECLASSNOTSUPPORT um
  // jogo pode ficar repetindo o pedido para sempre (o tectoy fazia isso 50230
  // vezes com o SQLMGR). Recebendo o objeto, ele segue e a falha aparece no
  // SetMediaData, que agora diz exatamente qual formato recusou.
  for (uint32_t sibling : {0x01005502u,  // MP3
                            0x01005503u,  // QCP  (PureVoice, ainda sem decoder)
                            0x01005504u,  // PMD
                            0x01005505u,  // MIDIOUTMSG
                            0x01005506u,  // MIDIOUTQCP
                            0x01005508u,  // MMF
                            0x01005509u,  // PHR
                            0x0100550bu,  // AAC
                            0x0100550cu,  // IMELODY
                            0x0100550eu,  // AMR
                            0x01005512u,  // XMF
                            0x01005513u}) {  // DLS
    shell_hle.RegisterFactory(sibling,
                               [&media_hle]() { return media_hle.CreateMediaObject(); });
  }

  // AEECLSID_MEDIAUTIL = 0x0100550d (AEECLSID_MULTIMEDIA + 13).
  //
  // Nao e codec, e a FABRICA de objetos de midia -- por isso nao esta na lista
  // de irmaos acima, que registra apenas classes de formato. O SDK que temos
  // traz a interface E a implementacao de referencia:
  //   platform/media/inc/AEEMediaUtil.h
  //     AEEINTERFACE(IMediaUtil): AddRef, Release, QueryInterface, CreateMedia,
  //     EncodeMedia, CreateMediaEx (6 slots)
  //   platform/media/src/mediautil/AEEMediaUtil.c
  //     CreateMedia: escolhe a classe (extensao -> MIME "audio/<ext>" e depois
  //     "video/<ext>" via ISHELL_GetHandler; sem extensao, le o arquivo e usa
  //     ISHELL_DetectType), cria com ISHELL_CreateInstance e em seguida chama
  //     IMEDIA_SetMediaData(pMedia, pmd) -- que o proprio SDK define como
  //     SetMediaParm(p, MM_PARM_MEDIA_DATA, (int32)pmd, 0), exatamente o
  //     parametro 1 que o MediaHle ja implementa.
  //
  // Quem pede (varredura do literal nos .mod do corpus): tectoy.mod (3 sitios),
  // rocketweb.mod (2), allstarcards.mod (1), quake.mod (1).
  // No tectoy o pedido esta na construcao do formulario de animacao
  // (AnimationVideo_Form.c: literal 0x0100550d em 0x101bc8, CreateInstance em
  // 0x101ac8) e a ausencia dele ERA a causa medida de
  // "Couldn't create animation video form (1)" (tectoymain.c:807): o registrador
  // de retorno comeca em 1 e so vira 0 quando esse objeto existe.
  //
  // Escolha de codec: este projeto nao despacha por classe de formato -- o
  // SetMediaParm fareja o conteudo (ver o comentario de classe do MediaHle).
  // Criar o objeto e entregar o AEEMediaData ao mesmo SetMediaParm mantem UM
  // unico caminho de decodificacao em vez de dois.
  {
    constexpr uint32_t kEbadParm = 14;        // AEEError.h: #define EBADPARM 14
    constexpr uint32_t kEnomemory = 2;        // AEEError.h: #define ENOMEMORY 2
    constexpr uint32_t kEunsupported = 20;    // AEEError.h: #define EUNSUPPORTED 20
    std::vector<zeebulator::HleRuntime::HleFunction> media_util_methods(
        6, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
    media_util_methods[3] = [&cpu, &media_hle](zeebulator::IArmCore& core) {
      // int CreateMedia(IMediaUtil *po, AEEMediaData *pmd, IMedia **ppm)
      const uint32_t pmd = core.GetRegister(zeebulator::kR1);
      const uint32_t ppm = core.GetRegister(zeebulator::kR2);
      auto& m = cpu.GetMemory();
      if (pmd == 0 || ppm == 0) {
        if (ppm != 0) m.Write32(ppm, 0);
        core.SetRegister(zeebulator::kR0, kEbadParm);
        return;
      }
      m.Write32(ppm, 0);  // o real faz *ppm = NULL antes de tentar
      const uint32_t object = media_hle.CreateMediaObject();
      if (object == 0) {
        core.SetRegister(zeebulator::kR0, kEnomemory);
        return;
      }
      const int err = media_hle.ApplyMediaData(core, object, pmd);
      if (err != 0) {
        // O real faz IMEDIA_Release e devolve o erro do SetMediaData. Nao
        // devolvemos objeto: o jogo trata o erro (e foi assim que os codecs
        // faltantes apareceram no log, com o nome do formato recusado).
        if (std::getenv("ZEEB_LOG_MEDIAUTIL")) {
          std::fprintf(stderr,
                       "[mediautil] CreateMedia recusado: SetMediaData devolveu %d (pmd=0x%08x)\n",
                       err, pmd);
        }
        core.SetRegister(zeebulator::kR0, static_cast<uint32_t>(err));
        return;
      }
      if (std::getenv("ZEEB_LOG_MEDIAUTIL")) {
        std::fprintf(stderr, "[mediautil] CreateMedia pmd=0x%08x -> IMedia 0x%08x\n", pmd, object);
      }
      m.Write32(ppm, object);
      core.SetRegister(zeebulator::kR0, 0);
    };
    // Slots 4 e 5 (EncodeMedia, CreateMediaEx) NAO estao implementados, e
    // recusam explicitamente em vez de devolver sucesso vazio: um factory que
    // finge criar midia e exatamente o tipo de stub que o artigo deste projeto
    // acusa. EncodeMedia codifica midia (nenhum titulo do corpus pede ate
    // agora) e CreateMediaEx recebe AEEMediaCreateInfo (lista de AEEMediaDataEx),
    // que este projeto nao decodifica.
    for (int slot : {4, 5}) {
      media_util_methods[slot] = [slot, kEunsupported](zeebulator::IArmCore& core) {
        if (std::getenv("ZEEB_STUB_TRACE")) {
          std::fprintf(stderr, "[mediautil] slot %d (%s) NAO implementado -> EUNSUPPORTED\n", slot,
                       slot == 4 ? "EncodeMedia" : "CreateMediaEx");
        }
        core.SetRegister(zeebulator::kR0, kEunsupported);
      };
    }
    const uint32_t media_util_obj = zeebulator::BuildInterfaceObject(
        cpu.GetMemory(), hle, /*vtable=*/0x8000B400, /*object=*/0x8000B800, media_util_methods);
    shell_hle.RegisterFactory(/*AEECLSID_MEDIAUTIL=*/0x0100550d,
                              [media_util_obj]() { return media_util_obj; });
  }

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
  if (std::getenv("ZEEB_CALLSTACK")) {
    zeebulator::CallStackTracer::Instance().SetEnabled(true);
  }
  zeebulator::CallStackTracer::Instance().RegisterSymbol(kBase, "AEEMod_Load");
  uint32_t applet_ptr = 0;
  uint32_t handle_event_fn = 0;
  bool injected_simulated_download_complete = false;
  bool boot_continuation_active = false;
  // Fase 5: linear execution trace logger (ZEEB_TRACE=lo-hi[,limit][,path]).
  // Env-gated, off by default. When the guest PC enters [lo,hi) the
  // interpreter's OnExec hook appends PC+opcode+regs to the trace file --
  // an ordered instruction stream (complements ZEEB_SPIN_PROFILE's PC
  // histogram). Armed HERE, before AEEMod_Load, so the trace covers the
  // FULL lifecycle incl. boot-time walls that fire during CreateInstance /
  // EVT_APP_START (e.g. the Data East COPROC/SWI null-blx at 0x10350c),
  // not just the post-boot event loop. Observation-only.
  if (const char* tr = std::getenv("ZEEB_TRACE")) {
    uint32_t lo = 0, hi = 0;
    uint64_t limit = 100000;
    std::string path = "zeeb_trace.log";
    // Format: lo-hi[,limit][,path]  (lo/hi hex or dec via strtoul base 0)
    const char* p = tr;
    lo = static_cast<uint32_t>(std::strtoul(p, const_cast<char**>(&p), 0));
    if (*p == '-') hi = static_cast<uint32_t>(std::strtoul(p + 1, const_cast<char**>(&p), 0));
    if (*p == ',') { limit = std::strtoull(p + 1, const_cast<char**>(&p), 0); }
    if (*p == ',') { path = p + 1; }
    if (hi > lo) {
      zeebulator::DebugHooks::Instance().EnableTrace(lo, hi, limit, path);
      std::fprintf(stderr,
                   "[trace] logging PC in [0x%08x,0x%08x) limit=%llu -> %s\n",
                   lo, hi, static_cast<unsigned long long>(limit), path.c_str());
    } else {
      std::fprintf(stderr, "[trace] bad ZEEB_TRACE=\"%s\" (want lo-hi[,limit][,path])\n", tr);
    }
  }
  // Write-watch-with-writer-PC (ZEEB_WWATCH=addr[,len][,path]). Armed here,
  // before AEEMod_Load, so it captures boot-time writers (CreateInstance /
  // EVT_APP_START). Logs each distinct (addr,writer_pc) once. Off by default.
  if (const char* ww = std::getenv("ZEEB_WWATCH")) {
    uint32_t addr = 0;
    uint32_t len = 4;
    std::string path = "zeeb_wwatch.log";
    const char* p = ww;
    addr = static_cast<uint32_t>(std::strtoul(p, const_cast<char**>(&p), 0));
    if (*p == ',') { len = static_cast<uint32_t>(std::strtoul(p + 1, const_cast<char**>(&p), 0)); }
    if (*p == ',') { path = p + 1; }
    if (addr != 0) {
      zeebulator::DebugHooks::Instance().EnableWriteWatchLog(addr, len, path);
      std::fprintf(stderr,
                   "[wwatch] logging writers of [0x%08x,0x%08x) -> %s\n",
                   addr, addr + len, path.c_str());
    } else {
      std::fprintf(stderr, "[wwatch] bad ZEEB_WWATCH=\"%s\" (want addr[,len][,path])\n", ww);
    }
  }
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
    mod_runtime.SetAppletOutAddress(kPpObjAddr);
    // O shell precisa do MESMO endereco: o applet manda evento para a propria
    // classe durante o CreateInstance, antes de o chamador receber o ponteiro.
    shell_hle.SetAppletOutAddress(kPpObjAddr);
    // LoadResString le os .brf do VFS (era um stub ate agora).
    shell_hle.SetVirtualFilesystem(&vfs);
    bool trace_ci = std::getenv("ZEEB_TRACE_CI") != nullptr;
    auto create_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, create_instance_fn,
                                                 module_ptr, shell, cls_id, kPpObjAddr,
                                                 /*trace=*/trace_ci, /*hle_trace=*/trace_ci, &display, &backend);
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
    shell_hle.SetAppletPointer(applet_ptr);
    // SendEvent passa a entregar ao HandleEvent real do applet (ver ishell.cpp).
    shell_hle.SetAppletHandleEvent(handle_event_fn);
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

    // Run any threads started during EVT_APP_START
    run_pending_threads_fn("post-start");

    // Z-Wheel boot animation trigger (AnimationVideo_Form, confirmed via Zeebx & disassembly of 0x101828):
    // The console signals (0x801, 0x5064, 1) to the root widget's handler registered via slot 4.
    // This arms the 1000ms timer (callback 0x1014ac) which advances frames and plays sounds_loading.wav.
    for (const auto& registration : *registered_widget_handlers) {
      const auto& handler = registration.handler;
      if (handler.function != 0) {
        std::printf("[zwheel] Triggering boot animation handler fn=0x%08x ctx=0x%08x\n",
                    handler.function, handler.context);
        try {
          CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, handler.function,
                                 handler.context, 0x801, 0x5064, 1,
                                 /*trace=*/false, /*hle_trace=*/false, &display, &backend);
        } catch (const std::exception& e) {
          std::printf("[zwheel] boot animation trigger threw: %s\n", e.what());
        }
      }
    }

    stage = "HandleEvent(EVT_APP_RESUME)";
    // Root cause of the "first-party tick-1 wall" (AirRacez/Bajaz/Boiaz/
    // JetBoardz/tennis/volley/... — RE'd 2026-09-02): these applets do NOT
    // build their "current scene" object (app+0x64) during EVT_APP_START.
    // Instead APP_START calls the scene-manager (the HLE clsid-0x01001017
    // object at [app+0x60]) slot 7, which SCHEDULES a timer whose callback
    // runs the scene-init FSM (AirRacez 0x110e84) that allocates the scene.
    // The real console drains that timer on the next frame BEFORE the applet
    // is resumed; the probe was firing EVT_APP_RESUME immediately, so RESUME
    // dereferenced the still-null scene (blx [[app+0x34]]+0x34 -> 0x00090024).
    // Fix: drain any timers the START handler scheduled before resuming, the
    // same way the main event loop drains them per frame. Bounded so a title
    // that self-re-arms a timer every frame can't spin here forever; the scene
    // is built on the very first drained tick in practice.
    {
      constexpr int kMaxDrainTicks = 8;
      for (int t = 0; t < kMaxDrainTicks; ++t) {
        auto expired = shell_hle.Tick(16);
        if (expired.empty() && !thread_hle.HasPendingThreads()) break;
        mod_runtime.Tick(16);
        run_pending_threads_fn("pre-resume");
        for (const auto& timer : expired) {
          uint32_t call_r0 = timer.r0_override.value_or(timer.user_data);
          uint32_t call_r1 = timer.r0_override.has_value() ? timer.user_data : 0;
          try {
            mod_runtime.ConsumeYieldRequest();
            auto drained = CallArmFunctionChecked(
                cpu, kTrapBase, kBase, mod_size, timer.callback,
                call_r0, call_r1, 0, 0, /*trace=*/false,
                /*hle_trace=*/false, &display, &backend, nullptr,
                /*resume=*/false,
                [&mod_runtime]() { return mod_runtime.ConsumeYieldRequest(); });
            if (drained.wandered_outside_module || drained.exceeded_step_budget) {
              std::printf("  [pre-resume drain] timer callback did not complete trustworthily\n");
            }
            if (drained.yielded) {
              boot_continuation_active = true;
              break;
            }
          } catch (const std::exception& e) {
            std::printf("  [pre-resume drain] timer callback threw: %s\n", e.what());
          }
        }
        if (boot_continuation_active) break;
      }
    }
    // Standard BREW lifecycle: directly advance to the event loop without faking EVT_APP_RESUME.
    // (EVT_APP_RESUME is only valid when resuming from a previously suspended applet).
  } catch (const std::exception& e) {
    std::printf("%s threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n", stage, e.what(),
                cpu.GetRegister(zeebulator::kPC), cpu.GetRegister(zeebulator::kPC) - kBase);
    std::string trace = zeebulator::CallStackTracer::Instance().FormatStackTrace(cpu);
    if (!trace.empty()) {
      std::printf("Stack trace:\n%s\n", trace.c_str());
    }
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
    std::ifstream state_in(save_state_load_path, std::ios::binary);
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
                ok ? "loaded" : "FAILED to load", save_state_load_path.c_str(),
                gl_ok ? "ok" : "FAILED", audio_ok ? "restored" : "not present in this save file");
    backend.ShowStatusMessage(ok && gl_ok ? "STATE LOADED" : "LOAD FAILED");
  }

  // Queue a physical HID edge, then schedule the exact ISignal the game
  // registered. Guest callbacks are dispatched later in the outer loop, not
  // re-entered from the host event handler.
  auto InjectHidButtonEvent = [&](uint32_t hid_button_uid, bool pressed) {
    uint32_t index = 0;
    for (; index < kHidButtonUids.size(); ++index) {
      if (kHidButtonUids[index] == hid_button_uid) break;
    }
    if (index == kHidButtonUids.size()) return;
    if ((*hid_button_state)[index] == pressed) return;
    (*hid_button_state)[index] = pressed;
    simulated_button_events->push_back(
        HidButtonEvent{static_cast<int32_t>(index), pressed ? 1 : 0,
                       static_cast<int32_t>(hid_button_uid)});
    uint32_t signal = *registered_button_signal;
    if (signal != 0 && hid_signals->count(signal) != 0) {
      pending_hid_signals->push_back(signal);
      if (std::getenv("ZEEB_LOG_HID")) {
        std::fprintf(stderr, "[hid] queued uid=0x%08x state=%d signal=0x%08x\n",
                     hid_button_uid, pressed ? 1 : 0, signal);
      }
    }
  };

  // --- Programmatic control channel (ZEEB_CONTROL_PORT) -------------------
  // An out-of-band TCP/NDJSON server (core/control/control_server.h) lets an
  // agent/script drive the emulator without synthesizing OS keystrokes into
  // an X window. The accept/recv runs on its own thread and only ENQUEUES
  // requests; every command is executed HERE, inline in the tick loop, so it
  // shares the single-threaded CPU/Memory/HLE ownership with zero races.
  // Off unless ZEEB_CONTROL_PORT is set. Names map to the same real Zeebo
  // Z-Pad HID UIDs the keyboard/controller paths use.
  zeebulator::ControlServer control_server;
  auto ButtonUidByName = [](const std::string& n) -> uint32_t {
    if (n == "up") return kHidUidDPadUp;
    if (n == "down") return kHidUidDPadDown;
    if (n == "left") return kHidUidDPadLeft;
    if (n == "right") return kHidUidDPadRight;
    if (n == "back") return kHidUidBack;
    if (n == "button1") return kHidUidButton1;
    if (n == "button2") return kHidUidButton2;
    if (n == "button3") return kHidUidButton3;
    if (n == "button4") return kHidUidButton4;
    if (n == "lshoulder") return kHidUidLeftShoulderUpper;
    if (n == "rshoulder") return kHidUidRightShoulderUpper;
    return 0;
  };
  bool control_wants_quit = false;
  bool dbg_paused = false;  // Fase 2: set when a bp/wp trips; cleared by `cont`.
  // Fase 4: RAM search / cheat finder. Host-side incremental search over
  // guest RAM. rsreset seeds candidates from a [lo,hi) scan at a cell
  // width (1/2/4); rsfilter keeps only candidates matching an op against
  // either an exact value or the previous snapshot; rslist returns the
  // surviving addresses. State lives here (captured by ControlExec) so it
  // persists across IPC calls and is only ever touched on the loop thread.
  struct RamSearch {
    uint32_t lo = 0, hi = 0;
    int width = 4;         // 1, 2 or 4
    bool seeded = false;
    std::vector<uint32_t> addrs;   // surviving candidate addresses
    std::vector<uint32_t> prev;    // last snapshot values, parallel to addrs
  } ramsearch;
  // Advances the guest by exactly `n` real game ticks (drives the same
  // self-rearming ISHELL_SetTimer callbacks the main loop's own timer burst
  // does), presenting between them. Returns after the requested ticks or as
  // soon as the run stops being trustworthy. Declared as a std::function so
  // the command handler below can call it; defined via a lambda that
  // captures the loop state by reference.
  // NOTE: the actual per-tick guest advance is the `shell_hle.Tick` +
  // `mod_runtime.Tick` + `media_hle.Tick` sequence the main loop already
  // runs; `step` just requests N iterations of that to happen, which the
  // main loop honors by looping locally here (it holds no other thread).

  if (const char* cport = std::getenv("ZEEB_CONTROL_PORT")) {
    int port = std::atoi(cport);
    if (port > 0) control_server.Start(port);
  }
  // Loopback HTTP screen mirror (ZEEB_MIRROR_PORT): serves the exact
  // composited frame the native window shows, as PNG, so an out-of-band
  // viewer/agent can watch what's on screen (browser at http://127.0.0.1:
  // <port>/ or GET /frame.png). Frames are published from this GL-owning
  // thread; the HTTP serving runs on the server's own thread. Off unless set.
  zeebulator::MirrorServer mirror_server;
  int mirror_every = 3;  // publish every N ticks (~10fps at ~31fps cadence)
  // Debug UI: ON BY DEFAULT while the emulator is under development. The tabbed
  // debug view (Screen / CPU / BREW API / GPU / Input / Media / Log) is served
  // by the same loopback HTTP server at http://127.0.0.1:<port>/debug. It is a
  // SECOND window/tab in the browser, next to the native SDL window a human
  // watches on Wayland. Toggle off with ZEEB_DEBUG_UI=0. A human on a headless
  // box, or anyone who wants a fixed port, can also set ZEEB_MIRROR_PORT.
  const char* dbg_env = std::getenv("ZEEB_DEBUG_UI");
  bool debug_ui_on = !(dbg_env && std::strcmp(dbg_env, "0") == 0);
  int mirror_port = 0;
  if (const char* mport = std::getenv("ZEEB_MIRROR_PORT")) {
    mirror_port = std::atoi(mport);
  } else if (debug_ui_on) {
    mirror_port = 48750;  // default debug/mirror port
  }
  if (const char* mev = std::getenv("ZEEB_MIRROR_EVERY")) {
    int e = std::atoi(mev);
    if (e > 0) mirror_every = e;
  }
  if (debug_ui_on) zeebulator::DebugSink::Instance().Enable();
  if (mirror_port > 0) {
    mirror_server.Start(mirror_port);
    // Live guest-memory peek for the debug UI's Memory tab (/api/mem).
    // Read-only best-effort snapshot via Memory::Read8 (a const page
    // lookup) -- never mutates guest state, matching the mirror contract.
    mirror_server.SetMemReader([&cpu](uint32_t addr, uint32_t len) {
      static const char* kHex = "0123456789abcdef";
      std::string out;
      out.reserve(static_cast<size_t>(len) * 2);
      auto& mem = cpu.GetMemory();
      for (uint32_t i = 0; i < len; ++i) {
        uint8_t b = mem.Read8(addr + i);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
      }
      return out;
    });
    if (debug_ui_on)
      std::fprintf(stderr, "[debug] tabbed debug UI at http://127.0.0.1:%d/debug\n",
                   mirror_port);
  }
  std::vector<uint8_t> mirror_rgba;  // reused capture buffer
  std::printf("Reached the event loop with no unhandled instruction! Window will stay open.\n");
  // One-time absolute seed (ZEEB_SEED_ABS=addr:value, hex), applied here so it
  // works under BOTH interp and JIT (unlike the PC-hook ZEEB_SEED_63C which is
  // interp-only). Used to plant the +0x63c optional-callback sentinel into the
  // already-constructed (BSS-deterministic) engine object so the guard's beq is
  // taken instead of blx 0. Diagnostic; the address is title-specific.
  if (const char* sa = std::getenv("ZEEB_SEED_ABS")) {
    unsigned addr = 0, val = 0;
    if (std::sscanf(sa, "%x:%x", &addr, &val) == 2 && addr >= 0x1000) {
      uint32_t before = cpu.GetMemory().Read32(addr);
      cpu.GetMemory().Write32(addr, val);
      std::printf("[seedabs] [0x%08x] 0x%08x -> 0x%08x\n", addr, before, val);
    }
  }
  bool running = true;
  // Diagnostic-only: preserves SDL/window events but never forwards keyboard
  // or controller input into guest state, for reproducible passive runs.
  const bool guest_input_disabled = std::getenv("ZEEB_DISABLE_INPUT") != nullptr;
  zeebulator::ZPadState previous_pad_state;
  SDL_Event event;
  constexpr uint32_t kTickMs = 16;
  uint64_t tick_count = 0;
  // Audio is paced by the host clock. A heavy guest frame can take longer
  // than kTickMs; mixing only simulated 16-ms quanta then starves SDL even
  // with correct fractional rounding. Wall pacing keeps PCM pitch normal and
  // leaves the queue prebuffer to cover an individual long callback.
  uint32_t audio_last_mix_ms = SDL_GetTicks();
  uint64_t audio_frame_remainder = 0;
  // A guest callback may cooperatively yield from AEEHelper sleep while its
  // full architectural continuation remains in `cpu`. While active, no other
  // guest callback may run on that single CPU context; resume it first.
  bool callback_continuation_active = boot_continuation_active;
  // Pending programmatic `step`: a step command doesn't fulfill until the
  // guest has actually advanced the requested number of ticks (below).
  bool ctl_step_active = false;
  uint64_t ctl_step_target = 0;
  std::shared_ptr<zeebulator::ControlRequest> ctl_step_req;
  // Executes one control request inline (main thread; safe re: CPU/HLE).
  // `step` is the only deferred command; everything else fulfills here.
  auto ControlExec = [&](const std::shared_ptr<zeebulator::ControlRequest>& req) {
    const std::string& c = req->cmd;
    char buf[256];
    if (c == "ping") {
      req->reply.set_value("{\"ok\":true,\"pong\":true}");
    } else if (c == "press" || c == "down" || c == "up") {
      uint32_t uid = ButtonUidByName(req->button);
      if (uid == 0) {
        req->reply.set_value("{\"ok\":false,\"error\":\"bad button\"}");
      } else {
        if (c == "press") {
          InjectHidButtonEvent(uid, true);
          InjectHidButtonEvent(uid, false);
        } else {
          InjectHidButtonEvent(uid, c == "down");
        }
        req->reply.set_value("{\"ok\":true}");
      }
    } else if (c == "state") {
      std::snprintf(buf, sizeof(buf),
                    "{\"ok\":true,\"tick\":%llu,\"pc\":%u,\"running\":%s}",
                    static_cast<unsigned long long>(tick_count),
                    cpu.GetRegister(zeebulator::kPC), running ? "true" : "false");
      req->reply.set_value(buf);
    } else if (c == "reg") {
      if (!req->has_i0 || req->i0 < 0 || req->i0 > 16) {
        req->reply.set_value("{\"ok\":false,\"error\":\"reg n 0..16\"}");
      } else {
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"n\":%ld,\"value\":%u}", req->i0,
                      cpu.GetRegister(static_cast<int>(req->i0)));
        req->reply.set_value(buf);
      }
    } else if (c == "read") {
      long len = req->has_i1 ? req->i1 : 4;
      if (!req->has_i0 || len <= 0 || len > 256) {
        req->reply.set_value("{\"ok\":false,\"error\":\"read addr + len(1..256)\"}");
      } else {
        std::string hex;
        hex.reserve(static_cast<size_t>(len) * 2);
        for (long i = 0; i < len; ++i) {
          uint8_t byte = cpu.GetMemory().Read8(static_cast<uint32_t>(req->i0) + i);
          char hb[3];
          std::snprintf(hb, sizeof(hb), "%02x", byte);
          hex += hb;
        }
        std::string out = "{\"ok\":true,\"addr\":" + std::to_string(req->i0) +
                          ",\"hex\":\"" + hex + "\"}";
        req->reply.set_value(out);
      }
    } else if (c == "dump") {
      // Full sparse RAM dump to a host file (allocated pages only, via
      // Memory::Serialize's page-index + 4KB format). Raw, deterministic,
      // read-only re: guest state -- the micro-hackathon's RAM-capture
      // primitive (compressible downstream; raw kept for analysis). Opt-in:
      // only reachable when ZEEB_CONTROL_PORT is set (loopback-only).
      std::string path = req->str_path.empty() ? "/tmp/zeeb_ram.bin" : req->str_path;
      std::ofstream ofs(path, std::ios::binary);
      if (!ofs) {
        req->reply.set_value("{\"ok\":false,\"error\":\"cannot open path\"}");
      } else if (!cpu.GetMemory().Serialize(ofs)) {
        req->reply.set_value("{\"ok\":false,\"error\":\"serialize failed\"}");
      } else {
        ofs.close();
        req->reply.set_value("{\"ok\":true,\"path\":\"" + path + "\"}");
      }
    } else if (c == "write") {
      // Hex payload -> bytes written at addr (max 256, mirrors read).
      const std::string& hx = req->str_hex;
      if (!req->has_i0 || hx.empty() || (hx.size() % 2) != 0 || hx.size() > 512) {
        req->reply.set_value(
            "{\"ok\":false,\"error\":\"write addr + hex(even, <=256 bytes)\"}");
      } else {
        auto nyb = [](char ch) -> int {
          if (ch >= '0' && ch <= '9') return ch - '0';
          if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
          if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
          return -1;
        };
        long n = 0;
        bool bad = false;
        for (size_t i = 0; i + 1 < hx.size(); i += 2) {
          int hi = nyb(hx[i]), lo = nyb(hx[i + 1]);
          if (hi < 0 || lo < 0) { bad = true; break; }
          cpu.GetMemory().Write8(static_cast<uint32_t>(req->i0) + n,
                                 static_cast<uint8_t>((hi << 4) | lo));
          ++n;
        }
        if (bad) {
          req->reply.set_value("{\"ok\":false,\"error\":\"non-hex digit\"}");
        } else {
          std::snprintf(buf, sizeof(buf),
                        "{\"ok\":true,\"addr\":%ld,\"len\":%ld}", req->i0, n);
          req->reply.set_value(buf);
        }
      }
    } else if (c == "setreg") {
      if (!req->has_i0 || req->i0 < 0 || req->i0 > 16 || !req->has_val) {
        req->reply.set_value(
            "{\"ok\":false,\"error\":\"setreg n 0..16 + value\"}");
      } else {
        cpu.SetRegister(static_cast<int>(req->i0),
                        static_cast<uint32_t>(req->val));
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"n\":%ld,\"value\":%u}",
                      req->i0, cpu.GetRegister(static_cast<int>(req->i0)));
        req->reply.set_value(buf);
      }
    } else if (c == "stacktrace") {
      std::string trace = zeebulator::CallStackTracer::Instance().FormatStackTrace(cpu);
      std::string esc;
      for (char ch : trace) {
        if (ch == '"') esc += "\\\"";
        else if (ch == '\\') esc += "\\\\";
        else if (ch == '\n') esc += "\\n";
        else esc += ch;
      }
      req->reply.set_value("{\"ok\":true,\"trace\":\"" + esc + "\"}");
    } else if (c == "calltree") {
      std::string tree = zeebulator::CallStackTracer::Instance().FormatCallTree();
      std::string esc;
      for (char ch : tree) {
        if (ch == '"') esc += "\\\"";
        else if (ch == '\\') esc += "\\\\";
        else if (ch == '\n') esc += "\\n";
        else esc += ch;
      }
      req->reply.set_value("{\"ok\":true,\"tree\":\"" + esc + "\"}");
    } else if (c == "bp") {
      if (!req->has_i0) {
        req->reply.set_value("{\"ok\":false,\"error\":\"bp addr\"}");
      } else {
        zeebulator::DebugHooks::Instance().AddBreakpoint(
            static_cast<uint32_t>(req->i0));
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"bp\":%ld}", req->i0);
        req->reply.set_value(buf);
      }
    } else if (c == "bpclear") {
      if (!req->has_i0) {
        // No addr -> clear everything (bps + wps).
        zeebulator::DebugHooks::Instance().ClearAll();
        req->reply.set_value("{\"ok\":true,\"cleared\":\"all\"}");
      } else {
        zeebulator::DebugHooks::Instance().ClearBreakpoint(
            static_cast<uint32_t>(req->i0));
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"bpclear\":%ld}", req->i0);
        req->reply.set_value(buf);
      }
    } else if (c == "watch") {
      long len = req->has_i1 ? req->i1 : 4;
      if (!req->has_i0 || len <= 0) {
        req->reply.set_value("{\"ok\":false,\"error\":\"watch addr + len + mode(r|w|rw)\"}");
      } else {
        auto mode = zeebulator::DebugHooks::kW;
        const std::string& mm = req->str_mode;
        if (mm == "r") mode = zeebulator::DebugHooks::kR;
        else if (mm == "rw") mode = zeebulator::DebugHooks::kRW;
        else mode = zeebulator::DebugHooks::kW;
        zeebulator::DebugHooks::Instance().AddWatchpoint(
            static_cast<uint32_t>(req->i0), static_cast<uint32_t>(len), mode);
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"watch\":%ld,\"len\":%ld,\"mode\":\"%s\"}",
                      req->i0, len, mm.empty() ? "w" : mm.c_str());
        req->reply.set_value(buf);
      }
    } else if (c == "cont") {
      zeebulator::DebugHooks::Instance().ClearHit();
      dbg_paused = false;
      req->reply.set_value("{\"ok\":true,\"resumed\":true}");
    } else if (c == "rsreset") {
      // rsreset lo hi [width]: seed candidates over [lo,hi) at cell width.
      uint32_t lo = req->has_i0 ? static_cast<uint32_t>(req->i0) : 0x80000000u;
      uint32_t hi = req->has_i2 ? static_cast<uint32_t>(req->i2) : (lo + 0x100000u);
      int w = req->has_width ? static_cast<int>(req->width) : 4;
      if (w != 1 && w != 2 && w != 4) w = 4;
      if (hi <= lo) hi = lo + w;
      // Cap the candidate count so a huge range can't blow up memory.
      constexpr size_t kMaxCells = 4u * 1024 * 1024;  // up to 4M candidates
      ramsearch.lo = lo; ramsearch.hi = hi; ramsearch.width = w;
      ramsearch.addrs.clear(); ramsearch.prev.clear();
      auto& mem = cpu.GetMemory();
      auto readcell = [&](uint32_t a) -> uint32_t {
        if (w == 1) return mem.Read8(a);
        if (w == 2) return mem.Read16(a);
        return mem.Read32(a);
      };
      size_t n = 0;
      for (uint32_t a = lo; a + static_cast<uint32_t>(w) <= hi && n < kMaxCells; a += w, ++n) {
        ramsearch.addrs.push_back(a);
        ramsearch.prev.push_back(readcell(a));
      }
      ramsearch.seeded = true;
      std::snprintf(buf, sizeof(buf),
                    "{\"ok\":true,\"seeded\":%zu,\"lo\":%u,\"hi\":%u,\"width\":%d}",
                    ramsearch.addrs.size(), lo, hi, w);
      req->reply.set_value(buf);
    } else if (c == "rsfilter") {
      // rsfilter op[=str_mode] [value]: keep candidates matching op.
      // ops vs exact value: eq ne lt gt   ; vs previous snapshot:
      // inc(>prev) dec(<prev) changed unchanged.
      if (!ramsearch.seeded) {
        req->reply.set_value("{\"ok\":false,\"error\":\"rsreset first\"}");
      } else {
        const std::string& op = req->str_mode;
        uint32_t v = static_cast<uint32_t>(req->val);
        bool have_v = req->has_val;
        auto& mem = cpu.GetMemory();
        int w = ramsearch.width;
        auto readcell = [&](uint32_t a) -> uint32_t {
          if (w == 1) return mem.Read8(a);
          if (w == 2) return mem.Read16(a);
          return mem.Read32(a);
        };
        std::vector<uint32_t> na, np;
        na.reserve(ramsearch.addrs.size());
        np.reserve(ramsearch.addrs.size());
        for (size_t i = 0; i < ramsearch.addrs.size(); ++i) {
          uint32_t a = ramsearch.addrs[i];
          uint32_t cur = readcell(a);
          uint32_t prev = ramsearch.prev[i];
          bool keep = false;
          if (op == "eq") keep = have_v && cur == v;
          else if (op == "ne") keep = have_v && cur != v;
          else if (op == "lt") keep = have_v && cur < v;
          else if (op == "gt") keep = have_v && cur > v;
          else if (op == "inc") keep = cur > prev;
          else if (op == "dec") keep = cur < prev;
          else if (op == "changed") keep = cur != prev;
          else if (op == "unchanged") keep = cur == prev;
          if (keep) { na.push_back(a); np.push_back(cur); }
        }
        ramsearch.addrs.swap(na);
        ramsearch.prev.swap(np);
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"op\":\"%s\",\"remaining\":%zu}",
                      op.c_str(), ramsearch.addrs.size());
        req->reply.set_value(buf);
      }
    } else if (c == "rslist") {
      // rslist [n]: return up to n surviving candidates (addr+value).
      size_t limit = req->has_i0 ? static_cast<size_t>(req->i0) : 32;
      if (limit > 256) limit = 256;
      std::string out = "{\"ok\":true,\"count\":" +
                        std::to_string(ramsearch.addrs.size()) + ",\"width\":" +
                        std::to_string(ramsearch.width) + ",\"cells\":[";
      auto& mem = cpu.GetMemory();
      int w = ramsearch.width;
      auto readcell = [&](uint32_t a) -> uint32_t {
        if (w == 1) return mem.Read8(a);
        if (w == 2) return mem.Read16(a);
        return mem.Read32(a);
      };
      for (size_t i = 0; i < ramsearch.addrs.size() && i < limit; ++i) {
        if (i) out += ",";
        out += "{\"addr\":" + std::to_string(ramsearch.addrs[i]) +
               ",\"value\":" + std::to_string(readcell(ramsearch.addrs[i])) + "}";
      }
      out += "]}";
      req->reply.set_value(out);
    } else if (c == "screenshot") {
      std::string path = req->str_path.empty() ? "/tmp/zeeb_shot.ppm" : req->str_path;
      bool shot_ok = false;
      int w = display.width(), h = display.height();
      // ZEEB_2D_ONLY=1: captura a framebuffer 2D de software em vez do FBO do
      // GL. Sem isto nao da para saber se uma tela 2D "sumiu" porque o guest
      // nao desenhou ou porque o quadro GL e que chega na tela.
      const bool so_2d = std::getenv("ZEEB_2D_ONLY") != nullptr;
      if (!so_2d && backend.HasRealGlActivity()) {
        shot_ok = backend.CaptureScreenshot(path);
      }
      if (!shot_ok) {
        const auto& fb = display.LastPresentedFramebuffer();
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
          std::fprintf(f, "P6\n%d %d\n255\n", w, h);
          for (size_t i = 0; i < fb.size(); ++i) {
            uint16_t px = fb[i];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            uint8_t r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            uint8_t g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            uint8_t b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            uint8_t rgb[3] = {r, g, b};
            std::fwrite(rgb, 1, 3, f);
          }
          std::fclose(f);
          shot_ok = true;
        }
      }
      if (shot_ok) {
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"w\":%d,\"h\":%d,\"path\":\"%s\"}", w, h, path.c_str());
        req->reply.set_value(buf);
      } else {
        req->reply.set_value("{\"ok\":false,\"error\":\"cannot capture screenshot\"}");
      }
    } else if (c == "step") {
      long n = req->has_i0 ? req->i0 : 1;
      if (n < 1) n = 1;
      if (n > 100000) n = 100000;
      ctl_step_active = true;
      ctl_step_target = tick_count + static_cast<uint64_t>(n);
      ctl_step_req = req;  // fulfilled once tick_count reaches target
    } else if (c == "quit") {
      control_wants_quit = true;
      req->reply.set_value("{\"ok\":true}");
    } else {
      req->reply.set_value("{\"ok\":false,\"error\":\"unknown cmd\"}");
    }
  };
  while (running) {
    uint32_t loop_start_ms = SDL_GetTicks();
    // Drain queued programmatic control requests and run them inline.
    if (control_server.IsRunning()) {
      for (auto& req : control_server.Drain()) ControlExec(req);
      if (control_wants_quit) running = false;
    }
    if (const char* shot_env = std::getenv("ZEEB_SHOT_TICK")) {
      static uint64_t shot_target = std::strtoull(shot_env, nullptr, 10);
      static bool shot_done = false;
      if (!shot_done && tick_count >= shot_target) {
        shot_done = true;
        std::string shot_path = "/tmp/zw_shot.ppm";
        backend.CaptureScreenshot(shot_path);
        std::printf("[screenshot] Captured tick=%llu to %s\n", (unsigned long long)tick_count, shot_path.c_str());
      }
    }
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
      if (!guest_input_disabled &&
          (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !event.key.repeat) {
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
        // Dual-channel dispatch (matching Zeemu AppRunner & Zeebx):
        // 1. BREW virtual key code (AVK) via IApplet_HandleEvent (or root form).
        // 2. Physical gamepad UID via IHIDDevice (signals & GetNextButtonEvent).
        uint32_t avk = SdlKeyToAvk(event.key.keysym.sym);
        if (avk != 0 && applet_ptr != 0) {
          constexpr uint32_t kEvtKeyDown = 0x101;
          constexpr uint32_t kEvtKeyUp = 0x102;
          uint32_t evt = (event.type == SDL_KEYDOWN) ? kEvtKeyDown : kEvtKeyUp;
          bool consumed = false;
          // Handler mais novo primeiro. Ignore entradas historicas substituidas;
          // so o handler atualmente instalado em cada objeto pode receber tecla.
          for (auto rit = registered_widget_handlers->rbegin();
               rit != registered_widget_handlers->rend() && !consumed; ++rit) {
            auto current = widget_handlers->find(rit->object);
            if (current == widget_handlers->end() ||
                current->second.function != rit->handler.function ||
                current->second.context != rit->handler.context ||
                current->second.function == 0) continue;
            try {
              const auto wr = CallArmFunctionChecked(
                  cpu, kTrapBase, kBase, mod_size, current->second.function,
                  current->second.context, evt, avk, 0,
                  /*trace=*/false, /*hle_trace=*/false, &display, &backend);
              consumed = wr.r0 != 0 && !wr.wandered_outside_module && !wr.exceeded_step_budget;
            } catch (const std::exception& e) {
              std::fprintf(stderr, "[widget] key handler obj=0x%08x abortou: %s\n",
                           rit->object, e.what());
            }
          }
          try {
            CallResult key_result{};
            if (!consumed) {
              key_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size,
                                                   handle_event_fn, applet_ptr, evt, avk, 0,
                                                   /*trace=*/false, /*hle_trace=*/false, &display,
                                                   &backend);
            }
            key_result.r0 = consumed ? 1 : key_result.r0;
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
        if (hid_button_uid != 0) {
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
    if (!guest_input_disabled && std::getenv("ZEEB_DISABLE_CONTROLLER") == nullptr &&
        backend.HasController()) {
      zeebulator::ZPadState pad_state = zeebulator::NormalizeZPadState(backend.PollInput());
      for (const zeebulator::ZPadButtonEdge& edge :
           zeebulator::DiffZPadButtonEdges(previous_pad_state.buttons, pad_state.buttons)) {
        uint32_t hid_button_uid = ZPadButtonToHidUid(edge.button);
        if (hid_button_uid != 0) InjectHidButtonEvent(hid_button_uid, edge.pressed);
      }
      previous_pad_state = pad_state;
    }

    // BREW signals are asynchronous. Drain only at this event-loop boundary,
    // after host input has been queued and before normal guest timers run.
    if (!dbg_paused && !callback_continuation_active) {
      while (!pending_hid_signals->empty()) {
        uint32_t signal = pending_hid_signals->front();
        pending_hid_signals->pop_front();
        auto it = hid_signals->find(signal);
        if (it == hid_signals->end() || it->second.callback == 0) continue;
        const HidSignal registration = it->second;
        if (std::getenv("ZEEB_LOG_HID")) {
          std::fprintf(stderr, "[hid] dispatch signal=0x%08x callback=0x%08x context=0x%08x device_slot=0x%08x\n",
                       signal, registration.callback, registration.context, registration.device_slot);
        }
        if (registration.device_slot != 0) {
          cpu.GetMemory().Write32(registration.device_slot, kHidDeviceObject);
        }
        try {
          auto result = CallArmFunctionChecked(
              cpu, kTrapBase, kBase, mod_size, registration.callback,
              registration.context, 0, 0, 0, /*trace=*/false,
              /*hle_trace=*/false, &display, &backend, &abd_text_state,
              /*resume=*/false,
              [&mod_runtime]() { return mod_runtime.ConsumeYieldRequest(); });
          callback_continuation_active = result.yielded;
          if (std::getenv("ZEEB_LOG_HID")) {
            std::fprintf(stderr, "[hid] callback result yielded=%d pc=0x%08x r0=0x%08x\n",
                         result.yielded ? 1 : 0, cpu.GetRegister(zeebulator::kPC), result.r0);
          }
          if (result.wandered_outside_module || result.exceeded_step_budget) {
            std::printf("HID signal callback did not complete trustworthily -- stopping.\n");
            running = false;
            break;
          }
        } catch (const std::exception& e) {
          std::printf("HID signal callback threw: %s (pc=0x%08x)\n", e.what(),
                      cpu.GetRegister(zeebulator::kPC));
          running = false;
          break;
        }
        if (callback_continuation_active) break;
      }
    }

    // Debugger pause gate (Fase 2). A breakpoint/watchpoint hit stops the
    // guest from advancing (we skip mod_runtime.Tick below) while keeping
    // the control channel live, so `read`/`reg`/`state`/`step`/`cont` all
    // work at the trap point. Observation-only: nothing here alters what
    // the interpreter did -- it already executed the trapping instruction.
    if (zeebulator::DebugHooks::Instance().Hit() && !dbg_paused) {
      dbg_paused = true;
      std::printf("  [dbg] PAUSED: %s\n",
                  zeebulator::DebugHooks::Instance().HitInfo().c_str());
      std::fflush(stdout);
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
    // Um callback guest usa o MESMO contexto ARM. Nao empilhe notificacoes
    // cruas sobre uma continuacao que cedeu: CallArmFunction sobrescreveria
    // PC/LR/registradores e o resume retomaria a notificacao, nao o callback
    // original. Adie todos os ticks ate a continuacao terminar.
    if (!callback_continuation_active) {
      media_hle.Tick();
      mem_astream_hle.Tick();
      unzip_stream_hle.Tick();
    }
    // Real BREW timers are one-shot -- real game code re-arms its own via
    // ISHELL_SetTimer from inside the callback (see core/brew/ishell.h).
    // Driving these is what actually runs the game's per-frame logic;
    // nothing calls into the module otherwise from here on.
    if (!dbg_paused) mod_runtime.Tick(kTickMs);
    // ZEEB_TIMER_PREEMPT=1 — BREW event-loop fidelity fix (grounded in the
    // public BREW SDK contract: AEEShell/AEECallback — a scheduled ISHELL_
    // SetTimer callback is serviced by the shell's own loop, it does NOT wait
    // for the app's current callback to return; the app's per-frame loop lives
    // in the shell's timer queue, re-armed each frame). Some titles (Data East
    // cluster — cninja et al.) schedule a 16ms frame timer at EVT_APP_START
    // then enter a GetUpTimeMS busy-wait that never returns, so with our old
    // "only tick timers when no continuation is active" rule the frame timer
    // was starved forever (ROADMAP UPDATE 12/13; recon: brew-sim-recon/notes).
    // Here, when a continuation is live but timers are due, we save the guest
    // context, run the due timer callback(s) (the real frame loop) exactly as
    // the shell would, then restore the continuation's context and let it
    // resume where it yielded. Opt-in; default behavior unchanged.
    if (!dbg_paused && callback_continuation_active &&
        std::getenv("ZEEB_TIMER_PREEMPT") != nullptr) {
      auto due = shell_hle.Tick(kTickMs);
      if (std::getenv("ZEEB_PREEMPT_DIAG")) {
        std::fprintf(stderr, "[preemptdiag] kTickMs=%u pending=%zu due=%zu\n",
                     (unsigned)kTickMs, shell_hle.PendingTimerCount(), due.size());
      }
      if (!due.empty()) {
        // Save the yielded continuation's full architectural context.
        std::array<uint32_t, 16> saved_regs{};
        for (int i = 0; i < 16; ++i) saved_regs[i] = cpu.GetRegister(i);
        uint32_t saved_cpsr = cpu.GetCpsr();
        for (const auto& timer : due) {
          uint32_t call_r0 = timer.r0_override.value_or(timer.user_data);
          uint32_t call_r1 = timer.r0_override.has_value() ? timer.user_data : 0;
          mod_runtime.ConsumeYieldRequest();
          try {
            auto tr = CallArmFunctionChecked(
                cpu, kTrapBase, kBase, mod_size, timer.callback, call_r0,
                call_r1, 0, 0, /*trace=*/false, /*hle_trace=*/false, &display,
                &backend, &abd_text_state, /*resume=*/false,
                [&mod_runtime]() { return mod_runtime.ConsumeYieldRequest(); });
            if (tr.wandered_outside_module || tr.exceeded_step_budget) {
              std::printf("preempting frame timer did not complete trustworthily -- stopping.\n");
              running = false;
              break;
            }
          } catch (const std::exception& e) {
            std::printf("preempting frame timer threw: %s (pc=0x%08x)\n", e.what(),
                        cpu.GetRegister(zeebulator::kPC));
            running = false;
            break;
          }
          ++tick_count;
          if (std::getenv("ZEEB_TICK_DIAG") && (tick_count % 60 == 0)) {
            std::fprintf(stderr, "[tickdiag/preempt] tick=%llu cb=0x%08x\n",
                         static_cast<unsigned long long>(tick_count), timer.callback);
          }
        }
        // Restore the continuation's context so its resume picks up exactly
        // where it yielded (the busy-wait sees the clock advanced by the frame).
        for (int i = 0; i < 16; ++i) cpu.SetRegister(i, saved_regs[i]);
        cpu.SetCpsr(saved_cpsr);
      }
    }
    if (!dbg_paused && callback_continuation_active) {
      try {
        auto resumed = CallArmFunctionChecked(
            cpu, kTrapBase, kBase, mod_size, /*entry=*/0, 0, 0, 0, 0,
            /*trace=*/false, /*hle_trace=*/false, &display, &backend,
            &abd_text_state, /*resume=*/true,
            [&mod_runtime]() { return mod_runtime.ConsumeYieldRequest(); });
        callback_continuation_active = resumed.yielded;
        if (resumed.wandered_outside_module || resumed.exceeded_step_budget) {
          std::printf("resumed callback did not complete trustworthily -- stopping.\n");
          running = false;
        }
      } catch (const std::exception& e) {
        std::printf("resumed callback threw: %s (pc=0x%08x)\n", e.what(),
                    cpu.GetRegister(zeebulator::kPC));
        callback_continuation_active = false;
        running = false;
      }
    }
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
    if (std::getenv("ZEEB_NO_DOWNLOAD_INJECT") == nullptr &&
        !callback_continuation_active && !injected_simulated_download_complete &&
        *captured_download_callback != 0 &&
        tick_count >= 30) {
      injected_simulated_download_complete = true;
      constexpr uint32_t kSimulatedEventStructAddr = 0x80066000;
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 8, 4);
      // `+16` selects the callback's branch. Disasm of AirRacez 0x158a9c shows
      // `+16 == 1` SETS [obj+36]=1 (the real success path, also tail-calls the
      // registered handler), while `+16 == 2` ZEROES it. The old hard-coded 2
      // was right for ddragonz 0x11d020 but actively breaks AirRacez. Let the
      // caller pick the real success value.
      const char* iv = std::getenv("ZEEB_DOWNLOAD_INJECT_VALUE");
      const uint32_t inject_val = (iv && iv[0] == '1') ? 1u : 2u;
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 16, inject_val);
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
    // Z-Wheel: Avanco automatico da tela de instrucoes do Z-Pad (AnimationVideo_Form -> MainMenu).
    // Conforme documentado no Zeebx (`skip_wheel_instructions`, machine.rs:4157), a tela de instrucoes
    // aguarda entrada de confirmacao (`AVK_0` = 0xe030 ou `AVK_SELECT` = 0xe064) entregue como
    // `EVT_KEY` (0x100) ao tratador registrado em 0x0017f48c para disparar o fechamento e transitar
    // para o carrossel do menu principal (0x17eaa8 -> 0x16e8c4 -> 0x13f260).
    static bool zwheel_instructions_dismissed = false;
    if (!zwheel_instructions_dismissed && !callback_continuation_active) {
      for (const auto& [obj, handler] : *widget_handlers) {
        if (handler.function == 0x0017f48c) {
          zwheel_instructions_dismissed = true;
          std::printf("  [zwheel] Avancando da tela de instrucoes do Z-Pad (fn=0x%08x ctx=0x%08x)\n",
                      handler.function, handler.context);
          try {
            CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, handler.function,
                                   handler.context, /*EVT_KEY=*/0x100, /*AVK_0=*/0xe030, 0,
                                   /*trace=*/false, /*hle_trace=*/false, &display, &backend);
          } catch (const std::exception& e) {
            std::printf("  [zwheel] Avanco de instrucoes threw: %s\n", e.what());
          }
          break;
        }
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
    // Run any pending cooperative threads before or between timer ticks.
    // Titulos cujo laco principal e orientado a threads (ex. AirRacez, Bajaz,
    // Boiaz, JetBoardz, Rolimaz, baddudes, hbarrel) executam fatias de thread
    // em vez de timers IShell -- cada fatia e um avanco real do guest.
    tick_count += run_pending_threads_fn("tick");
    // Veredito honesto de renderizacao: "chegou ao event loop" nao
    // distingue jogo rodando de jogo parado. Pura observacao, atras de env.
    if (std::getenv("ZEEB_DRAW_STATS")) {
      static uint64_t last_total = 0;
      static int draw_report = 0;
      auto& ds = zeebulator::DrawStats::Instance();
      if (ds.TotalDraws() != last_total || (draw_report % 50) == 0) {
        char tag[64];
        std::snprintf(tag, sizeof(tag), "tick=%d", static_cast<int>(tick_count));
        ds.Print(tag);
        last_total = ds.TotalDraws();
      }
      ++draw_report;
    }

    for (const auto& timer :
         (dbg_paused || callback_continuation_active)
             ? decltype(shell_hle.Tick(kTickMs)){}
             : shell_hle.Tick(kTickMs)) {
      if (std::getenv("ZEEB_LOG_TIMER") != nullptr) std::fprintf(stderr, "[timer] Firing callback=0x%08x user_data=0x%08x tick=%llu\n", timer.callback, timer.user_data, (unsigned long long)tick_count);
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
        // Discard any sleep edge produced by an earlier, fully synchronous
        // lifecycle call; this continuation belongs only to this callback.
        mod_runtime.ConsumeYieldRequest();
        auto tick_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, timer.callback,
                                                   call_r0, call_r1, 0, 0,
                                                   /*trace=*/false,
                                                   /*hle_trace=*/trace_this_tick, &display, &backend,
                                                   &abd_text_state, /*resume=*/false,
                                                   [&mod_runtime]() {
                                                     return mod_runtime.ConsumeYieldRequest();
                                                   });
        callback_continuation_active = tick_result.yielded;
        if (tick_result.wandered_outside_module || tick_result.exceeded_step_budget) {
          std::printf("timer callback did not complete trustworthily (wandered=%d exceeded=%d) -- ignoring and continuing\n",
                      tick_result.wandered_outside_module, tick_result.exceeded_step_budget);
          // Do NOT abort the entire running session just because a single frame timer exceeded budget or wandered!
          // Real games can recover or continue advancing on subsequent frames.
        }
      } catch (const std::exception& e) {
        std::printf("timer callback threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n",
                    e.what(), cpu.GetRegister(zeebulator::kPC),
                    cpu.GetRegister(zeebulator::kPC) - kBase);
        running = false;
        break;
      }
      ++tick_count;
      if (std::getenv("ZEEB_TICK_DIAG") && (tick_count % 60 == 0)) {
        std::fprintf(stderr, "[tickdiag] tick=%llu cb=0x%08x pc=0x%08x\n",
                     static_cast<unsigned long long>(tick_count),
                     *registered_button_signal,
                     cpu.GetRegister(zeebulator::kPC));
      }
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
        // Espelha o buffer do IDIB, quando o jogo escreveu nele.
        //
        // So sobrescreve se houver pixel nao-zero: titulos que usam o
        // caminho HLE (zenonia: 499 BitBlt + 499 Update) continuam
        // intocados, e um jogo que ainda nao desenhou nada nao apaga o
        // que ja estava na tela.
        // Custa 307k Read16 por quadro, e a medicao mostrou que nenhum
        // titulo do corpus escreve em pBmp (write-watch em ddragonz:
        // zero escritas, IDIB nunca sequer consultada). Fica atras de
        // env para nao cobrar esse preco de todo mundo por um caminho
        // que hoje ninguem usa -- o buffer em si continua existindo,
        // porque pBmp nulo e defeito de verdade para quem usar.
        if (std::getenv("ZEEB_DIB_MIRROR") != nullptr) {
          auto& gm = cpu.GetMemory();
          auto& fb = display.MutableFramebuffer();
          const size_t px = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight);
          bool any = false;
          for (size_t i = 0; i < px && i < fb.size(); ++i) {
            uint16_t v = gm.Read16(0x88000000u + static_cast<uint32_t>(i * 2));
            if (v != 0) { any = true; fb[i] = v; }
          }
          if (any) display.PresentLiveFramebuffer();
        }
        display.RepresentLastFrame();
        // Present the live framebuffer for ABD (font atlas set) OR for any
        // non-ABD title when the generic fill-quad/texture bridge is enabled
        // (ZEEB_GENERIC_RENDER) -- otherwise peggle et al. draw into the live
        // framebuffer via the generic slot-107 path but never present it.
        if (abd_font_atlas.has_value() || std::getenv("ZEEB_GENERIC_RENDER") != nullptr) {
          display.PresentLiveFramebuffer();
        }
      }
    }
    // OwnerDraw nao depende de existir timer vencido. Antes este passe ficava
    // DENTRO do for de timers; uma tela sem timer jamais desenhava. Falhas nao
    // podem sumir em catch(...): registrem objeto, funcao e motivo.
    static uint32_t last_widget_draw_ms = 0;
    const uint32_t widget_draw_now = SDL_GetTicks();
    if (!dbg_paused && !callback_continuation_active &&
        widget_draw_now - last_widget_draw_ms >= 16) {
      last_widget_draw_ms = widget_draw_now;
      for (const auto& [obj, handler] : *widget_draw_callbacks) {
        if (handler.function == 0) continue;
        try {
          uint32_t draw_x = 0, draw_y = 0;
          for (const auto& [geom_key, geom_block] : *widget_geometry) {
            if (static_cast<uint32_t>(geom_key & 0xffffffffu) == obj) {
              draw_x = geom_block[0];
              draw_y = geom_block[1];
              break;
            }
          }
          const auto draw_result = CallArmFunctionChecked(
              cpu, kTrapBase, kBase, mod_size, handler.function,
              handler.context, display_obj, draw_x, draw_y,
              /*trace=*/false, /*hle_trace=*/false, &display, &backend);
          if (draw_result.wandered_outside_module || draw_result.exceeded_step_budget) {
            std::fprintf(stderr,
                         "[widget] draw nao confiavel obj=0x%08x fn=0x%08x wandered=%d exceeded=%d\n",
                         obj, handler.function, draw_result.wandered_outside_module,
                         draw_result.exceeded_step_budget);
          }
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[widget] draw abortou obj=0x%08x fn=0x%08x: %s\n",
                       obj, handler.function, e.what());
        } catch (...) {
          std::fprintf(stderr, "[widget] draw abortou obj=0x%08x fn=0x%08x: excecao desconhecida\n",
                       obj, handler.function);
        }
      }
      // Conteudo dos ImageWidget. O jogo entrega o IImage ao widget por SetIPtr
      // e nunca chama IImage::Draw -- num BREW real o desenho e da biblioteca
      // de widgets do aparelho. Sem este passe, as telas 2D de abertura (logo e
      // bandeira) eram decodificadas e nunca apareciam: medido em 18 s, a
      // framebuffer ficava 307200/307200 px brancos ate o palco 3D entrar.
      for (const auto& [wobj, iobj] : *widget_images) {
        auto vis = widget_visibility->find(wobj);
        if (vis != widget_visibility->end() && !vis->second) continue;
        uint32_t ix = 0, iy = 0;
        for (const auto& [geom_key, geom_block] : *widget_geometry) {
          if (static_cast<uint32_t>(geom_key & 0xffffffffu) == wobj) {
            ix = geom_block[0];
            iy = geom_block[1];
            break;
          }
        }
        draw_image_object(iobj, static_cast<int>(ix), static_cast<int>(iy));
      }
      // Apresenta o framebuffer apos o passe de desenho dos widgets (palco 3D e roller).
      // EXCECAO: quando o backend GL tem atividade real, quem manda no quadro e o GL.
      // Apresentar o framebuffer de software por cima apaga o palco 3D (regressao observada).
      if (std::getenv("ZEEB_LOG_DRAW") != nullptr) {
        // Diagnostico: o que existe no framebuffer NO MOMENTO de apresentar.
        // Sem isso nao da para separar "o jogo nao desenhou" de "desenhou e
        // alguem apagou depois".
        const auto& fb = display.MutableFramebuffer();
        size_t nao_branco = 0;
        for (uint16_t px : fb) {
          if (px != 0xffff) ++nao_branco;
        }
        static uint64_t presents = 0;
        if ((presents++ % 30) == 0) {
          std::fprintf(stderr, "[draw] present: pixels nao brancos=%zu/%zu\n", nao_branco,
                       fb.size());
        }
      }
      // ZEEB_2D_ONLY=1 derruba a regra "quem manda no quadro e o GL": a camada
      // 2D passa a ser apresentada sempre, mesmo com atividade GL real.
      if (std::getenv("ZEEB_2D_ONLY") != nullptr || !backend.HasRealGlActivity()) {
        display.PresentLiveFramebuffer();
      }
    }
    const uint32_t audio_now_ms = SDL_GetTicks();
    const uint32_t audio_elapsed_ms = audio_now_ms - audio_last_mix_ms;
    audio_last_mix_ms = audio_now_ms;
    audio_frame_remainder += static_cast<uint64_t>(kAudioSampleRate) * audio_elapsed_ms;
    const size_t audio_frames = static_cast<size_t>(audio_frame_remainder / 1000);
    audio_frame_remainder %= 1000;
    if (audio_frames != 0) mixer.Mix(backend, audio_frames);
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
    static uint64_t last_gl_draws_seen = 0;
    uint64_t cur_gl_draws = zeebulator::DrawStats::Instance().gl_draw_arrays;
    bool had_gl_draws_this_tick = (cur_gl_draws > last_gl_draws_seen);
    last_gl_draws_seen = cur_gl_draws;

    if (had_gl_draws_this_tick && !backend.HasRealGlActivity() &&
        !backend.IsOffscreenTargetBound()) {
      // Some IGLES11 titles render but never issue an explicit swap. Present their FBO;
      // once an app proves it owns EGL swapping, never compete with its real frames.
      // NAO vale quando o alvo e um pbuffer offscreen: ali o jogo esta desenhando
      // para LER de volta e compor sozinho (Z-Wheel). Apresentar aquilo na janela
      // era substituir a interface inteira pelo palco 3D.
      // Usa o caminho que nao marca "o jogo trocou buffer" -- marcar ali
      // desligava, para sempre, o present do framebuffer 2D.
      backend.PresentGlFrameWithoutSwapMark();
    } else if (!backend.HasRealGlActivity()) {
      display.RepresentLastFrame();
    }
    if (abd_font_atlas.has_value()) {
      display.PresentLiveFramebuffer();
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
      if (tick_count >= kStartTick && *registered_button_signal != 0) {
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
      // `*registered_button_signal != 0` exactly like the real
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
        if (pos != last_pos_acted && *registered_button_signal != 0) {
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
    // Publish a mirror frame every N ticks (captures the fully-composited
    // FBO -- real GL app content + 2D + overlay). Done here, on the thread
    // that owns the GL context.
    if (mirror_server.IsRunning() && (tick_count % static_cast<uint64_t>(mirror_every) == 0)) {
      int mw = 0, mh = 0;
      if (backend.CaptureFrameRgba(mirror_rgba, &mw, &mh) && !mirror_rgba.empty()) {
        mirror_server.PublishFrame(mirror_rgba.data(), mw, mh);
      }
    }
    // Feed the debug UI's CPU tab with a register/state snapshot (cheap; the
    // sink is inert when the debug UI is off).
    if (zeebulator::DebugSink::Instance().Enabled()) {
      zeebulator::DebugState ds;
      for (int i = 0; i < 16; ++i) ds.regs[i] = cpu.GetRegister(i);
      ds.cpsr = cpu.GetCpsr();
      ds.tick = tick_count;
      ds.running = running;
      // Real measured cadence (EMA over recent iterations), not the target.
      {
        static uint32_t last_ms = 0;
        static double ema_fps = 0.0;
        uint32_t now_ms = SDL_GetTicks();
        if (last_ms != 0 && now_ms > last_ms) {
          double inst = 1000.0 / static_cast<double>(now_ms - last_ms);
          ema_fps = (ema_fps == 0.0) ? inst : (ema_fps * 0.9 + inst * 0.1);
        }
        last_ms = now_ms;
        ds.fps = ema_fps;
      }
      zeebulator::DebugSink::Instance().SetState(ds);
    }
    // Fulfill a pending programmatic `step` once the guest has advanced the
    // requested number of ticks (or if the run stopped). Reports the tick
    // reached so the controller can pace itself against real game progress.
    if (ctl_step_active && (tick_count >= ctl_step_target || !running)) {
      char sbuf[96];
      std::snprintf(sbuf, sizeof(sbuf), "{\"ok\":true,\"tick\":%llu,\"running\":%s}",
                    static_cast<unsigned long long>(tick_count), running ? "true" : "false");
      ctl_step_req->reply.set_value(sbuf);
      ctl_step_req.reset();
      ctl_step_active = false;
    }
  }
  control_server.Stop();
  mirror_server.Stop();

  // Watch-hold (ZEEB_WATCH_HOLD=seconds): keep the window on screen for a human
  // to look at even after the guest's own loop ended (returned, aborted, or hit
  // its error dialog). Without this the window vanishes the instant `running`
  // goes false, so a batch "boot each title for N seconds and eyeball it" run
  // only ever shows a brief flash. We keep pumping SDL events (so the WM stays
  // responsive and the last rendered frame -- error screen included -- stays
  // visible) and re-present the last frame until the deadline or a real
  // window close. Purely a viewing aid; changes no guest state.
  if (const char* hold = std::getenv("ZEEB_WATCH_HOLD")) {
    double hold_s = std::atof(hold);
    if (hold_s > 0.0) {
      uint32_t deadline = SDL_GetTicks() + static_cast<uint32_t>(hold_s * 1000.0);
      bool closed = false;
      while (!closed && SDL_GetTicks() < deadline) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
          if (ev.type == SDL_QUIT) { closed = true; break; }
          if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
            closed = true; break;
          }
        }
        backend.PumpForHold();  // keep the last frame visible + WM responsive
        SDL_Delay(16);
      }
    }
  }

  if (const char* shot_exit = std::getenv("ZEEB_SHOT_EXIT")) {
    backend.CaptureScreenshot(shot_exit);
    std::printf("[screenshot] Captured exit to %s\n", shot_exit);
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
