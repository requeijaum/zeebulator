#include "core/brew/bitmap_hle.h"
#include <gtest/gtest.h>
#include "core/cpu/arm_interpreter.h"
#include "core/memory/memory.h"

namespace zeebulator {

namespace {
constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
}  // namespace

TEST(BitmapHle, LayoutAndVtableMethodsWorkCorrectly) {
  Memory memory;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);

  const int width = 320;
  const int height = 240;
  const int depth = 16;
  const uint32_t buffer_addr = 0x100000;
  const uint32_t vtable_addr = 0x2000;
  const uint32_t object_addr = 0x3000;

  BitmapHle bmp(memory, hle, width, height, depth, buffer_addr);
  uint32_t iface = bmp.Build(vtable_addr, object_addr);
  EXPECT_EQ(iface, object_addr);

  // Check Qualcomm DIB header layout in memory
  EXPECT_EQ(memory.Read32(object_addr + 0), vtable_addr);
  EXPECT_EQ(memory.Read32(object_addr + 8), buffer_addr);
  EXPECT_EQ(memory.Read16(object_addr + 20), 320);
  EXPECT_EQ(memory.Read16(object_addr + 22), 240);
  EXPECT_EQ(memory.Read16(object_addr + 24), 640);  // 320 * 2 bytes pitch
  EXPECT_EQ(memory.Read8(object_addr + 28), 16);   // depth

  // Test GetInfo (slot 12)
  uint32_t info_struct_addr = 0x4000;
  cpu.SetRegister(kR1, info_struct_addr);
  cpu.SetRegister(kR2, 12);  // sizeof(AEEBitmapInfo)
  uint32_t res = hle.CallArmFunction(memory.Read32(vtable_addr + 12 * 4), object_addr, info_struct_addr, 12);
  EXPECT_EQ(res, 0u);
  EXPECT_EQ(memory.Read32(info_struct_addr + 0), 320u);
  EXPECT_EQ(memory.Read32(info_struct_addr + 4), 240u);
  EXPECT_EQ(memory.Read32(info_struct_addr + 8), 16u);

  // Test SetTransparencyColor (slot 14) and GetTransparencyColor (slot 15)
  res = hle.CallArmFunction(memory.Read32(vtable_addr + 14 * 4), object_addr, 0xF81F);
  EXPECT_EQ(res, 0u);
  EXPECT_EQ(memory.Read32(object_addr + 16), 0xF81Fu);

  const uint32_t color_out = 0x4800;
  res = hle.CallArmFunction(memory.Read32(vtable_addr + 15 * 4), object_addr, color_out);
  EXPECT_EQ(res, 0u);
  EXPECT_EQ(memory.Read32(color_out), 0xF81Fu);

  // Test QueryInterface (slot 2)
  uint32_t out_ptr_addr = 0x5000;
  res = hle.CallArmFunction(memory.Read32(vtable_addr + 2 * 4), object_addr, BitmapHle::kClsidDib, out_ptr_addr);
  EXPECT_EQ(res, 0u);
  EXPECT_EQ(memory.Read32(out_ptr_addr), object_addr);
}

}  // namespace zeebulator
