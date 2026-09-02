#include "core/brew/bitmap_hle.h"
#include "core/brew/interface_object.h"
#include <algorithm>
#include <cstdio>

namespace zeebulator {

namespace {

void Stub(IArmCore& core) {
  core.SetRegister(kR0, 0);
}

int CalculatePitchBytes(int width, int depth) {
  if (width <= 0 || depth <= 0) return 0;
  const int bits = width * depth;
  return ((bits + 31) / 32) * 4;
}

}  // namespace

BitmapHle::BitmapHle(Memory& memory, HleRuntime& hle, int width, int height,
                     int depth, uint32_t buffer_addr, uint32_t palette_addr,
                     uint32_t transparent_color)
    : memory_(memory),
      hle_(hle),
      width_(std::max(width, 0)),
      height_(std::max(height, 0)),
      depth_(depth),
      pitch_(CalculatePitchBytes(std::max(width, 0), depth)),
      buffer_addr_(buffer_addr),
      palette_addr_(palette_addr),
      transparent_color_(transparent_color) {}

void BitmapHle::AddRef(IArmCore& core) {
  core.SetRegister(kR0, core.GetRegister(kR0));
}

void BitmapHle::Release(IArmCore& core) {
  core.SetRegister(kR0, 0);
}

void BitmapHle::QueryInterface(IArmCore& core) {
  // int QueryInterface(IBitmap *pIBitmap, AEECLSID cls, void **ppOut)
  uint32_t cls = core.GetRegister(kR1);
  uint32_t pp_out = core.GetRegister(kR2);
  uint32_t res_obj = 0;
  if (cls == kClsidBitmap || cls == kClsidDib || cls == kClsidDib20) {
    res_obj = object_addr_;
  }
  if (pp_out != 0) {
    memory_.Write32(pp_out, res_obj);
  }
  core.SetRegister(kR0, res_obj != 0 ? 0 : 4);  // 0 = SUCCESS, 4 = CLASSNOTSUPPORT
}

void BitmapHle::GetInfo(IArmCore& core) {
  // void GetInfo(IBitmap *pIBitmap, AEEBitmapInfo *pInfo, int nSize)
  uint32_t pinfo = core.GetRegister(kR1);
  if (pinfo != 0) {
    // AEEBitmapInfo layout: cx (16), cy (16), nPitch (16), nDepth (8), nColorScheme (8)
    memory_.Write16(pinfo + 0, static_cast<uint16_t>(width_));
    memory_.Write16(pinfo + 2, static_cast<uint16_t>(height_));
    memory_.Write16(pinfo + 4, static_cast<uint16_t>(pitch_));
    memory_.Write8(pinfo + 6, static_cast<uint8_t>(depth_));
    memory_.Write8(pinfo + 7, static_cast<uint8_t>(depth_ == 16 ? 16 : 0));
  }
  core.SetRegister(kR0, 0);
}

void BitmapHle::SetTransparencyColor(IArmCore& core) {
  // void SetTransparencyColor(IBitmap *pIBitmap, uint32 color)
  transparent_color_ = core.GetRegister(kR1);
  if (object_addr_ != 0) {
    memory_.Write32(object_addr_ + 16, transparent_color_);
  }
  core.SetRegister(kR0, 0);
}

void BitmapHle::GetTransparencyColor(IArmCore& core) {
  core.SetRegister(kR0, transparent_color_);
}

uint32_t BitmapHle::Build(uint32_t vtable_address, uint32_t object_address) {
  vtable_addr_ = vtable_address;
  object_addr_ = object_address;

  // IBitmap vtable (standard Qualcomm BREW layout)
  std::vector<HleRuntime::HleFunction> methods = {
      [this](IArmCore& c) { AddRef(c); },                 // 0 AddRef
      [this](IArmCore& c) { Release(c); },                // 1 Release
      [this](IArmCore& c) { QueryInterface(c); },         // 2 QueryInterface
      Stub,                                               // 3 RGBToNative
      Stub,                                               // 4 NativeToRGB
      Stub,                                               // 5 DrawPixel
      Stub,                                               // 6 GetPixel
      Stub,                                               // 7 SetPixels
      Stub,                                               // 8 DrawHScanline
      Stub,                                               // 9 FillRect
      Stub,                                               // 10 BltIn
      Stub,                                               // 11 BltOut
      [this](IArmCore& c) { GetInfo(c); },                // 12 GetInfo
      Stub,                                               // 13 CreateCompatibleBitmap
      [this](IArmCore& c) { SetTransparencyColor(c); },   // 14 SetTransparencyColor
      [this](IArmCore& c) { GetTransparencyColor(c); },   // 15 GetTransparencyColor
  };

  // Populate object fields (Qualcomm DIB layout)
  memory_.Write32(object_address + 0, vtable_address);
  memory_.Write32(object_address + 4, 0);                  // pPaletteMap
  memory_.Write32(object_address + 8, buffer_addr_);       // pBmp
  memory_.Write32(object_address + 12, palette_addr_);     // pRGB
  memory_.Write32(object_address + 16, transparent_color_);// transparentColor
  memory_.Write16(object_address + 20, static_cast<uint16_t>(width_));
  memory_.Write16(object_address + 22, static_cast<uint16_t>(height_));
  memory_.Write16(object_address + 24, static_cast<uint16_t>(pitch_));
  memory_.Write16(object_address + 26, 0);                 // wPaletteEntries
  memory_.Write8(object_address + 28, static_cast<uint8_t>(depth_));
  memory_.Write8(object_address + 29, static_cast<uint8_t>(depth_ == 16 ? 16 : 0)); // nColorScheme

  for (uint32_t i = 30; i < 36; ++i) {
    memory_.Write8(object_address + i, 0);
  }

  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
