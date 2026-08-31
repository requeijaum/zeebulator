#pragma once

#include <SDL.h>

#include "core/brew/gl_backend.h"

namespace zeebulator {

// Real host OpenGL implementation of GlBackend, for the standalone SDL2
// frontend. Owns one real SDL_GLContext on the given window -- created
// lazily in CreateContext() (mirroring EGL's own "no context exists until
// eglMakeCurrent actually needs one" lifecycle), torn down in
// DestroyContext(). Every method is a thin, direct forward to the
// platform's real OpenGL 1.1 fixed-function entry points (glClear,
// glVertexPointer, glDrawArrays, ...) -- GlHle has already done all
// GLfixed/emulated-memory marshaling before any call reaches here, so
// there's no emulation-specific logic in this file at all.
class Sdl2GlBackend : public GlBackend {
 public:
  explicit Sdl2GlBackend(SDL_Window* window);
  ~Sdl2GlBackend() override;

  bool CreateContext() override;
  void DestroyContext() override;
  void SwapBuffers() override;

  void Clear(GLbitfield mask) override;
  void ClearColor(float r, float g, float b, float a) override;
  void Viewport(int x, int y, int width, int height) override;
  void Enable(GLenum cap) override;
  void Disable(GLenum cap) override;
  void MatrixMode(GLenum mode) override;
  void LoadIdentity() override;
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

 private:
  SDL_Window* window_;
  SDL_GLContext gl_context_ = nullptr;
};

}  // namespace zeebulator
