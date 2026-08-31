#pragma once

#include <vector>

#include "core/brew/gl_types.h"

namespace zeebulator {

// Fully host-resolved vertex attribute arrays for one draw call: GlHle
// has already walked emulated memory (per each array's real
// pointer/type/stride/size) and converted every component to a float, so
// GlBackend implementations never need to touch emulated memory or
// GLfixed/GLbyte/GLshort decoding themselves. `vertex_count` values are
// present in every enabled array, in draw order -- for glDrawElements
// that means already de-indexed (attributes gathered per index, in the
// index buffer's order), not the original vertex-buffer order.
struct GlVertexArrays {
  bool has_position = false;
  int position_size = 0;  // components per vertex: 2, 3, or 4
  std::vector<float> positions;

  bool has_color = false;  // always 4 components (r,g,b,a) per vertex
  std::vector<float> colors;

  bool has_texcoord = false;
  int texcoord_size = 0;  // components per vertex: 2, 3, or 4
  std::vector<float> texcoords;

  bool has_normal = false;  // always 3 components (x,y,z) per vertex
  std::vector<float> normals;

  int vertex_count = 0;
};

// A fully host-resolved glTexImage2D upload: GlHle has already copied the
// real pixel bytes out of emulated memory (when present) into a
// host-owned buffer, so `pixels` is a real host pointer (or null, meaning
// "reserve storage of this size/format, no data yet" -- the real, legal
// glTexImage2D meaning of a null pixels argument, used before a series of
// glTexSubImage2D calls).
struct GlTextureImage {
  int level = 0;
  GLenum internal_format = 0;
  int width = 0;
  int height = 0;
  GLenum format = 0;
  GLenum type = 0;
  const uint8_t* pixels = nullptr;
};

// The seam between GlHle's vtable dispatch and a real host graphics
// context, mirroring how Backend (core/backend.h) seams the rest of the
// core off from the outside world. GlHle marshals real IGL/IEGL vtable
// calls (emulated ARM registers, GLfixed args, emulated-memory pointers)
// down to this host-native surface (floats, real host pointers); a
// frontend implements it against real desktop OpenGL, tests implement it
// against a recording fake -- neither the marshaling logic nor its tests
// need a live GPU context.
//
// Method names/signatures intentionally mirror the real GLES1.x/EGL
// entry points closely (so the mapping in GlHle is obvious) but are not
// copied from any Qualcomm or Khronos header -- see gl_types.h.
class GlBackend {
 public:
  virtual ~GlBackend() = default;

  // EGL-ish lifecycle. GlHle owns all EGLDisplay/EGLSurface/EGLContext/
  // EGLConfig handle bookkeeping itself (see gl_hle.cc) -- GlBackend only
  // needs to actually stand up, or tear down, one real host GL context.
  virtual bool CreateContext() = 0;
  virtual void DestroyContext() = 0;
  virtual void SwapBuffers() = 0;

  // Core GL state / transform calls, host-native (float, not GLfixed).
  // GLenum/GLbitfield are passed through as-is: their values are the same
  // industry-standard Khronos constants on every real GL/GLES
  // implementation, so no translation is needed for these two types.
  virtual void Clear(GLbitfield mask) = 0;
  virtual void ClearColor(float r, float g, float b, float a) = 0;
  virtual void Viewport(int x, int y, int width, int height) = 0;
  virtual void Enable(GLenum cap) = 0;
  virtual void Disable(GLenum cap) = 0;
  virtual void MatrixMode(GLenum mode) = 0;
  virtual void LoadIdentity() = 0;
  // Real, confirmed-needed for real per-sprite transform scoping
  // (TASKS.md/PHASE8_LOG.md Phase 8): both were silent no-op Stubs
  // until real front/back sprite ordering was found live-broken --
  // real code's standard "glPushMatrix(); glTranslatef(...); <draw
  // one sprite>; glPopMatrix();" per-sprite pattern needs a real
  // matrix stack, or every sprite's translate keeps accumulating onto
  // the previous one instead of being independently scoped from a
  // shared base, corrupting every subsequent sprite's real position
  // (Z included) for the rest of that frame.
  virtual void PushMatrix() = 0;
  virtual void PopMatrix() = 0;
  virtual void Ortho(float left, float right, float bottom, float top,
                      float near_plane, float far_plane) = 0;
  virtual void Frustum(float left, float right, float bottom, float top,
                        float near_plane, float far_plane) = 0;
  virtual void Translate(float x, float y, float z) = 0;
  virtual void Rotate(float angle_degrees, float x, float y, float z) = 0;
  virtual void Scale(float x, float y, float z) = 0;
  virtual void Color4(float r, float g, float b, float a) = 0;
  // Real, confirmed-needed for real OBM1 sprite transparency (TASKS.md/
  // PHASE8_LOG.md Phase 8): Double Dragon's own real disassembly shows
  // it pairing GL_ALPHA_TEST (glAlphaFuncx(GL_NOTEQUAL, 0.0), discarding
  // exactly the real magenta-color-key pixels this project's OBM1
  // decoder now outputs as alpha=0) with GL_BLEND (glBlendFunc(
  // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)) before drawing sprites --
  // the standard real GLES1.x combination. `func`/`sfactor`/`dfactor`
  // pass through as raw Khronos enums, same rationale as Enable/Disable.
  virtual void AlphaFunc(GLenum func, float ref) = 0;
  virtual void BlendFunc(GLenum sfactor, GLenum dfactor) = 0;
  // Real, confirmed-needed for real sprite/HUD layering (TASKS.md/
  // PHASE8_LOG.md Phase 8): Double Dragon enables GL_DEPTH_TEST once at
  // startup and never touches it again, relying on a real depth buffer
  // for correct front/back ordering -- confirmed live via the actual,
  // visible symptom of not having one wired up (enemies and a
  // health-bar fill rendered in the wrong order, i.e. submission order
  // instead of real depth order). `func` passes through as a raw
  // Khronos enum, same rationale as Enable/Disable.
  virtual void DepthFunc(GLenum func) = 0;
  virtual void ClearDepth(float depth) = 0;
  virtual void DepthMask(bool flag) = 0;

  // A single draw call covers both glDrawArrays and glDrawElements --
  // GlHle resolves either into the same already-host-native
  // GlVertexArrays shape before calling this (see gl_hle.cc).
  virtual void DrawArrays(GLenum mode, const GlVertexArrays& arrays) = 0;

  // Texture object management + upload. GLenum/GLuint values (target,
  // texture names, pname) pass through as-is -- same rationale as
  // Enable/Disable above.
  virtual void GenTextures(GLsizei n, GLuint* textures) = 0;
  virtual void DeleteTextures(GLsizei n, const GLuint* textures) = 0;
  virtual void BindTexture(GLenum target, GLuint texture) = 0;
  // `param` is the raw integer value, not GLfixed-converted: every
  // standard GLES1.x glTexParameterx pname (MIN/MAG_FILTER, WRAP_S/T) is
  // enum-valued, and the real spec's "x" convention for enum parameters
  // is to pass the enum's raw integer through the fixed-point slot
  // unconverted, not scaled by 65536.
  virtual void TexParameter(GLenum target, GLenum pname, GLint param) = 0;
  virtual void TexImage2D(GLenum target, const GlTextureImage& image) = 0;
};

}  // namespace zeebulator
