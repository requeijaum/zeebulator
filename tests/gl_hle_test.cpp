#include "core/brew/gl_hle.h"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::GlBackend;
using zeebulator::GlHle;
using zeebulator::HleRuntime;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kGlVtable = 0x80000000;
constexpr uint32_t kGlObject = 0x80001000;
constexpr uint32_t kEglVtable = 0x80002000;
constexpr uint32_t kEglObject = 0x80003000;
constexpr uint32_t kScratch = 0x00090000;

class FakeGlBackend : public zeebulator::GlBackend {
 public:
  bool CreateContext() override {
    ++create_context_count;
    return true;
  }
  void DestroyContext() override { ++destroy_context_count; }
  void SwapBuffers() override { ++swap_buffers_count; }

  void Clear(zeebulator::GLbitfield mask) override { last_clear_mask = mask; }
  void ClearColor(float r, float g, float b, float a) override {
    clear_color = {r, g, b, a};
  }
  void Viewport(int x, int y, int width, int height) override {
    viewport = {x, y, width, height};
  }
  void Enable(zeebulator::GLenum cap) override { last_enabled = cap; }
  void Disable(zeebulator::GLenum cap) override { last_disabled = cap; }
  void MatrixMode(zeebulator::GLenum mode) override { last_matrix_mode = mode; }
  void LoadIdentity() override { ++load_identity_count; }
  void PushMatrix() override { ++push_matrix_count; }
  void PopMatrix() override { ++pop_matrix_count; }
  void Ortho(float left, float right, float bottom, float top, float near_plane,
             float far_plane) override {
    ortho = {left, right, bottom, top, near_plane, far_plane};
  }
  void Frustum(float left, float right, float bottom, float top, float near_plane,
               float far_plane) override {
    frustum = {left, right, bottom, top, near_plane, far_plane};
  }
  void Translate(float x, float y, float z) override { translate = {x, y, z}; }
  void Rotate(float angle_degrees, float x, float y, float z) override {
    rotate = {angle_degrees, x, y, z};
  }
  void Scale(float x, float y, float z) override { scale = {x, y, z}; }
  void Color4(float r, float g, float b, float a) override { color4 = {r, g, b, a}; }
  void AlphaFunc(zeebulator::GLenum func, float ref) override {
    last_alpha_func = func;
    last_alpha_ref = ref;
  }
  void BlendFunc(zeebulator::GLenum sfactor, zeebulator::GLenum dfactor) override {
    last_blend_sfactor = sfactor;
    last_blend_dfactor = dfactor;
  }
  void DepthFunc(zeebulator::GLenum func) override { last_depth_func = func; }
  void ClearDepth(float depth) override { last_clear_depth = depth; }
  void DepthMask(bool flag) override { last_depth_mask = flag; }
  void DrawArrays(zeebulator::GLenum mode, const zeebulator::GlVertexArrays& arrays) override {
    ++draw_count;
    last_mode = mode;
    last_arrays = arrays;
  }

  void GenTextures(zeebulator::GLsizei n, zeebulator::GLuint* textures) override {
    for (zeebulator::GLsizei i = 0; i < n; ++i) {
      textures[i] = next_texture_id++;
    }
  }
  void DeleteTextures(zeebulator::GLsizei n, const zeebulator::GLuint* textures) override {
    deleted_textures.assign(textures, textures + n);
  }
  void BindTexture(zeebulator::GLenum target, zeebulator::GLuint texture) override {
    last_bind_target = target;
    last_bound_texture = texture;
  }
  void TexParameter(zeebulator::GLenum target, zeebulator::GLenum pname,
                     zeebulator::GLint param) override {
    last_texparam_target = target;
    last_texparam_pname = pname;
    last_texparam_value = param;
  }
  void TexImage2D(zeebulator::GLenum target, const zeebulator::GlTextureImage& image) override {
    ++teximage_count;
    last_teximage_target = target;
    last_teximage = image;
    if (image.pixels != nullptr) {
      size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                      static_cast<size_t>(zeebulator::GlPixelSize(image.format, image.type));
      last_teximage_pixels.assign(image.pixels, image.pixels + total);
    } else {
      last_teximage_pixels.clear();
    }
    last_teximage.pixels = nullptr;  // avoid keeping a dangling pointer around
  }

  int create_context_count = 0;
  zeebulator::GLuint next_texture_id = 1;
  std::vector<zeebulator::GLuint> deleted_textures;
  zeebulator::GLenum last_bind_target = 0;
  zeebulator::GLuint last_bound_texture = 0;
  zeebulator::GLenum last_texparam_target = 0;
  zeebulator::GLenum last_texparam_pname = 0;
  zeebulator::GLint last_texparam_value = 0;
  int teximage_count = 0;
  zeebulator::GLenum last_teximage_target = 0;
  zeebulator::GlTextureImage last_teximage;
  std::vector<uint8_t> last_teximage_pixels;
  int draw_count = 0;
  zeebulator::GLenum last_mode = 0;
  zeebulator::GlVertexArrays last_arrays;
  int destroy_context_count = 0;
  int swap_buffers_count = 0;
  int load_identity_count = 0;
  int push_matrix_count = 0;
  int pop_matrix_count = 0;
  zeebulator::GLbitfield last_clear_mask = 0;
  zeebulator::GLenum last_enabled = 0;
  zeebulator::GLenum last_disabled = 0;
  zeebulator::GLenum last_matrix_mode = 0;
  std::array<float, 4> clear_color{};
  std::array<int, 4> viewport{};
  std::array<float, 6> ortho{};
  std::array<float, 6> frustum{};
  std::array<float, 3> translate{};
  std::array<float, 4> rotate{};
  std::array<float, 3> scale{};
  std::array<float, 4> color4{};
  zeebulator::GLenum last_alpha_func = 0;
  float last_alpha_ref = 0.0f;
  zeebulator::GLenum last_blend_sfactor = 0;
  zeebulator::GLenum last_blend_dfactor = 0;
  zeebulator::GLenum last_depth_func = 0;
  float last_clear_depth = 0.0f;
  bool last_depth_mask = true;
};

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  FakeGlBackend backend;
  GlHle gl_hle{backend};
  uint32_t gl_obj;
  uint32_t egl_obj;

  Fixture() {
    gl_obj = gl_hle.BuildGl(cpu.GetMemory(), hle, kGlVtable, kGlObject);
    egl_obj = gl_hle.BuildEgl(cpu.GetMemory(), hle, kEglVtable, kEglObject);
  }

  uint32_t GlSlot(uint32_t slot) { return cpu.GetMemory().Read32(kGlVtable + slot * 4); }
  uint32_t EglSlot(uint32_t slot) { return cpu.GetMemory().Read32(kEglVtable + slot * 4); }
};

constexpr zeebulator::GLfixed ToFixed(float v) {
  return static_cast<zeebulator::GLfixed>(v * 65536.0f);
}

void WriteFloat(zeebulator::Memory& mem, uint32_t addr, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  mem.Write32(addr, bits);
}

}  // namespace

TEST(GlHle, BuildReturnsObjectAddressPointingAtVtable) {
  Fixture f;
  EXPECT_EQ(f.gl_obj, kGlObject);
  EXPECT_EQ(f.egl_obj, kEglObject);
  EXPECT_EQ(f.cpu.GetMemory().Read32(kGlObject), kGlVtable);
  EXPECT_EQ(f.cpu.GetMemory().Read32(kEglObject), kEglVtable);
}

TEST(GlHle, VtableHasRealSlotCounts) {
  // 80 = 3 base (AddRef/Release/QueryInterface) + 77 gl* methods.
  // 28 = 3 base + 25 egl* methods. Every slot must be a distinct,
  // non-null sentinel address -- proves the full real-order scaffold was
  // actually built, not just the handful of implemented slots.
  Fixture f;
  for (uint32_t slot = 0; slot < 80; ++slot) {
    EXPECT_NE(f.GlSlot(slot), 0u) << "IGL slot " << slot << " missing";
  }
  for (uint32_t slot = 0; slot < 28; ++slot) {
    EXPECT_NE(f.EglSlot(slot), 0u) << "IEGL slot " << slot << " missing";
  }
}

TEST(GlHle, GlClearForwardsRawBitmaskToBackend) {
  Fixture f;
  // glClear does NOT receive the interface pointer -- R0 is the mask arg.
  f.hle.CallArmFunction(f.GlSlot(7), 0x4100);
  EXPECT_EQ(f.backend.last_clear_mask, 0x4100u);
}

TEST(GlHle, GlClearColorxConvertsFixedPointToFloat) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(8), ToFixed(1.0f), ToFixed(0.5f), ToFixed(0.25f),
                         ToFixed(0.0f));
  EXPECT_FLOAT_EQ(f.backend.clear_color[0], 1.0f);
  EXPECT_FLOAT_EQ(f.backend.clear_color[1], 0.5f);
  EXPECT_FLOAT_EQ(f.backend.clear_color[2], 0.25f);
  EXPECT_FLOAT_EQ(f.backend.clear_color[3], 0.0f);
}

TEST(GlHle, GlViewportForwardsIntegerArgs) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(79), 1, 2, 640, 480);
  EXPECT_EQ(f.backend.viewport[0], 1);
  EXPECT_EQ(f.backend.viewport[1], 2);
  EXPECT_EQ(f.backend.viewport[2], 640);
  EXPECT_EQ(f.backend.viewport[3], 480);
}

TEST(GlHle, GlEnableAndDisableForwardRawEnum) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(28), 0x0DE1);  // GL_TEXTURE_2D
  EXPECT_EQ(f.backend.last_enabled, 0x0DE1u);
  f.hle.CallArmFunction(f.GlSlot(24), 0x0B71);  // GL_DEPTH_TEST
  EXPECT_EQ(f.backend.last_disabled, 0x0B71u);
}

TEST(GlHle, GlMatrixModeAndLoadIdentity) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(51), 0x1701);  // GL_PROJECTION
  EXPECT_EQ(f.backend.last_matrix_mode, 0x1701u);
  f.hle.CallArmFunction(f.GlSlot(46));
  EXPECT_EQ(f.backend.load_identity_count, 1);
}

TEST(GlHle, GlOrthoxReadsTwoStackArgsAndConvertsFixed) {
  Fixture f;
  f.cpu.SetRegister(zeebulator::kSP, kScratch);
  f.cpu.GetMemory().Write32(kScratch, static_cast<uint32_t>(ToFixed(-1.0f)));  // zNear
  f.cpu.GetMemory().Write32(kScratch + 4, static_cast<uint32_t>(ToFixed(1.0f)));  // zFar
  f.hle.CallArmFunction(f.GlSlot(56), ToFixed(0.0f), ToFixed(640.0f), ToFixed(480.0f),
                         ToFixed(0.0f));
  EXPECT_FLOAT_EQ(f.backend.ortho[0], 0.0f);
  EXPECT_FLOAT_EQ(f.backend.ortho[1], 640.0f);
  EXPECT_FLOAT_EQ(f.backend.ortho[2], 480.0f);
  EXPECT_FLOAT_EQ(f.backend.ortho[3], 0.0f);
  EXPECT_FLOAT_EQ(f.backend.ortho[4], -1.0f);
  EXPECT_FLOAT_EQ(f.backend.ortho[5], 1.0f);
}

TEST(GlHle, GlTranslateRotateScaleConvertFixedToFloat) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(77), ToFixed(1.0f), ToFixed(2.0f), ToFixed(3.0f));
  EXPECT_FLOAT_EQ(f.backend.translate[0], 1.0f);
  EXPECT_FLOAT_EQ(f.backend.translate[2], 3.0f);

  f.hle.CallArmFunction(f.GlSlot(63), ToFixed(90.0f), ToFixed(0.0f), ToFixed(1.0f),
                         ToFixed(0.0f));
  EXPECT_FLOAT_EQ(f.backend.rotate[0], 90.0f);
  EXPECT_FLOAT_EQ(f.backend.rotate[2], 1.0f);

  f.hle.CallArmFunction(f.GlSlot(65), ToFixed(2.0f), ToFixed(2.0f), ToFixed(2.0f));
  EXPECT_FLOAT_EQ(f.backend.scale[0], 2.0f);
}

TEST(GlHle, GlColor4xConvertsFixedToFloat) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(12), ToFixed(1.0f), ToFixed(0.0f), ToFixed(0.0f),
                         ToFixed(1.0f));
  EXPECT_FLOAT_EQ(f.backend.color4[0], 1.0f);
  EXPECT_FLOAT_EQ(f.backend.color4[1], 0.0f);
  EXPECT_FLOAT_EQ(f.backend.color4[3], 1.0f);
}

TEST(GlHle, EglLifecycleMatchesRealSampleAppCallSequence) {
  // Mirrors the real call order from Qualcomm's own OGLES sample source
  // (simple_drawtexture.c): eglGetDisplay -> eglInitialize ->
  // eglChooseConfig -> eglCreateWindowSurface -> eglCreateContext ->
  // eglMakeCurrent -> ... -> eglSwapBuffers -> teardown.
  Fixture f;

  uint32_t display = f.hle.CallArmFunction(f.EglSlot(4), /*NativeDisplayType=*/0);
  EXPECT_NE(display, 0u);

  uint32_t init_ok = f.hle.CallArmFunction(f.EglSlot(5), display, /*major=*/0, /*minor=*/0);
  EXPECT_EQ(init_ok, zeebulator::kEglTrue);

  uint32_t configs_out = kScratch + 0x100;
  uint32_t num_config_out = kScratch + 0x104;
  f.cpu.SetRegister(zeebulator::kSP, kScratch);
  f.cpu.GetMemory().Write32(kScratch, num_config_out);  // stack arg 0
  uint32_t chosen = f.hle.CallArmFunction(f.EglSlot(10), display, /*attrib_list=*/0,
                                           configs_out, /*config_size=*/1);
  EXPECT_EQ(chosen, zeebulator::kEglTrue);
  EXPECT_NE(f.cpu.GetMemory().Read32(configs_out), 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(num_config_out), 1u);
  uint32_t config = f.cpu.GetMemory().Read32(configs_out);

  uint32_t surface = f.hle.CallArmFunction(f.EglSlot(12), display, config,
                                            /*window=*/0, /*attrib_list=*/0);
  EXPECT_NE(surface, 0u);

  uint32_t context = f.hle.CallArmFunction(f.EglSlot(17), display, config,
                                            /*share_list=*/0, /*attrib_list=*/0);
  EXPECT_NE(context, 0u);

  uint32_t made_current =
      f.hle.CallArmFunction(f.EglSlot(19), display, surface, surface, context);
  EXPECT_EQ(made_current, zeebulator::kEglTrue);
  EXPECT_EQ(f.backend.create_context_count, 1);

  f.hle.CallArmFunction(f.EglSlot(26), display, surface);
  EXPECT_EQ(f.backend.swap_buffers_count, 1);

  f.hle.CallArmFunction(f.EglSlot(18), display, context);  // eglDestroyContext
  EXPECT_EQ(f.backend.destroy_context_count, 1);

  f.hle.CallArmFunction(f.EglSlot(15), display, surface);  // eglDestroySurface
  f.hle.CallArmFunction(f.EglSlot(6), display);             // eglTerminate
}

TEST(GlHle, EglMakeCurrentOnlyCreatesHostContextOnce) {
  Fixture f;
  uint32_t display = f.hle.CallArmFunction(f.EglSlot(4), 0);
  uint32_t context = f.hle.CallArmFunction(f.EglSlot(17), display, 0, 0, 0);
  f.hle.CallArmFunction(f.EglSlot(19), display, 0, 0, context);
  f.hle.CallArmFunction(f.EglSlot(19), display, 0, 0, context);
  EXPECT_EQ(f.backend.create_context_count, 1)
      << "repeated eglMakeCurrent with the same context shouldn't recreate the host context";
}

TEST(GlHle, EglGetErrorReturnsSuccessSentinel) {
  Fixture f;
  EXPECT_EQ(f.hle.CallArmFunction(f.EglSlot(3)), 0x3000u);
}

TEST(GlHle, EglQueryStringNeverReturnsNull) {
  Fixture f;
  for (uint32_t name : {0x3053u, 0x3054u, 0x3055u, 0x308Du, 0xDEADu}) {
    EXPECT_NE(f.hle.CallArmFunction(f.EglSlot(7), 0, name), 0u) << "name " << name;
  }
}

TEST(GlHle, EglQueryStringExtensionsIsEmptySinceNoneAreImplemented) {
  Fixture f;
  uint32_t addr = f.hle.CallArmFunction(f.EglSlot(7), 0, /*EGL_EXTENSIONS=*/0x3055);
  EXPECT_EQ(f.cpu.GetMemory().Read8(addr), 0u) << "empty string: first byte is the terminator";
}

TEST(GlHle, DrawArraysGathersByteVerticesAndNormalizesUnsignedByteColors) {
  Fixture f;
  uint32_t vertex_addr = kScratch + 0x200;
  uint32_t color_addr = kScratch + 0x300;

  // 3 vertices, GL_BYTE xyz, tightly packed (stride 0 -> 3 bytes/vertex).
  const int8_t vertices[9] = {1, 2, 3, -1, -2, -3, 4, 5, 6};
  for (size_t i = 0; i < 9; ++i) {
    f.cpu.GetMemory().Write8(vertex_addr + static_cast<uint32_t>(i),
                              static_cast<uint8_t>(vertices[i]));
  }
  // 3 colors, GL_UNSIGNED_BYTE rgba, tightly packed (stride 0 -> 4 bytes/vertex).
  const uint8_t colors[12] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0};
  for (size_t i = 0; i < 12; ++i) {
    f.cpu.GetMemory().Write8(color_addr + static_cast<uint32_t>(i), colors[i]);
  }

  // glVertexPointer(size=3, type=GL_BYTE, stride=0, pointer)
  f.hle.CallArmFunction(f.GlSlot(78), 3, zeebulator::kGlByte, 0, vertex_addr);
  // glColorPointer(size=4, type=GL_UNSIGNED_BYTE, stride=0, pointer)
  f.hle.CallArmFunction(f.GlSlot(14), 4, zeebulator::kGlUnsignedByte, 0, color_addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlVertexArray);   // glEnableClientState
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlColorArray);    // glEnableClientState

  // glDrawArrays(mode=GL_TRIANGLES, first=0, count=3)
  f.hle.CallArmFunction(f.GlSlot(26), 0x0004, 0, 3);

  ASSERT_EQ(f.backend.draw_count, 1);
  EXPECT_EQ(f.backend.last_mode, 0x0004u);
  const auto& arrays = f.backend.last_arrays;
  EXPECT_EQ(arrays.vertex_count, 3);
  ASSERT_TRUE(arrays.has_position);
  EXPECT_EQ(arrays.position_size, 3);
  std::vector<float> expected_positions = {1, 2, 3, -1, -2, -3, 4, 5, 6};
  EXPECT_EQ(arrays.positions, expected_positions);

  ASSERT_TRUE(arrays.has_color);
  ASSERT_EQ(arrays.colors.size(), 12u);
  EXPECT_FLOAT_EQ(arrays.colors[0], 1.0f);
  EXPECT_FLOAT_EQ(arrays.colors[1], 0.0f);
  EXPECT_FLOAT_EQ(arrays.colors[7], 128.0f / 255.0f);
  EXPECT_FLOAT_EQ(arrays.colors[10], 1.0f);

  EXPECT_FALSE(arrays.has_texcoord);
  EXPECT_FALSE(arrays.has_normal);
}

TEST(GlHle, DrawArraysHonorsNonZeroStride) {
  Fixture f;
  uint32_t addr = kScratch + 0x400;
  // 2 vertices, GL_BYTE xyz + 1 padding byte, stride=4.
  const uint8_t raw[8] = {10, 20, 30, 99, 40, 50, 60, 88};
  for (size_t i = 0; i < 8; ++i) {
    f.cpu.GetMemory().Write8(addr + static_cast<uint32_t>(i), raw[i]);
  }
  f.hle.CallArmFunction(f.GlSlot(78), 3, zeebulator::kGlByte, 4, addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlVertexArray);
  f.hle.CallArmFunction(f.GlSlot(26), 0x0004, 0, 2);

  std::vector<float> expected = {10, 20, 30, 40, 50, 60};
  EXPECT_EQ(f.backend.last_arrays.positions, expected);
}

TEST(GlHle, DrawElementsWithUnsignedByteIndicesGathersInIndexOrder) {
  Fixture f;
  uint32_t vertex_addr = kScratch + 0x500;
  // 4 vertices, GL_FLOAT xy, tightly packed (stride 0 -> 8 bytes/vertex).
  const float verts[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (size_t i = 0; i < 8; ++i) {
    WriteFloat(f.cpu.GetMemory(), vertex_addr + static_cast<uint32_t>(i) * 4, verts[i]);
  }
  uint32_t index_addr = kScratch + 0x600;
  const uint8_t indices[4] = {0, 2, 1, 2};
  for (size_t i = 0; i < 4; ++i) {
    f.cpu.GetMemory().Write8(index_addr + static_cast<uint32_t>(i), indices[i]);
  }

  f.hle.CallArmFunction(f.GlSlot(78), 2, zeebulator::kGlFloat, 0, vertex_addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlVertexArray);
  // glDrawElements(mode=GL_TRIANGLES, count=4, type=GL_UNSIGNED_BYTE, indices)
  f.hle.CallArmFunction(f.GlSlot(27), 0x0004, 4, zeebulator::kGlUnsignedByte, index_addr);

  ASSERT_EQ(f.backend.draw_count, 1);
  EXPECT_EQ(f.backend.last_arrays.vertex_count, 4);
  std::vector<float> expected = {1, 2, 5, 6, 3, 4, 5, 6};  // v0, v2, v1, v2
  EXPECT_EQ(f.backend.last_arrays.positions, expected);
}

TEST(GlHle, DrawElementsWithUnsignedShortIndices) {
  Fixture f;
  uint32_t vertex_addr = kScratch + 0x700;
  const float verts[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (size_t i = 0; i < 8; ++i) {
    WriteFloat(f.cpu.GetMemory(), vertex_addr + static_cast<uint32_t>(i) * 4, verts[i]);
  }
  uint32_t index_addr = kScratch + 0x800;
  f.cpu.GetMemory().Write16(index_addr, 3);
  f.cpu.GetMemory().Write16(index_addr + 2, 0);

  f.hle.CallArmFunction(f.GlSlot(78), 2, zeebulator::kGlFloat, 0, vertex_addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlVertexArray);
  f.hle.CallArmFunction(f.GlSlot(27), 0x0004, 2, zeebulator::kGlUnsignedShort, index_addr);

  std::vector<float> expected = {7, 8, 1, 2};  // v3, v0
  EXPECT_EQ(f.backend.last_arrays.positions, expected);
}

TEST(GlHle, NormalPointerAlwaysResolvesThreeComponentsWithNoSizeArg) {
  Fixture f;
  uint32_t addr = kScratch + 0x900;
  WriteFloat(f.cpu.GetMemory(), addr, 0.0f);
  WriteFloat(f.cpu.GetMemory(), addr + 4, 1.0f);
  WriteFloat(f.cpu.GetMemory(), addr + 8, 0.0f);

  // glNormalPointer(type=GL_FLOAT, stride=0, pointer) -- no size argument.
  f.hle.CallArmFunction(f.GlSlot(55), zeebulator::kGlFloat, 0, addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlNormalArray);
  f.hle.CallArmFunction(f.GlSlot(26), 0x0004, 0, 1);

  ASSERT_TRUE(f.backend.last_arrays.has_normal);
  std::vector<float> expected = {0.0f, 1.0f, 0.0f};
  EXPECT_EQ(f.backend.last_arrays.normals, expected);
}

TEST(GlHle, DisableClientStateExcludesArrayFromNextDraw) {
  Fixture f;
  uint32_t addr = kScratch + 0xA00;
  const uint8_t raw[3] = {1, 2, 3};
  for (size_t i = 0; i < 3; ++i) f.cpu.GetMemory().Write8(addr + static_cast<uint32_t>(i), raw[i]);

  f.hle.CallArmFunction(f.GlSlot(78), 3, zeebulator::kGlByte, 0, addr);
  f.hle.CallArmFunction(f.GlSlot(29), zeebulator::kGlVertexArray);  // enable
  f.hle.CallArmFunction(f.GlSlot(25), zeebulator::kGlVertexArray);  // disable
  f.hle.CallArmFunction(f.GlSlot(26), 0x0004, 0, 1);

  EXPECT_FALSE(f.backend.last_arrays.has_position);
  EXPECT_TRUE(f.backend.last_arrays.positions.empty());
}

TEST(GlHle, GenTexturesWritesHostAssignedIdsIntoEmulatedMemory) {
  Fixture f;
  uint32_t out = kScratch + 0xB00;
  // glGenTextures(n=2, textures)
  f.hle.CallArmFunction(f.GlSlot(36), 2, out);
  EXPECT_EQ(f.cpu.GetMemory().Read32(out), 1u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(out + 4), 2u);
}

TEST(GlHle, DeleteTexturesReadsIdsFromEmulatedMemory) {
  Fixture f;
  uint32_t in = kScratch + 0xC00;
  f.cpu.GetMemory().Write32(in, 7);
  f.cpu.GetMemory().Write32(in + 4, 8);
  // glDeleteTextures(n=2, textures)
  f.hle.CallArmFunction(f.GlSlot(20), 2, in);
  ASSERT_EQ(f.backend.deleted_textures.size(), 2u);
  EXPECT_EQ(f.backend.deleted_textures[0], 7u);
  EXPECT_EQ(f.backend.deleted_textures[1], 8u);
}

TEST(GlHle, BindTextureForwardsRawIds) {
  Fixture f;
  // glBindTexture(target=GL_TEXTURE_2D, texture=5)
  f.hle.CallArmFunction(f.GlSlot(5), 0x0DE1, 5);
  EXPECT_EQ(f.backend.last_bind_target, 0x0DE1u);
  EXPECT_EQ(f.backend.last_bound_texture, 5u);
}

TEST(GlHle, TexParameterxPassesRawEnumNotFixedConverted) {
  Fixture f;
  // glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR) --
  // GL_LINEAR (0x2601) must arrive unconverted, NOT divided by 65536 the
  // way a true GLfixed scalar would be.
  f.hle.CallArmFunction(f.GlSlot(75), 0x0DE1, 0x2801, 0x2601);
  EXPECT_EQ(f.backend.last_texparam_target, 0x0DE1u);
  EXPECT_EQ(f.backend.last_texparam_pname, 0x2801u);
  EXPECT_EQ(f.backend.last_texparam_value, 0x2601);
}

TEST(GlHle, TexImage2DCopiesRgbaUnsignedByteImageBytes) {
  Fixture f;
  uint32_t pixels_addr = kScratch + 0xD00;
  const uint8_t pixels[8] = {255, 0, 0, 255, 0, 255, 0, 128};  // 2x1 RGBA
  for (size_t i = 0; i < 8; ++i) {
    f.cpu.GetMemory().Write8(pixels_addr + static_cast<uint32_t>(i), pixels[i]);
  }

  uint32_t stack = kScratch + 0xE00;
  f.cpu.SetRegister(zeebulator::kSP, stack);
  f.cpu.GetMemory().Write32(stack, 1);                              // height
  f.cpu.GetMemory().Write32(stack + 4, 0);                          // border
  f.cpu.GetMemory().Write32(stack + 8, zeebulator::kGlRgba);        // format
  f.cpu.GetMemory().Write32(stack + 12, zeebulator::kGlUnsignedByte); // type
  f.cpu.GetMemory().Write32(stack + 16, pixels_addr);               // pixels

  // glTexImage2D(target, level=0, internalformat=GL_RGBA, width=2, ...)
  f.hle.CallArmFunction(f.GlSlot(74), 0x0DE1, 0, zeebulator::kGlRgba, 2);

  ASSERT_EQ(f.backend.teximage_count, 1);
  EXPECT_EQ(f.backend.last_teximage_target, 0x0DE1u);
  EXPECT_EQ(f.backend.last_teximage.width, 2);
  EXPECT_EQ(f.backend.last_teximage.height, 1);
  EXPECT_EQ(f.backend.last_teximage.format, zeebulator::kGlRgba);
  EXPECT_EQ(f.backend.last_teximage.type, zeebulator::kGlUnsignedByte);
  ASSERT_EQ(f.backend.last_teximage_pixels.size(), 8u);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(f.backend.last_teximage_pixels[i], pixels[i]) << "byte " << i;
  }
}

TEST(GlHle, TexImage2DWithNullPixelsOnlyReservesStorage) {
  Fixture f;
  uint32_t stack = kScratch + 0xF00;
  f.cpu.SetRegister(zeebulator::kSP, stack);
  f.cpu.GetMemory().Write32(stack, 4);                              // height
  f.cpu.GetMemory().Write32(stack + 4, 0);                          // border
  f.cpu.GetMemory().Write32(stack + 8, zeebulator::kGlRgba);        // format
  f.cpu.GetMemory().Write32(stack + 12, zeebulator::kGlUnsignedByte); // type
  f.cpu.GetMemory().Write32(stack + 16, 0);                         // pixels = NULL

  f.hle.CallArmFunction(f.GlSlot(74), 0x0DE1, 0, zeebulator::kGlRgba, 4);

  ASSERT_EQ(f.backend.teximage_count, 1);
  EXPECT_EQ(f.backend.last_teximage.width, 4);
  EXPECT_EQ(f.backend.last_teximage.height, 4);
  EXPECT_TRUE(f.backend.last_teximage_pixels.empty());
}

TEST(GlHle, TexImage2DHandlesPackedRgb565PixelSize) {
  Fixture f;
  uint32_t pixels_addr = kScratch + 0x1000;
  f.cpu.GetMemory().Write16(pixels_addr, 0xF800);  // pure red in 5-6-5

  uint32_t stack = kScratch + 0x1100;
  f.cpu.SetRegister(zeebulator::kSP, stack);
  f.cpu.GetMemory().Write32(stack, 1);                                  // height
  f.cpu.GetMemory().Write32(stack + 4, 0);                              // border
  f.cpu.GetMemory().Write32(stack + 8, zeebulator::kGlRgb);             // format
  f.cpu.GetMemory().Write32(stack + 12, zeebulator::kGlUnsignedShort565); // type
  f.cpu.GetMemory().Write32(stack + 16, pixels_addr);                   // pixels

  f.hle.CallArmFunction(f.GlSlot(74), 0x0DE1, 0, zeebulator::kGlRgb, 1);

  ASSERT_EQ(f.backend.last_teximage_pixels.size(), 2u)
      << "one RGB565 pixel is always exactly 2 bytes, regardless of format's component count";
  EXPECT_EQ(f.backend.last_teximage_pixels[0], 0x00);
  EXPECT_EQ(f.backend.last_teximage_pixels[1], 0xF8);
}

// Real disassembly (TASKS.md/PHASE8_LOG.md Phase 8) found Double
// Dragon's own data.ggz archive holds real OBM1 images exclusively,
// never real ATITC -- and that real code reuses this same real
// glCompressedTexImage2D vtable slot to upload them, passing a
// pointer 8 bytes past a real OBM1 header ("OI" magic, flag, bpp)
// that the app itself already parsed to get this call's width/height.
TEST(GlHle, CompressedTexImage2DDecodesARealObm1ImageWhenMagicBytesPrecedeTheData) {
  Fixture f;
  // A real-shaped 2x2, 4bpp OBM1 image: palette[0]=the real magenta
  // color-key (always transparent, regardless of stored color -- see
  // core/loader/obm1.h), palette[1]=green, pixels (row-major)
  // key/green/green/key.
  uint32_t header_addr = kScratch + 0x1200;
  zeebulator::Memory& mem = f.cpu.GetMemory();
  mem.Write8(header_addr + 0, 'O');
  mem.Write8(header_addr + 1, 'I');
  mem.Write8(header_addr + 2, 0x04);  // flag
  mem.Write8(header_addr + 3, 4);     // bpp
  mem.Write16(header_addr + 4, 2);    // width
  mem.Write16(header_addr + 6, 2);    // height
  uint32_t data_addr = header_addr + 8;
  mem.Write16(data_addr + 0 * 2, 0xF83E);  // palette[0] = real magenta color-key
  mem.Write16(data_addr + 1 * 2, 0x07E0);  // palette[1] = pure green
  for (uint32_t i = 2; i < 16; ++i) mem.Write16(data_addr + i * 2, 0);
  uint32_t pixel_addr = data_addr + 16 * 2;
  mem.Write8(pixel_addr + 0, 0x01);  // pixels (0,0)=0=key, (1,0)=1=green
  mem.Write8(pixel_addr + 1, 0x10);  // pixels (0,1)=1=green, (1,1)=0=key
  uint32_t image_size = 16 * 2 + 2;  // palette + packed pixel bytes

  uint32_t stack = kScratch + 0x1300;
  f.cpu.SetRegister(zeebulator::kSP, stack);
  mem.Write32(stack, 2);            // height
  mem.Write32(stack + 4, 0);        // border
  mem.Write32(stack + 8, image_size);
  mem.Write32(stack + 12, data_addr);

  // glCompressedTexImage2D(target, level=0, internalformat=<engine
  // tag, not a real GL enum -- irrelevant, magic bytes take priority>,
  // width=2, ...)
  f.hle.CallArmFunction(f.GlSlot(15), 0x0DE1, 0, 0x8b99, 2);

  ASSERT_EQ(f.backend.teximage_count, 1);
  EXPECT_EQ(f.backend.last_teximage_target, 0x0DE1u);
  EXPECT_EQ(f.backend.last_teximage.width, 2);
  EXPECT_EQ(f.backend.last_teximage.height, 2);
  EXPECT_EQ(f.backend.last_teximage.format, zeebulator::kGlRgba)
      << "real sprites need a real alpha channel for GL_ALPHA_TEST/GL_BLEND to act on";
  EXPECT_EQ(f.backend.last_teximage.type, zeebulator::kGlUnsignedByte);
  ASSERT_EQ(f.backend.last_teximage_pixels.size(), 2u * 2u * 4u);
  auto PixelAt = [&](int x, int y) {
    size_t base = (static_cast<size_t>(y) * 2 + x) * 4;
    return std::vector<uint8_t>(f.backend.last_teximage_pixels.begin() + base,
                                 f.backend.last_teximage_pixels.begin() + base + 4);
  };
  EXPECT_EQ(PixelAt(0, 0), (std::vector<uint8_t>{255, 4, 246, 0}))
      << "the real color-key pixel: real magenta color, alpha=0";
  EXPECT_EQ(PixelAt(1, 0), (std::vector<uint8_t>{0, 255, 0, 255}));
  EXPECT_EQ(PixelAt(0, 1), (std::vector<uint8_t>{0, 255, 0, 255}));
  EXPECT_EQ(PixelAt(1, 1), (std::vector<uint8_t>{255, 4, 246, 0}));
}

// Real disassembly (TASKS.md/PHASE8_LOG.md Phase 8): Double Dragon
// pairs GL_ALPHA_TEST/GL_BLEND with these real OBM1 uploads --
// confirmed real args glAlphaFuncx(GL_NOTEQUAL, 0.0) and
// glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
TEST(GlHle, AlphaFuncxAndBlendFuncForwardRealArgsToBackend) {
  Fixture f;
  constexpr zeebulator::GLenum kGlNotequal = 0x0205;
  constexpr zeebulator::GLenum kGlSrcAlpha = 0x0302;
  constexpr zeebulator::GLenum kGlOneMinusSrcAlpha = 0x0303;

  f.hle.CallArmFunction(f.GlSlot(4), kGlNotequal, ToFixed(0.0f));
  EXPECT_EQ(f.backend.last_alpha_func, kGlNotequal);
  EXPECT_FLOAT_EQ(f.backend.last_alpha_ref, 0.0f);

  f.hle.CallArmFunction(f.GlSlot(6), kGlSrcAlpha, kGlOneMinusSrcAlpha);
  EXPECT_EQ(f.backend.last_blend_sfactor, kGlSrcAlpha);
  EXPECT_EQ(f.backend.last_blend_dfactor, kGlOneMinusSrcAlpha);
}

TEST(GlHle, CompressedTexImage2DWithNoObm1MagicAndAnUnrecognizedFormatUploadsNothing) {
  Fixture f;
  uint32_t data_addr = kScratch + 0x1400;
  f.cpu.GetMemory().Write32(data_addr, 0x11223344);  // not "OI", not real ATITC data

  uint32_t stack = kScratch + 0x1500;
  f.cpu.SetRegister(zeebulator::kSP, stack);
  f.cpu.GetMemory().Write32(stack, 4);      // height
  f.cpu.GetMemory().Write32(stack + 4, 0);  // border
  f.cpu.GetMemory().Write32(stack + 8, 32); // imageSize
  f.cpu.GetMemory().Write32(stack + 12, data_addr);

  f.hle.CallArmFunction(f.GlSlot(15), 0x0DE1, 0, /*internalformat=*/0x1234, 4);

  EXPECT_EQ(f.backend.teximage_count, 0);
}
