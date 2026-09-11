#include "core/brew/idisplay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/brew/font5x7.h"
#include "core/brew/interface_object.h"
#include "core/brew/draw_stats.h"
#include <map>
#include "core/brew/stub_trace.h"

namespace zeebulator {

namespace {
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

// RGBVAL -> RGB565. Qualcomm AEERGBVAL.h defines
// MAKE_RGB(r,g,b) = (r<<8) | (g<<16) | (b<<24).
uint16_t ToRgb565(uint32_t rgbval) {
  uint32_t r = (rgbval >> 8) & 0xFF;
  uint32_t g = (rgbval >> 16) & 0xFF;
  uint32_t b = (rgbval >> 24) & 0xFF;
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
}  // namespace

IDisplayHle::IDisplayHle(Backend& backend, int width, int height)
    : backend_(backend),
      width_(width),
      height_(height),
      framebuffer_(static_cast<size_t>(width) * height, 0),
      clip_x_(0),
      clip_y_(0),
      clip_dx_(static_cast<int16_t>(width)),
      clip_dy_(static_cast<int16_t>(height)) {}

void IDisplayHle::DrawText(IArmCore& core) {
  ++DrawStats::Instance().disp_draw_text;
  // int DrawText(iname* po, AEEFont nFont, const AECHAR* pcText,
  //              int nChars, int x, int y, const AEERect* prcBackground,
  //              uint32 dwFlags)
  // po is R0 (unused here), nFont R1, pcText R2, nChars R3; x/y/rect/flags
  // are the AAPCS stack-passed arguments beyond the first four.
  uint32_t pc_text = core.GetRegister(kR2);
  int32_t n_chars = static_cast<int32_t>(core.GetRegister(kR3));
  int32_t x = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 0));
  int32_t y = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 1));

  // AECHAR is a real, single 8-bit byte per character on this real
  // Zeebo/BREW build -- NOT the 16-bit UTF-16 code unit real BREW's
  // AEEText.h documents as the general-case definition. Found by
  // decoding a real in-memory string at a real DrawText call site
  // (Double Dragon, TASKS.md Phase 8): every other byte of the
  // "16-bit code units" this was originally read as turned out to be
  // an ordinary ASCII letter, never a padding zero -- e.g. the raw
  // bytes spell "Failed in the initialization of the library." when
  // read one byte per character, which is not possible for real
  // ASCII-range UTF-16 (every high byte would have to be 0x00). A
  // negative nChars means "null-terminated" -- scan for a single zero
  // byte, not a zero 16-bit unit.
  int32_t len = n_chars;
  if (len < 0) {
    len = 0;
    while (core.GetMemory().Read8(pc_text + static_cast<uint32_t>(len)) != 0) {
      ++len;
    }
  }

  // 5x7 glyphs on a 6x8 cell (1px spacing right/below each glyph).
  constexpr int kGlyphW = 6;
  uint16_t color = ToRgb565(current_rgbval_);
  auto& mem = core.GetMemory();
  for (int i = 0; i < len; ++i) {
    uint8_t code_unit = mem.Read8(pc_text + static_cast<uint32_t>(i));
    char c = (code_unit < 128) ? static_cast<char>(code_unit) : '\0';
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const uint8_t* glyph = GetGlyph5x7(c);
    if (glyph == nullptr) continue;  // space: nothing to draw
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = glyph[row];
      for (int col = 0; col < 5; ++col) {
        if ((bits & (1u << (4 - col))) == 0) continue;
        int px = x + i * kGlyphW + col;
        int py = y + row;
        if (px >= 0 && px < width_ && py >= 0 && py < height_) {
          framebuffer_[static_cast<size_t>(py) * width_ + px] = color;
        }
      }
    }
  }
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void IDisplayHle::DrawRect(IArmCore& core) {
  ++DrawStats::Instance().disp_draw_rect;
  // void DrawRect(iname *po, const AEERect *pRect, RGBVAL clrFrame,
  //               RGBVAL clrFill, uint32 dwFlags)
  // po is R0 (unused), pRect R1, clrFrame R2 (border, not drawn -- no
  // border-rendering support yet), clrFill R3; dwFlags is the 5th,
  // stack-passed argument (unused).
  uint32_t rect_addr = core.GetRegister(kR1);
  uint32_t clr_fill = core.GetRegister(kR3);

  int x0 = 0, y0 = 0, x1 = width_, y1 = height_;
  if (rect_addr != 0) {
    // Real AEERect: { int16 x, y, dx, dy; } -- confirmed against real
    // AEEAppStart.h/AEERect.h, see PHASE8_LOG.md.
    auto& mem = core.GetMemory();
    x0 = static_cast<int16_t>(mem.Read16(rect_addr + 0));
    y0 = static_cast<int16_t>(mem.Read16(rect_addr + 2));
    x1 = x0 + static_cast<int16_t>(mem.Read16(rect_addr + 4));
    y1 = y0 + static_cast<int16_t>(mem.Read16(rect_addr + 6));
  }

  uint16_t color = ToRgb565(clr_fill);
  if (std::getenv("ZEEB_LOG_DRAW")) {
    if (rect_addr != 0 && std::getenv("ZEEB_RECT_RAW")) {
      auto& mem = core.GetMemory();
      std::fprintf(stderr, "[rectraw] @0x%08x: +0=%d +2=%d +4=%d +6=%d\n", rect_addr,
                   (int)(int16_t)mem.Read16(rect_addr+0),(int)(int16_t)mem.Read16(rect_addr+2),
                   (int)(int16_t)mem.Read16(rect_addr+4),(int)(int16_t)mem.Read16(rect_addr+6));
    }
    std::fprintf(stderr, "[draw] DrawRect rect=(%d,%d)-(%d,%d) fill=0x%06x\n",
                 x0, y0, x1, y1, clr_fill & 0xffffff);
  }
  for (int y = std::max(y0, 0); y < std::min(y1, height_); ++y) {
    for (int x = std::max(x0, 0); x < std::min(x1, width_); ++x) {
      framebuffer_[static_cast<size_t>(y) * width_ + x] = color;
    }
  }
}

void IDisplayHle::SetColor(IArmCore& core) {
  // RGBVAL SetColor(iname *po, AEEClrItem clr, RGBVAL rgb)
  // Real BREW tracks a color per AEEClrItem slot (text, background,
  // frame, ...); this collapses them all into one "current color" DrawText
  // reads -- a documented simplification, not a confirmed real behavior.
  uint32_t previous = current_rgbval_;
  current_rgbval_ = core.GetRegister(kR2);
  core.SetRegister(kR0, previous);
}

void IDisplayHle::SetClipRect(IArmCore& core) {
  // void SetClipRect(IDisplay *pIDisplay, const AEERect *pRect)
  uint32_t prect = core.GetRegister(kR1);
  if (prect != 0) {
    clip_x_ = static_cast<int16_t>(core.GetMemory().Read16(prect + 0));
    clip_y_ = static_cast<int16_t>(core.GetMemory().Read16(prect + 2));
    clip_dx_ = static_cast<int16_t>(core.GetMemory().Read16(prect + 4));
    clip_dy_ = static_cast<int16_t>(core.GetMemory().Read16(prect + 6));
  } else {
    clip_x_ = 0;
    clip_y_ = 0;
    clip_dx_ = static_cast<int16_t>(width_);
    clip_dy_ = static_cast<int16_t>(height_);
  }
  core.SetRegister(kR0, 0);
}

void IDisplayHle::GetClipRect(IArmCore& core) {
  // void GetClipRect(IDisplay *pIDisplay, AEERect *pRect)
  uint32_t prect = core.GetRegister(kR1);
  if (prect != 0) {
    core.GetMemory().Write16(prect + 0, static_cast<uint16_t>(clip_x_));
    core.GetMemory().Write16(prect + 2, static_cast<uint16_t>(clip_y_));
    core.GetMemory().Write16(prect + 4, static_cast<uint16_t>(clip_dx_));
    core.GetMemory().Write16(prect + 6, static_cast<uint16_t>(clip_dy_));
  }
  core.SetRegister(kR0, 0);
}

void IDisplayHle::BitBlt(IArmCore& core) {
  ++DrawStats::Instance().disp_bitblt;
  // int BitBlt(IDisplay *pIDisplay, int xDest, int yDest, int cxDest, int cyDest,
  //            IBitmap *pSrc, int xSrc, int ySrc, AEE_RasterOp rop)
  // R0 = pIDisplay, R1 = xDest, R2 = yDest, R3 = cxDest
  // Stack: [sp+0]=cyDest, [sp+4]=pSrc, [sp+8]=xSrc, [sp+12]=ySrc, [sp+16]=rop
  int x_dest = static_cast<int32_t>(core.GetRegister(kR1));
  int y_dest = static_cast<int32_t>(core.GetRegister(kR2));
  int cx_dest = static_cast<int32_t>(core.GetRegister(kR3));
  int cy_dest = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 0));
  uint32_t src_ptr = HleRuntime::ReadStackArg(core, 1);
  int x_src = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 2));
  int y_src = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 3));
  uint32_t rop = HleRuntime::ReadStackArg(core, 4);

  if (src_ptr == 0 || cx_dest <= 0 || cy_dest <= 0) {
    if (std::getenv("ZEEB_LOG_DRAW")) {
      std::fprintf(stderr, "[draw] BitBlt SKIP dest=(%d,%d) size=%dx%d src=0x%x\n",
                   x_dest, y_dest, cx_dest, cy_dest, src_ptr);
    }
    core.SetRegister(kR0, 0);
    return;
  }
  if (std::getenv("ZEEB_LOG_DRAW")) {
    std::fprintf(stderr, "[draw] BitBlt dest=(%d,%d) size=%dx%d src=0x%x\n",
                 x_dest, y_dest, cx_dest, cy_dest, src_ptr);
  }

  auto& mem = core.GetMemory();
  // Read DIB layout from src_ptr
  uint32_t p_bmp = mem.Read32(src_ptr + 8);
  uint32_t transparent_color = mem.Read32(src_ptr + 16);
  int src_w = static_cast<int>(mem.Read16(src_ptr + 20));
  int src_h = static_cast<int>(mem.Read16(src_ptr + 22));
  int src_pitch = static_cast<int>(mem.Read16(src_ptr + 24));
  int src_depth = static_cast<int>(mem.Read8(src_ptr + 28));

  if (p_bmp == 0) {
    p_bmp = src_ptr;
    src_w = cx_dest + std::max(x_src, 0);
    src_h = cy_dest + std::max(y_src, 0);
    src_pitch = src_w * 2;
    src_depth = 16;
    transparent_color = 0xFFFFFFFFu;
  }

  if (src_w <= 0) src_w = cx_dest;
  if (src_h <= 0) src_h = cy_dest;
  if (src_pitch <= 0) src_pitch = src_w * 2;

  // Some WIPI titles render a 320x240 compatible bitmap and present it in one
  // full-screen 640x480 BitBlt. Preserve ordinary BitBlt's 1:1 crop semantics;
  // only this unambiguous full-surface form scales nearest-neighbor.
  const bool scale_full_surface =
      x_dest == 0 && y_dest == 0 && x_src == 0 && y_src == 0 &&
      cx_dest == width_ && cy_dest == height_ && src_w > 0 && src_h > 0 &&
      (src_w != cx_dest || src_h != cy_dest);
  int copy_w = scale_full_surface ? cx_dest : std::min(cx_dest, src_w - x_src);
  int copy_h = scale_full_surface ? cy_dest : std::min(cy_dest, src_h - y_src);

  int clip_x0 = std::max<int>(clip_x_, 0);
  int clip_y0 = std::max<int>(clip_y_, 0);
  int clip_x1 = std::min<int>(clip_x_ + (clip_dx_ > 0 ? clip_dx_ : width_), width_);
  int clip_y1 = std::min<int>(clip_y_ + (clip_dy_ > 0 ? clip_dy_ : height_), height_);

  int x0 = std::max<int>(x_dest, clip_x0);
  int y0 = std::max<int>(y_dest, clip_y0);
  int x1 = std::min<int>(x_dest + copy_w, clip_x1);
  int y1 = std::min<int>(y_dest + copy_h, clip_y1);

  for (int y = y0; y < y1; ++y) {
    int sy = scale_full_surface
                 ? y_src + ((y - y_dest) * src_h) / cy_dest
                 : y_src + (y - y_dest);
    if (sy < 0 || sy >= src_h) continue;
    uint32_t src_row = p_bmp + static_cast<uint32_t>(sy * src_pitch);
    size_t dst_row_idx = static_cast<size_t>(y) * width_;

    for (int x = x0; x < x1; ++x) {
      int sx = scale_full_surface
                   ? x_src + ((x - x_dest) * src_w) / cx_dest
                   : x_src + (x - x_dest);
      if (sx < 0 || sx >= src_w) continue;

      uint16_t pixel = 0;
      if (src_depth == 16) {
        pixel = mem.Read16(src_row + static_cast<uint32_t>(sx * 2));
      } else {
        pixel = mem.Read16(src_row + static_cast<uint32_t>(sx * 2));
      }

      // 6 = AEE_RO_TRANSPARENT
      if (rop == 6 && static_cast<uint32_t>(pixel) == transparent_color) {
        continue;
      }
      // 1 = AEE_RO_XOR
      if (rop == 1) {
        framebuffer_[dst_row_idx + x] ^= pixel;
      } else {
        framebuffer_[dst_row_idx + x] = pixel;
      }
    }
  }

  core.SetRegister(kR0, 0);
}

void IDisplayHle::GetDeviceBitmap(IArmCore& core) {
  // int GetDeviceBitmap(IDisplay *pIDisplay, IBitmap **ppBitmap)
  // po is R0 (unused), ppBitmap R1. Real disassembly (PHASE8_LOG.md)
  // immediately dereferences *ppBitmap's vtable, so this must hand back
  // a real (if generic) interface object, not a null/unset pointer.
  uint32_t pp_bitmap = core.GetRegister(kR1);
  core.GetMemory().Write32(pp_bitmap, device_bitmap_ptr_);
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void IDisplayHle::SetDestination(IArmCore& core) {
  // int SetDestination(IDisplay *pIDisplay, IBitmap *pbmDst)
  // R1 = requested destination bitmap. NULL means "reset to device bitmap".
  // Real BREW accepts app-provided IBitmap implementations, not only DIBs
  // created by IDisplay, so keep whatever non-null object is handed in as
  // the active destination (matching zeemu BrewDisplay). This lets offscreen
  // composition targets route correctly instead of always drawing to the
  // device framebuffer.
  uint32_t requested = core.GetRegister(kR1);
  destination_ptr_ = (requested == 0) ? device_bitmap_ptr_ : requested;
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void IDisplayHle::GetDestination(IArmCore& core) {
  // IBitmap* GetDestination(IDisplay *pIDisplay)
  // Returns the current destination bitmap; falls back to device bitmap.
  uint32_t dst = destination_ptr_ ? destination_ptr_ : device_bitmap_ptr_;
  core.SetRegister(kR0, dst);
}

void IDisplayHle::IsEnabled(IArmCore& core) {
  // boolean IsEnabled(IDisplay *pIDisplay)
  // The device display is always enabled on Zeebo hardware.
  core.SetRegister(kR0, 1);
}

uint32_t IDisplayHle::AllocateDib(IArmCore& core, int width, int height,
                                   int depth, uint32_t* status) {
  constexpr uint32_t kAeeSuccess = 0;
  constexpr uint32_t kEFailed = 1;
  constexpr uint32_t kENoMemory = 4;

  // No configured arena/runtime cannot succeed.
  if (hle_ == nullptr || dib_arena_end_ == 0) {
    *status = kEFailed;
    return 0;
  }
  // Only 16bpp DIBs are supported by this backend today.
  if (depth != 16 || width <= 0 || height <= 0) {
    *status = kEFailed;
    return 0;
  }

  const int pitch = ((width * depth + 31) / 32) * 4;
  const uint32_t buffer_size = static_cast<uint32_t>(pitch) * static_cast<uint32_t>(height);
  auto align4 = [](uint32_t v) { return (v + 3u) & ~3u; };
  const uint32_t vtable_size = 6u * 4u;
  const uint32_t object_size = 36u;

  uint32_t vtable_addr = align4(dib_arena_next_);
  uint32_t object_addr = align4(vtable_addr + vtable_size);
  uint32_t buffer_addr = align4(object_addr + object_size);
  uint32_t new_next = align4(buffer_addr + buffer_size);

  if (new_next > dib_arena_end_) {
    *status = kENoMemory;
    return 0;
  }

  // Zero the pixel buffer so a freshly-created DIB starts blank.
  for (uint32_t i = 0; i < buffer_size; i += 2) {
    core.GetMemory().Write16(buffer_addr + i, 0);
  }

  auto dib = std::make_unique<BitmapHle>(core.GetMemory(), *hle_, width, height,
                                         depth, buffer_addr);
  uint32_t obj = dib->Build(vtable_addr, object_addr);
  owned_dibs_.push_back(std::move(dib));
  dib_arena_next_ = new_next;
  *status = kAeeSuccess;
  return obj;
}

void IDisplayHle::CreateDIBitmap(IArmCore& core) {
  // int CreateDIBitmap(IDisplay *pIDisplay, IBitmap **ppbmDIB, uint8 depth,
  //                    uint16 width, uint16 height)
  // R0 = pIDisplay, R1 = ppbmDIB (output), R2 = depth (low byte),
  // R3 = width (low 16), stack[0] = height.
  // Matches the zeemu BrewDisplay CreateDIBitmap handler arg order.
  uint32_t pp_out = core.GetRegister(kR1);
  int depth = static_cast<int>(core.GetRegister(kR2) & 0xFF);
  int width = static_cast<int>(core.GetRegister(kR3) & 0xFFFF);
  int height = static_cast<int>(HleRuntime::ReadStackArg(core, 0) & 0xFFFF);

  if (pp_out == 0) {
    core.SetRegister(kR0, 1);  // EFAILED
    return;
  }
  uint32_t status = 0;
  uint32_t obj = AllocateDib(core, width, height, depth, &status);
  core.GetMemory().Write32(pp_out, obj);
  core.SetRegister(kR0, status);
}

void IDisplayHle::CreateDIBitmapEx(IArmCore& core) {
  // int CreateDIBitmapEx(IDisplay *pIDisplay, IBitmap **ppbmDIB, uint8 depth,
  //                      uint16 height, uint16 width, uint16 paletteEntries,
  //                      uint16 extraBytes)
  // R0 = pIDisplay, R1 = ppbmDIB, R2 = depth, R3 = height, stack[0] = width,
  // stack[1] = paletteEntries, stack[2] = extraBytes.
  // Arg order (depth,height then width on stack) matches zeemu BrewDisplay's
  // CreateDIBitmapEx handler. Palette/extraBytes are accepted and validated
  // but not yet materialized (16bpp DIBs need no palette); a future
  // paletted-DIB path can consume them.
  uint32_t pp_out = core.GetRegister(kR1);
  int depth = static_cast<int>(core.GetRegister(kR2) & 0xFF);
  int height = static_cast<int>(core.GetRegister(kR3) & 0xFFFF);
  int width = static_cast<int>(HleRuntime::ReadStackArg(core, 0) & 0xFFFF);
  int palette_entries = static_cast<int>(HleRuntime::ReadStackArg(core, 1) & 0xFFFF);
  int extra_bytes = static_cast<int>(HleRuntime::ReadStackArg(core, 2) & 0xFFFF);

  if (pp_out == 0) {
    core.SetRegister(kR0, 1);  // EFAILED
    return;
  }
  if (palette_entries < 0 || extra_bytes < 0) {
    core.GetMemory().Write32(pp_out, 0);
    core.SetRegister(kR0, 1);  // EFAILED
    return;
  }
  uint32_t status = 0;
  uint32_t obj = AllocateDib(core, width, height, depth, &status);
  core.GetMemory().Write32(pp_out, obj);
  core.SetRegister(kR0, status);
}

void IDisplayHle::Update(IArmCore& core_for_lr) {
  ++DrawStats::Instance().disp_update;
  // Quem chama Update e o dono do laco de render. Com zero draw calls
  // antes dele (ddragonz: 1150 Update, 0 draws), o LR e o unico fio que
  // leva ao ponto onde o jogo decidiu nao desenhar.
  if (std::getenv("ZEEB_LOG_UPDATE_LR") != nullptr) {
    static std::map<uint32_t, uint64_t> callers;
    uint32_t lr = core_for_lr.GetRegister(kLR);
    uint64_t n = ++callers[lr];
    if (n == 1 || n == 100 || (n % 500) == 0) {
      std::printf("[update] caller lr=0x%08x hits=%llu distinct=%zu\n",
                  lr, static_cast<unsigned long long>(n), callers.size());
    }
  }
  if (std::getenv("ZEEB_LOG_DRAW")) {
    std::fprintf(stderr, "[draw] Update (present frame)\n");
  }
  last_presented_ = framebuffer_;
  has_presented_ = true;
  backend_.PushVideoFrame(framebuffer_.data(), width_, height_,
                           PixelFormat::kRGB565);
}

uint32_t IDisplayHle::Build(Memory& memory, HleRuntime& hle,
                             uint32_t vtable_address, uint32_t object_address) {
  hle_ = &hle;
  // Order matches AEEIDisplay.h's INHERIT_IDisplay macro exactly
  // (verified directly against real Qualcomm source -- see TASKS.md
  // Phase 3). Originally only the first 13 slots (through DrawFrame)
  // were built, on the assumption that was the full pre-BREW-MP
  // interface -- that assumption was wrong: real disassembly of Double
  // Dragon (PHASE8_LOG.md) shows it calling slot 18 (SetClipRect)
  // directly, so the real Zeebo IDisplay includes the later slots too.
  // All 26 real slots are present now; only AddRef/Release/DrawText/
  // Update have real behavior so far -- extend individual stubs as
  // games need them.
  std::vector<HleRuntime::HleFunction> methods = {
      LoggedStub("IDisplay", 0, "AddRef"),                                    // 0 AddRef
      LoggedStub("IDisplay", 1, "Release"),                                    // 1 Release
      // 2 GetFontMetrics(IDisplay*, AEEFont, int *pnAscent, int *pnDescent):
      // returns the font height; ascent/descent are optional out-params. Real
      // titles size their own text layout from this, and a silent zero makes
      // every line collapse onto the same baseline.
      [this](IArmCore& c) {
        constexpr int kAscent = 11;
        constexpr int kDescent = 3;
        uint32_t p_ascent = c.GetRegister(kR2);
        uint32_t p_descent = c.GetRegister(kR3);
        if (p_ascent != 0) c.GetMemory().Write32(p_ascent, kAscent);
        if (p_descent != 0) c.GetMemory().Write32(p_descent, kDescent);
        c.SetRegister(kR0, kAscent + kDescent);
      },
      LoggedStub("IDisplay", 3, "MeasureTextEx"),                                    // 3 MeasureTextEx
      [this](IArmCore& c) { DrawText(c); },     // 4  DrawText
      [this](IArmCore& c) { DrawRect(c); },     // 5  DrawRect
      [this](IArmCore& c) { BitBlt(c); },       // 6  BitBlt
      [this](IArmCore& c) { Update(c); },       // 7  Update
      LoggedStub("IDisplay", 8, "SetAnnunciators"),                                    // 8 SetAnnunciators
      LoggedStub("IDisplay", 9, "Backlight"),                                    // 9 Backlight
      [this](IArmCore& c) { SetColor(c); },     // 10 SetColor
      LoggedStub("IDisplay", 11, "GetSymbol"),                                    // 11 GetSymbol
      LoggedStub("IDisplay", 12, "DrawFrame"),                                    // 12 DrawFrame
      [this](IArmCore& c) { CreateDIBitmap(c); },  // 13 CreateDIBitmap
      [this](IArmCore& c) { SetDestination(c); },  // 14 SetDestination
      [this](IArmCore& c) { GetDestination(c); },  // 15 GetDestination
      [this](IArmCore& c) { GetDeviceBitmap(c); },  // 16 GetDeviceBitmap
      LoggedStub("IDisplay", 17, "SetFont"),                                    // 17 SetFont
      [this](IArmCore& c) { SetClipRect(c); },  // 18 SetClipRect
      [this](IArmCore& c) { GetClipRect(c); },  // 19 GetClipRect
      LoggedStub("IDisplay", 20, "Clone"),                                    // 20 Clone
      LoggedStub("IDisplay", 21, "MakeDefault"),                                    // 21 MakeDefault
      [this](IArmCore& c) { IsEnabled(c); },    // 22 IsEnabled
      LoggedStub("IDisplay", 23, "NotifyEnable"),                                    // 23 NotifyEnable
      [this](IArmCore& c) { CreateDIBitmapEx(c); },  // 24 CreateDIBitmapEx
      LoggedStub("IDisplay", 25, "SetPrefs"),                                    // 25 SetPrefs
  };
  return BuildInterfaceObject(memory, hle, vtable_address, object_address,
                               methods);
}

}  // namespace zeebulator
