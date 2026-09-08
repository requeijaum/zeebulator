#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/brew/gl_backend.h"

namespace zeebulator {

// Rasterizador de software OpenGL ES 1.1 -- a implementação concreta de
// GlBackend que efetivamente PINTA a geometria GL num framebuffer RGB565,
// destravando o teto "GL-HLE não rasteriza offscreen" (UPDATE 19). As
// outras concretas eram (1) GlTextureLog, um gravador que só loga, e (2)
// Sdl2GlBackend, que precisa de uma janela/contexto GL de desktop real e
// só existe no app standalone -- nenhuma servia ao game_probe headless,
// onde o ddragonz emitia Clear + milhares de GL_TRIANGLES sem nada
// aparecer na tela.
//
// CLEAN-ROOM: todo o código foi escrito do zero, em C++, no estilo do
// resto de core/brew/. A ESTRUTURA/ALGORITMO (setup de triângulo por
// função de aresta, caixa envolvente, interpolação perspectiva-correta,
// amostra de textura com wrap/filtro, pilha de matrizes 4x4, recorte no
// plano próximo) segue a teoria pública padrão de rasterização e o
// mapeamento fixed-function do GLES1.1; nenhuma linha, nome de header
// proprietário ou artefato derivado do vendor Qualcomm foi copiado.
//
// Escopo mínimo para o ddragonz (sprites 2D/2.5D via GL_TRIANGLES com
// textura + alpha): pilha MODELVIEW/PROJECTION com Ortho/Frustum/
// Translate/Rotate/Scale/Push/Pop; transform model->clip->NDC->viewport;
// rasterização por edge-function; interpolação de cor/texcoord
// perspectiva-correta; textura RGBA com CLAMP_TO_EDGE/REPEAT e
// NEAREST/LINEAR; alpha test; alpha blend; z-buffer float; Clear de cor
// e profundidade; escrita final em RGB565.
class SoftGlBackend : public GlBackend {
 public:
  // `framebuffer` é o buffer RGB565 vivo do IDisplayHle (via
  // MutableFramebuffer()) -- compor NELE é o que faz o screenshot enxergar
  // o resultado. `present` é chamado no SwapBuffers para commitar o frame
  // (o equivalente a PresentLiveFramebuffer). `on_present` pode ser nulo.
  SoftGlBackend(std::vector<uint16_t>& framebuffer, int width, int height);

  // Callback opcional disparado em cada SwapBuffers (game_probe liga isso a
  // display.PresentLiveFramebuffer()).
  void SetPresentCallback(void (*on_present)(void*), void* user) {
    on_present_ = on_present;
    present_user_ = user;
  }

  bool CreateContext() override { return true; }
  void DestroyContext() override {}
  void SwapBuffers() override;

  void Clear(GLbitfield mask) override;
  void ClearColor(float r, float g, float b, float a) override;
  void Viewport(int x, int y, int width, int height) override;
  void Enable(GLenum cap) override;
  void Disable(GLenum cap) override;
  void MatrixMode(GLenum mode) override;
  void LoadIdentity() override;
  void LoadMatrix(const float m[16]) override;
  void MultMatrix(const float m[16]) override;
  void PushMatrix() override;
  void PopMatrix() override;
  void Ortho(float left, float right, float bottom, float top, float near_plane,
             float far_plane) override;
  void Frustum(float left, float right, float bottom, float top, float near_plane,
               float far_plane) override;
  void Translate(float x, float y, float z) override;
  void Rotate(float angle_degrees, float x, float y, float z) override;
  void Scale(float x, float y, float z) override;
  void Color4(float r, float g, float b, float a) override;
  void AlphaFunc(GLenum func, float ref) override;
  void BlendFunc(GLenum sfactor, GLenum dfactor) override;
  void DepthFunc(GLenum func) override;
  void ClearDepth(float depth) override;
  void DepthMask(bool flag) override;
  void DrawArrays(GLenum mode, const GlVertexArrays& arrays) override;

  void GenTextures(GLsizei n, GLuint* textures) override;
  void DeleteTextures(GLsizei n, const GLuint* textures) override;
  void BindTexture(GLenum target, GLuint texture) override;
  void TexParameter(GLenum target, GLenum pname, GLint param) override;
  void TexImage2D(GLenum target, const GlTextureImage& image) override;

  // --- Superfície de teste (unit tests) ---------------------------------
  // Expostos para os testes verificarem cobertura de pixel/blend/textura
  // sem precisar de um IDisplay/emulador completo.
  const std::vector<uint16_t>& framebuffer() const { return framebuffer_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  // Matriz 4x4 em ordem column-major (mesma convenção do OpenGL), guardada
  // linearmente: m[col*4 + row].
  using Matrix = std::array<float, 16>;

  struct Vertex {
    std::array<float, 4> position;  // clip-space (após projeção)
    std::array<float, 4> color;
    std::array<float, 2> uv;
  };

  struct Texture {
    int width = 0;
    int height = 0;
    std::vector<std::array<uint8_t, 4>> pixels;  // RGBA, row-major
    GLenum wrap_s = 0x812F;  // GL_CLAMP_TO_EDGE
    GLenum wrap_t = 0x812F;
    GLenum min_filter = 0x2601;  // GL_LINEAR
    GLenum mag_filter = 0x2601;
    std::array<float, 4> Sample(float u, float v) const;
    std::array<float, 4> Texel(int x, int y) const;
  };

  std::vector<Matrix>& CurrentStack();
  Matrix& Top();
  void MultTop(const Matrix& m);
  void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c);
  void ClipAndDraw(Vertex a, Vertex b, Vertex c);
  void RasterizePrepared(const Vertex& v0, const Vertex& v1, const Vertex& v2);

  std::vector<uint16_t>& framebuffer_;
  // Buffer de cor interno RGBA8 (como o zeebx): todo blend acontece aqui em 8
  // bits e so e quantizado para o framebuffer_ 565 do IDisplay no SwapBuffers.
  // Evita acumulo de erro de arredondamento por-fragmento (155 draws sobrepostos
  // escureciam a cena quando o blend lia/escrevia direto no 565).
  std::vector<std::array<uint8_t, 3>> color_;
  int width_;
  int height_;
  std::vector<float> depth_;  // z-buffer, width_*height_, NDC [0,1]

  // Pilhas de matrizes. MODELVIEW e PROJECTION são as duas que o escopo
  // mínimo precisa (o ddragonz não usa TEXTURE matrix).
  std::vector<Matrix> modelview_{1};
  std::vector<Matrix> projection_{1};
  GLenum matrix_mode_ = 0x1700;  // GL_MODELVIEW

  int vp_x_ = 0, vp_y_ = 0, vp_w_ = 0, vp_h_ = 0;
  std::array<float, 4> clear_color_{0, 0, 0, 1};
  float clear_depth_ = 1.0f;
  std::array<float, 4> current_color_{1, 1, 1, 1};

  bool blend_ = false;
  GLenum blend_src_ = 1;   // GL_ONE
  GLenum blend_dst_ = 0;   // GL_ZERO
  bool depth_test_ = false;
  GLenum depth_func_ = 0x0201;  // GL_LESS
  bool depth_mask_ = true;
  bool alpha_test_ = false;
  GLenum alpha_func_ = 0x0207;  // GL_ALWAYS
  float alpha_ref_ = 0.0f;
  bool texture_2d_ = false;

  std::unordered_map<GLuint, Texture> textures_;
  GLuint next_texture_ = 1;
  GLuint bound_texture_ = 0;

  void (*on_present_)(void*) = nullptr;
  void* present_user_ = nullptr;
};

}  // namespace zeebulator
