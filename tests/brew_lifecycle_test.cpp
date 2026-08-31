// End-to-end integration test: loads our real, compiled (not synthetic)
// test app (tests/fixtures/hello_brew/) and drives it through the actual
// BREW app-lifecycle contract -- AEEMod_Load -> IModule::CreateInstance
// -> HandleEvent(EVT_APP_START) -- exactly the way the real OS would,
// using nothing but the CPU interpreter and the HLE layer. This is the
// real proof the whole pipeline works end to end, not just its pieces in
// isolation. See TASKS.md Phase 3 / PRD.md M0.

#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"
#include "fixtures/hello_brew/entry_offset.h"

using zeebulator::ArmInterpreter;
using zeebulator::Backend;
using zeebulator::HleRuntime;
using zeebulator::IDisplayHle;
using zeebulator::PixelFormat;
using zeebulator::ZPadState;

namespace {

class CapturingBackend : public Backend {
 public:
  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override {
    push_count++;
    last_width = width;
    last_height = height;
    last_format = format;
    last_frame.assign(static_cast<const uint16_t*>(framebuffer),
                       static_cast<const uint16_t*>(framebuffer) +
                           static_cast<size_t>(width) * height);
  }
  void PushAudioSamples(const int16_t*, size_t, int) override {}
  ZPadState PollInput() override { return {}; }

  int push_count = 0;
  int last_width = 0;
  int last_height = 0;
  PixelFormat last_format = PixelFormat::kRGB565;
  std::vector<uint16_t> last_frame;
};

std::vector<uint8_t> ReadFixture(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

}  // namespace

TEST(BrewLifecycle, HelloBrewAppDrawsTextAndUpdatesScreen) {
  auto mod_data = ReadFixture(std::string(FIXTURES_DIR) + "/hello_brew/hello_brew.bin");
  ASSERT_FALSE(mod_data.empty()) << "hello_brew.bin fixture not found";

  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  CapturingBackend backend;
  IDisplayHle display(backend, 64, 48);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);

  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle);
  uint32_t shell = shell_hle.Build(/*vtable=*/0x80000000, /*object=*/0x80001000);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle,
                                        /*vtable=*/0x80002000, /*object=*/0x80003000);

  uint32_t entry = kBase + kHelloBrewAeeModLoadOffset;

  // int AEEMod_Load(IShell *pIShell, void *ph, IModule **ppMod)
  constexpr uint32_t kPpModAddr = 0x00090000;
  hle.CallArmFunction(entry, /*pIShell=*/shell, /*ph=*/0, /*ppMod=*/kPpModAddr);
  uint32_t module_ptr = cpu.GetMemory().Read32(kPpModAddr);
  ASSERT_NE(module_ptr, 0u) << "AEEMod_Load didn't write *ppMod";

  // IModule vtable: AddRef=0, Release=1, CreateInstance=2, FreeResources=3
  // (verified against real AEEModGen.c -- see TASKS.md Phase 3).
  uint32_t module_vtable = cpu.GetMemory().Read32(module_ptr);
  uint32_t create_instance_fn = cpu.GetMemory().Read32(module_vtable + 2 * 4);
  ASSERT_NE(create_instance_fn, 0u);

  // int CreateInstance(IModule *pMod, IShell *pIShell, AEECLSID ClsId, void **ppObj)
  constexpr uint32_t kPpObjAddr = 0x00090010;
  constexpr uint32_t kClsId = 0x1234;
  hle.CallArmFunction(create_instance_fn, module_ptr, shell, kClsId, kPpObjAddr);
  uint32_t handle_event_fn = cpu.GetMemory().Read32(kPpObjAddr);
  ASSERT_NE(handle_event_fn, 0u) << "CreateInstance didn't write *ppObj";

  // AEEAppStart { int error; uint32 clsApp; IDisplay *pDisplay; AEERect rc; }
  constexpr uint32_t kAppStartAddr = 0x00090020;
  auto& mem = cpu.GetMemory();
  mem.Write32(kAppStartAddr + 0, 0);            // error
  mem.Write32(kAppStartAddr + 4, kClsId);       // clsApp
  mem.Write32(kAppStartAddr + 8, display_obj);  // pDisplay
  mem.Write32(kAppStartAddr + 12, 0);           // rc.x
  mem.Write32(kAppStartAddr + 16, 0);           // rc.y
  mem.Write32(kAppStartAddr + 20, 0);           // rc.dx
  mem.Write32(kAppStartAddr + 24, 0);           // rc.dy

  // boolean HandleEvent(void *pMe, int eventCode, uint16 wParam, uint32 dwParam)
  constexpr uint32_t kTestEvtAppStart = 1;  // matches hello_brew.c's own constant
  uint32_t handled = hle.CallArmFunction(handle_event_fn, /*pMe=*/0,
                                          kTestEvtAppStart, /*wParam=*/0,
                                          kAppStartAddr);
  EXPECT_EQ(handled, 1u);

  ASSERT_EQ(backend.push_count, 1) << "HandleEvent should have called IDISPLAY_Update once";
  EXPECT_EQ(backend.last_width, 64);
  EXPECT_EQ(backend.last_height, 48);
  // hello_brew.c draws "Hello" (5 chars, lowercase folded to uppercase
  // by DrawText -- font5x7 only has uppercase) at (10, 10) on 6x8 cells:
  // H=[10,14] E=[16,20] L=[22,26] L=[28,32] O=[34,38].
  EXPECT_EQ(backend.last_frame[10 * 64 + 10], 0xFFFFu) << "H's top-left corner (top row #...#)";
  EXPECT_EQ(backend.last_frame[10 * 64 + 16], 0xFFFFu) << "E's top-left corner (top row #####)";
  EXPECT_EQ(backend.last_frame[10 * 64 + 35], 0xFFFFu) << "O's top row is .###. -- middle is set";
  EXPECT_EQ(backend.last_frame[10 * 64 + 41], 0u) << "outside the drawn text";
}
