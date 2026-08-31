#include "frontends/standalone/sdl2_gl_backend.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace zeebulator {

Sdl2GlBackend::Sdl2GlBackend(SDL_Window* window) : window_(window) {}

Sdl2GlBackend::~Sdl2GlBackend() { DestroyContext(); }

bool Sdl2GlBackend::CreateContext() {
  if (gl_context_ != nullptr) return true;
  gl_context_ = SDL_GL_CreateContext(window_);
  if (gl_context_ == nullptr) {
    std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(window_, gl_context_);
  return true;
}

void Sdl2GlBackend::DestroyContext() {
  if (gl_context_ != nullptr) {
    SDL_GL_DeleteContext(gl_context_);
    gl_context_ = nullptr;
  }
}

void Sdl2GlBackend::SwapBuffers() { SDL_GL_SwapWindow(window_); }

void Sdl2GlBackend::Clear(GLbitfield mask) { glClear(mask); }
void Sdl2GlBackend::ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void Sdl2GlBackend::Viewport(int x, int y, int width, int height) {
  glViewport(x, y, width, height);
}
void Sdl2GlBackend::Enable(GLenum cap) { glEnable(cap); }
void Sdl2GlBackend::Disable(GLenum cap) { glDisable(cap); }
void Sdl2GlBackend::MatrixMode(GLenum mode) { glMatrixMode(mode); }
void Sdl2GlBackend::LoadIdentity() { glLoadIdentity(); }
void Sdl2GlBackend::PushMatrix() { glPushMatrix(); }
void Sdl2GlBackend::PopMatrix() { glPopMatrix(); }
void Sdl2GlBackend::Ortho(float left, float right, float bottom, float top, float near_plane,
                           float far_plane) {
  glOrtho(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2GlBackend::Frustum(float left, float right, float bottom, float top, float near_plane,
                             float far_plane) {
  glFrustum(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2GlBackend::Translate(float x, float y, float z) { glTranslatef(x, y, z); }
void Sdl2GlBackend::Rotate(float angle_degrees, float x, float y, float z) {
  glRotatef(angle_degrees, x, y, z);
}
void Sdl2GlBackend::Scale(float x, float y, float z) { glScalef(x, y, z); }
void Sdl2GlBackend::Color4(float r, float g, float b, float a) { glColor4f(r, g, b, a); }
void Sdl2GlBackend::AlphaFunc(GLenum func, float ref) { glAlphaFunc(func, ref); }
void Sdl2GlBackend::BlendFunc(GLenum sfactor, GLenum dfactor) { glBlendFunc(sfactor, dfactor); }
void Sdl2GlBackend::DepthFunc(GLenum func) { glDepthFunc(func); }
void Sdl2GlBackend::ClearDepth(float depth) { glClearDepth(static_cast<GLdouble>(depth)); }
void Sdl2GlBackend::DepthMask(bool flag) { glDepthMask(flag ? GL_TRUE : GL_FALSE); }

void Sdl2GlBackend::DrawArrays(GLenum mode, const GlVertexArrays& arrays) {
  if (arrays.has_position) {
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(arrays.position_size, GL_FLOAT, 0, arrays.positions.data());
  } else {
    glDisableClientState(GL_VERTEX_ARRAY);
  }
  if (arrays.has_color) {
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, arrays.colors.data());
  } else {
    glDisableClientState(GL_COLOR_ARRAY);
  }
  if (arrays.has_texcoord) {
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(arrays.texcoord_size, GL_FLOAT, 0, arrays.texcoords.data());
  } else {
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  }
  if (arrays.has_normal) {
    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, 0, arrays.normals.data());
  } else {
    glDisableClientState(GL_NORMAL_ARRAY);
  }
  glDrawArrays(mode, 0, arrays.vertex_count);
}

void Sdl2GlBackend::GenTextures(GLsizei n, GLuint* textures) { glGenTextures(n, textures); }
void Sdl2GlBackend::DeleteTextures(GLsizei n, const GLuint* textures) {
  glDeleteTextures(n, textures);
}
void Sdl2GlBackend::BindTexture(GLenum target, GLuint texture) { glBindTexture(target, texture); }
void Sdl2GlBackend::TexParameter(GLenum target, GLenum pname, GLint param) {
  glTexParameteri(target, pname, param);
}
void Sdl2GlBackend::TexImage2D(GLenum target, const GlTextureImage& image) {
  glTexImage2D(target, image.level, static_cast<GLint>(image.internal_format), image.width,
               image.height, /*border=*/0, image.format, image.type, image.pixels);
}

}  // namespace zeebulator
