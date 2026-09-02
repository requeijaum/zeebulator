#pragma once

#include <cstdint>
#include <vector>
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// HLE implementation of BREW IBitmap and IDIB (AEECLSID_DIB = 0x01001045)
// Qualcomm DIB object layout:
// offset +0: vtable pointer
// offset +4: pPaletteMap (u32)
// offset +8: pBmp (u32, pointer to pixel buffer)
// offset +12: pRGB (u32, pointer to palette)
// offset +16: transparentColor (u32)
// offset +20: cx (u16 width)
// offset +22: cy (u16 height)
// offset +24: nPitch (u16 pitch in bytes)
// offset +26: wPaletteEntries (u16)
// offset +28: nDepth (u8 bits per pixel)
// offset +29: nColorScheme (u8)
// offset +30..35: padding
class BitmapHle {
 public:
  static constexpr uint32_t kClsidDib = 0x01001045u;
  static constexpr uint32_t kClsidBitmap = 0x01001021u;
  static constexpr uint32_t kClsidDib20 = 0x0100102cu;

  BitmapHle(Memory& memory, HleRuntime& hle, int width, int height, int depth,
            uint32_t buffer_addr, uint32_t palette_addr = 0,
            uint32_t transparent_color = 0);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

  int width() const { return width_; }
  int height() const { return height_; }
  int depth() const { return depth_; }
  int pitch() const { return pitch_; }
  uint32_t buffer_address() const { return buffer_addr_; }
  uint32_t object_address() const { return object_addr_; }

 private:
  void AddRef(IArmCore& core);
  void Release(IArmCore& core);
  void QueryInterface(IArmCore& core);
  void GetInfo(IArmCore& core);
  void SetTransparencyColor(IArmCore& core);
  void GetTransparencyColor(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  int width_;
  int height_;
  int depth_;
  int pitch_;
  uint32_t buffer_addr_;
  uint32_t palette_addr_;
  uint32_t transparent_color_;
  uint32_t vtable_addr_ = 0;
  uint32_t object_addr_ = 0;
};

}  // namespace zeebulator
