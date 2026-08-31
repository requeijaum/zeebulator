// End-to-end integration test: loads our real, compiled (not synthetic)
// GLES-exercising test app (tests/fixtures/hello_gl/) and drives it
// through the real BREW app-lifecycle contract -- AEEMod_Load ->
// IModule::CreateInstance -> HandleEvent -- exactly like
// brew_lifecycle_test.cpp, except this app dispatches through the real
// IGL/IEGL vtable slot order instead of IDisplay. This is what actually
// validates GlHle's dispatch against code we didn't write in C++
// ourselves, not just tests/gl_hle_test.cpp driving it directly via
// HleRuntime::CallArmFunction. See TASKS.md Phase 5.

#include <array>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/gl_hle.h"
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"
#include "fixtures/hello_gl/entry_offset.h"

using zeebulator::ArmInterpreter;
using zeebulator::GlBackend;
using zeebulator::GlHle;
using zeebulator::GlVertexArrays;
using zeebulator::HleRuntime;

namespace {

class RecordingGlBackend : public GlBackend {
 public:
  bool CreateContext() override {
    ++create_context_count;
    return true;
  }
  void DestroyContext() override {}
  void SwapBuffers() override { ++swap_buffers_count; }

  void Clear(zeebulator::GLbitfield mask) override {
    ++clear_count;
    last_clear_mask = mask;
  }
  void ClearColor(float r, float g, float b, float a) override { clear_color = {r, g, b, a}; }
  void Viewport(int x, int y, int width, int height) override { viewport = {x, y, width, height}; }
  void Enable(zeebulator::GLenum) override {}
  void Disable(zeebulator::GLenum) override {}
  void MatrixMode(zeebulator::GLenum mode) override { matrix_modes.push_back(mode); }
  void LoadIdentity() override { ++load_identity_count; }
  void PushMatrix() override {}
  void PopMatrix() override {}
  void Ortho(float left, float right, float bottom, float top, float near_plane,
             float far_plane) override {
    ortho = {left, right, bottom, top, near_plane, far_plane};
  }
  void Frustum(float, float, float, float, float, float) override {}
  void Translate(float, float, float) override {}
  void Rotate(float, float, float, float) override {}
  void Scale(float, float, float) override {}
  void Color4(float, float, float, float) override {}
  void AlphaFunc(zeebulator::GLenum, float) override {}
  void BlendFunc(zeebulator::GLenum, zeebulator::GLenum) override {}
  void DepthFunc(zeebulator::GLenum) override {}
  void ClearDepth(float) override {}
  void DepthMask(bool) override {}
  void DrawArrays(zeebulator::GLenum mode, const GlVertexArrays& arrays) override {
    ++draw_count;
    last_mode = mode;
    last_arrays = arrays;
  }
  void GenTextures(zeebulator::GLsizei, zeebulator::GLuint*) override {}
  void DeleteTextures(zeebulator::GLsizei, const zeebulator::GLuint*) override {}
  void BindTexture(zeebulator::GLenum, zeebulator::GLuint) override {}
  void TexParameter(zeebulator::GLenum, zeebulator::GLenum, zeebulator::GLint) override {}
  void TexImage2D(zeebulator::GLenum, const zeebulator::GlTextureImage&) override {}

  int create_context_count = 0;
  int swap_buffers_count = 0;
  int clear_count = 0;
  int draw_count = 0;
  int load_identity_count = 0;
  zeebulator::GLbitfield last_clear_mask = 0;
  zeebulator::GLenum last_mode = 0;
  std::vector<zeebulator::GLenum> matrix_modes;
  std::array<float, 4> clear_color{};
  std::array<int, 4> viewport{};
  std::array<float, 6> ortho{};
  GlVertexArrays last_arrays;
};

std::vector<uint8_t> ReadFixture(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

}  // namespace

TEST(GlLifecycle, HelloGlAppDrawsRealTriangleThroughRealVtableDispatch) {
  auto mod_data = ReadFixture(std::string(FIXTURES_DIR) + "/hello_gl/hello_gl.bin");
  ASSERT_FALSE(mod_data.empty()) << "hello_gl.bin fixture not found";

  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  RecordingGlBackend backend;
  GlHle gl_hle(backend);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);

  uint32_t gl_obj =
      gl_hle.BuildGl(cpu.GetMemory(), hle, /*vtable=*/0x80004000, /*object=*/0x80005000);
  uint32_t egl_obj =
      gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80006000, /*object=*/0x80007000);

  uint32_t entry = kBase + kHelloGlAeeModLoadOffset;

  // int AEEMod_Load(IShell *pIShell, void *ph, IModule **ppMod)
  constexpr uint32_t kPpModAddr = 0x00090000;
  hle.CallArmFunction(entry, /*pIShell=*/0, /*ph=*/0, /*ppMod=*/kPpModAddr);
  uint32_t module_ptr = cpu.GetMemory().Read32(kPpModAddr);
  ASSERT_NE(module_ptr, 0u) << "AEEMod_Load didn't write *ppMod";

  uint32_t module_vtable = cpu.GetMemory().Read32(module_ptr);
  uint32_t create_instance_fn = cpu.GetMemory().Read32(module_vtable + 2 * 4);
  ASSERT_NE(create_instance_fn, 0u);

  constexpr uint32_t kPpObjAddr = 0x00090010;
  hle.CallArmFunction(create_instance_fn, module_ptr, /*pIShell=*/0, /*ClsId=*/0x1234,
                       kPpObjAddr);
  uint32_t handle_event_fn = cpu.GetMemory().Read32(kPpObjAddr);
  ASSERT_NE(handle_event_fn, 0u) << "CreateInstance didn't write *ppObj";

  // GlDemoParams { IGL *pIGL; IEGL *pIEGL; }
  constexpr uint32_t kParamsAddr = 0x00090020;
  auto& mem = cpu.GetMemory();
  mem.Write32(kParamsAddr + 0, gl_obj);
  mem.Write32(kParamsAddr + 4, egl_obj);

  constexpr uint32_t kTestEvtGlDemo = 2;  // matches hello_gl.c's own constant
  uint32_t handled =
      hle.CallArmFunction(handle_event_fn, /*pMe=*/0, kTestEvtGlDemo, /*wParam=*/0, kParamsAddr);
  EXPECT_EQ(handled, 1u);

  // Every stage of the real compiled app's EGL/GL sequence actually
  // reached the backend, in order, through real vtable dispatch:
  EXPECT_EQ(backend.create_context_count, 1) << "eglMakeCurrent should have created the context";
  EXPECT_EQ(backend.swap_buffers_count, 1) << "eglSwapBuffers should have run";

  ASSERT_EQ(backend.matrix_modes.size(), 2u);
  EXPECT_EQ(backend.matrix_modes[0], 0x1701u) << "GL_PROJECTION";
  EXPECT_EQ(backend.matrix_modes[1], 0x1700u) << "GL_MODELVIEW";
  EXPECT_EQ(backend.load_identity_count, 2);
  EXPECT_FLOAT_EQ(backend.ortho[0], -64.0f);
  EXPECT_FLOAT_EQ(backend.ortho[1], 64.0f);
  EXPECT_FLOAT_EQ(backend.ortho[4], -1.0f);
  EXPECT_FLOAT_EQ(backend.ortho[5], 1.0f);

  EXPECT_EQ(backend.viewport[2], 640);
  EXPECT_EQ(backend.viewport[3], 480);

  ASSERT_EQ(backend.clear_count, 1);
  EXPECT_EQ(backend.last_clear_mask, 0x4000u) << "GL_COLOR_BUFFER_BIT";
  EXPECT_FLOAT_EQ(backend.clear_color[3], 1.0f);

  // The real triangle, gathered from real emulated memory by real
  // compiled ARM code's glVertexPointer/glColorPointer/glDrawArrays
  // calls -- GL_BYTE vertices and GL_UNSIGNED_BYTE colors (normalized).
  ASSERT_EQ(backend.draw_count, 1);
  EXPECT_EQ(backend.last_mode, 0x0004u) << "GL_TRIANGLES";
  ASSERT_TRUE(backend.last_arrays.has_position);
  EXPECT_EQ(backend.last_arrays.vertex_count, 3);
  std::vector<float> expected_positions = {0, 40, -40, -40, 40, -40};
  EXPECT_EQ(backend.last_arrays.positions, expected_positions);

  ASSERT_TRUE(backend.last_arrays.has_color);
  ASSERT_EQ(backend.last_arrays.colors.size(), 12u);
  EXPECT_FLOAT_EQ(backend.last_arrays.colors[0], 1.0f);   // red vertex, R
  EXPECT_FLOAT_EQ(backend.last_arrays.colors[1], 0.0f);   // red vertex, G
  EXPECT_FLOAT_EQ(backend.last_arrays.colors[5], 1.0f);   // green vertex, G
  EXPECT_FLOAT_EQ(backend.last_arrays.colors[10], 1.0f);  // blue vertex, B
}
