#include "core/brew/idisplay.h"

#include <algorithm>

#include "core/brew/font5x7.h"
#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

// RGBVAL -> RGB565, assuming the common real-BREW 0x00RRGGBB packing
// (see idisplay.h doc comment for how confident we are in that layout).
uint16_t ToRgb565(uint32_t rgbval) {
  uint32_t r = (rgbval >> 16) & 0xFF;
  uint32_t g = (rgbval >> 8) & 0xFF;
  uint32_t b = rgbval & 0xFF;
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
}  // namespace

IDisplayHle::IDisplayHle(Backend& backend, int width, int height)
    : backend_(backend),
      width_(width),
      height_(height),
      framebuffer_(static_cast<size_t>(width) * height, 0) {}

void IDisplayHle::DrawText(IArmCore& core) {
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

void IDisplayHle::GetDeviceBitmap(IArmCore& core) {
  // int GetDeviceBitmap(IDisplay *pIDisplay, IBitmap **ppBitmap)
  // po is R0 (unused), ppBitmap R1. Real disassembly (PHASE8_LOG.md)
  // immediately dereferences *ppBitmap's vtable, so this must hand back
  // a real (if generic) interface object, not a null/unset pointer.
  uint32_t pp_bitmap = core.GetRegister(kR1);
  core.GetMemory().Write32(pp_bitmap, device_bitmap_ptr_);
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void IDisplayHle::Update(IArmCore&) {
  last_presented_ = framebuffer_;
  has_presented_ = true;
  backend_.PushVideoFrame(framebuffer_.data(), width_, height_,
                           PixelFormat::kRGB565);
}

uint32_t IDisplayHle::Build(Memory& memory, HleRuntime& hle,
                             uint32_t vtable_address, uint32_t object_address) {
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
      Stub,                                    // 0  AddRef
      Stub,                                    // 1  Release
      Stub,                                    // 2  GetFontMetrics
      Stub,                                    // 3  MeasureTextEx
      [this](IArmCore& c) { DrawText(c); },     // 4  DrawText
      [this](IArmCore& c) { DrawRect(c); },     // 5  DrawRect
      Stub,                                    // 6  BitBlt
      [this](IArmCore& c) { Update(c); },       // 7  Update
      Stub,                                    // 8  SetAnnunciators
      Stub,                                    // 9  Backlight
      [this](IArmCore& c) { SetColor(c); },     // 10 SetColor
      Stub,                                    // 11 GetSymbol
      Stub,                                    // 12 DrawFrame
      Stub,                                    // 13 CreateDIBitmap
      Stub,                                    // 14 SetDestination
      Stub,                                    // 15 GetDestination
      [this](IArmCore& c) { GetDeviceBitmap(c); },  // 16 GetDeviceBitmap
      Stub,                                    // 17 SetFont
      Stub,                                    // 18 SetClipRect
      Stub,                                    // 19 GetClipRect
      Stub,                                    // 20 Clone
      Stub,                                    // 21 MakeDefault
      Stub,                                    // 22 IsEnabled
      Stub,                                    // 23 NotifyEnable
      Stub,                                    // 24 CreateDIBitmapEx
      Stub,                                    // 25 SetPrefs
  };
  return BuildInterfaceObject(memory, hle, vtable_address, object_address,
                               methods);
}

}  // namespace zeebulator
