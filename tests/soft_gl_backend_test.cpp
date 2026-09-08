#include "core/brew/soft_gl_backend.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"

namespace zeebulator {
namespace {

// Enums Khronos padrão usados nos testes.
constexpr GLenum kGL_TRIANGLES = 0x0004;
constexpr GLbitfield kGL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLbitfield kGL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum kGL_BLEND = 0x0BE2;
constexpr GLenum kGL_SRC_ALPHA = 0x0302;
constexpr GLenum kGL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum kGL_ALPHA_TEST = 0x0BC0;
constexpr GLenum kGL_NOTEQUAL = 0x0205;
constexpr GLenum kGL_TEXTURE_2D = 0x0DE1;
constexpr GLenum kGL_TEXTURE_2D_TARGET = 0x0DE1;

// Framebuffer 16x16 para os testes.
constexpr int kW = 16;
constexpr int kH = 16;

std::array<uint8_t, 3> Rgb565ToRgb(uint16_t p) {
  return {static_cast<uint8_t>(((p >> 11) & 0x1F) << 3),
          static_cast<uint8_t>(((p >> 5) & 0x3F) << 2),
          static_cast<uint8_t>((p & 0x1F) << 3)};
}

// Monta um quad (2 triângulos) cobrindo NDC [-1,1] com cor sólida.
GlVertexArrays FullscreenQuad(std::array<float, 4> color, bool with_uv = false) {
  GlVertexArrays a;
  a.has_position = true;
  a.position_size = 3;
  a.vertex_count = 6;
  // (x,y) em NDC; z=0.
  const float pts[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
  const float uvs[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  for (int i = 0; i < 6; ++i) {
    a.positions.push_back(pts[i][0]);
    a.positions.push_back(pts[i][1]);
    a.positions.push_back(0.0f);
    a.has_color = true;
    a.colors.push_back(color[0]);
    a.colors.push_back(color[1]);
    a.colors.push_back(color[2]);
    a.colors.push_back(color[3]);
    if (with_uv) {
      a.has_texcoord = true;
      a.texcoord_size = 2;
      a.texcoords.push_back(uvs[i][0]);
      a.texcoords.push_back(uvs[i][1]);
    }
  }
  return a;
}

TEST(SoftGlBackend, ClearFillsFramebuffer) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  gl.Clear(kGL_COLOR_BUFFER_BIT);
  for (uint16_t p : fb) {
    auto rgb = Rgb565ToRgb(p);
    EXPECT_GT(rgb[0], 240);  // vermelho forte
    EXPECT_LT(rgb[1], 16);
    EXPECT_LT(rgb[2], 16);
  }
}

TEST(SoftGlBackend, TriangleFillsCenterPixel) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.Viewport(0, 0, kW, kH);
  // Quad verde cobrindo a tela toda.
  GlVertexArrays quad = FullscreenQuad({0.0f, 1.0f, 0.0f, 1.0f});
  gl.DrawArrays(kGL_TRIANGLES, quad);
  // O pixel central deve ficar verde.
  uint16_t center = fb[(kH / 2) * kW + (kW / 2)];
  auto rgb = Rgb565ToRgb(center);
  EXPECT_LT(rgb[0], 16);
  EXPECT_GT(rgb[1], 240);
  EXPECT_LT(rgb[2], 16);
  // A tela deixa de ser uniformemente zero.
  int nonzero = 0;
  for (uint16_t p : fb)
    if (p != 0) ++nonzero;
  EXPECT_GT(nonzero, kW * kH / 2);
}

TEST(SoftGlBackend, AlphaBlendMixesWithBackground) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.Viewport(0, 0, kW, kH);
  // Fundo branco.
  gl.ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  gl.Clear(kGL_COLOR_BUFFER_BIT);
  // Quad preto com alpha 0.5, blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
  gl.Enable(kGL_BLEND);
  gl.BlendFunc(kGL_SRC_ALPHA, kGL_ONE_MINUS_SRC_ALPHA);
  GlVertexArrays quad = FullscreenQuad({0.0f, 0.0f, 0.0f, 0.5f});
  gl.DrawArrays(kGL_TRIANGLES, quad);
  // 0*0.5 + 1*0.5 = 0.5 -> cinza médio.
  uint16_t center = fb[(kH / 2) * kW + (kW / 2)];
  auto rgb = Rgb565ToRgb(center);
  EXPECT_GT(rgb[0], 100);
  EXPECT_LT(rgb[0], 160);
}

TEST(SoftGlBackend, AlphaTestDiscardsFragments) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.Viewport(0, 0, kW, kH);
  gl.Enable(kGL_ALPHA_TEST);
  gl.AlphaFunc(kGL_NOTEQUAL, 0.0f);  // descarta alpha==0
  GlVertexArrays quad = FullscreenQuad({0.0f, 1.0f, 0.0f, 0.0f});  // alpha 0
  gl.DrawArrays(kGL_TRIANGLES, quad);
  // Nada deve ter sido escrito -- todos os fragmentos foram descartados.
  int nonzero = 0;
  for (uint16_t p : fb)
    if (p != 0) ++nonzero;
  EXPECT_EQ(nonzero, 0);
}

TEST(SoftGlBackend, TextureSampleModulatesColor) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.Viewport(0, 0, kW, kH);
  // Textura 2x2 azul sólida, RGBA unsigned byte.
  GLuint tex = 0;
  gl.GenTextures(1, &tex);
  gl.BindTexture(kGL_TEXTURE_2D_TARGET, tex);
  std::vector<uint8_t> pixels(2 * 2 * 4);
  for (int i = 0; i < 4; ++i) {
    pixels[i * 4 + 0] = 0;
    pixels[i * 4 + 1] = 0;
    pixels[i * 4 + 2] = 255;
    pixels[i * 4 + 3] = 255;
  }
  GlTextureImage img;
  img.width = 2;
  img.height = 2;
  img.format = 0x1908;  // GL_RGBA
  img.type = 0x1401;    // GL_UNSIGNED_BYTE
  img.pixels = pixels.data();
  gl.TexImage2D(kGL_TEXTURE_2D_TARGET, img);
  gl.Enable(kGL_TEXTURE_2D);
  // Quad branco com UV -> modula: branco * azul = azul.
  GlVertexArrays quad = FullscreenQuad({1.0f, 1.0f, 1.0f, 1.0f}, /*with_uv=*/true);
  gl.DrawArrays(kGL_TRIANGLES, quad);
  uint16_t center = fb[(kH / 2) * kW + (kW / 2)];
  auto rgb = Rgb565ToRgb(center);
  EXPECT_LT(rgb[0], 16);
  EXPECT_LT(rgb[1], 16);
  EXPECT_GT(rgb[2], 240);
}

TEST(SoftGlBackend, DepthTestOrdersFragments) {
  std::vector<uint16_t> fb(kW * kH, 0);
  SoftGlBackend gl(fb, kW, kH);
  gl.Viewport(0, 0, kW, kH);
  gl.Enable(0x0B71);  // GL_DEPTH_TEST
  gl.DepthFunc(0x0201);  // GL_LESS
  gl.ClearDepth(1.0f);
  gl.Clear(kGL_DEPTH_BUFFER_BIT | kGL_COLOR_BUFFER_BIT);
  // Quad vermelho perto (z=-0.5), depois verde longe (z=+0.5): verde deve
  // falhar o teste de profundidade e não sobrescrever o vermelho.
  auto quad_z = [](std::array<float, 4> color, float z) {
    GlVertexArrays a;
    a.has_position = true;
    a.position_size = 3;
    a.vertex_count = 6;
    const float pts[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 6; ++i) {
      a.positions.push_back(pts[i][0]);
      a.positions.push_back(pts[i][1]);
      a.positions.push_back(z);
      a.has_color = true;
      a.colors.push_back(color[0]);
      a.colors.push_back(color[1]);
      a.colors.push_back(color[2]);
      a.colors.push_back(color[3]);
    }
    return a;
  };
  GlVertexArrays near_red = quad_z({1, 0, 0, 1}, -0.5f);
  GlVertexArrays far_green = quad_z({0, 1, 0, 1}, 0.5f);
  gl.DrawArrays(kGL_TRIANGLES, near_red);
  gl.DrawArrays(kGL_TRIANGLES, far_green);
  uint16_t center = fb[(kH / 2) * kW + (kW / 2)];
  auto rgb = Rgb565ToRgb(center);
  EXPECT_GT(rgb[0], 240);  // permanece vermelho
  EXPECT_LT(rgb[1], 16);
}

}  // namespace
}  // namespace zeebulator
