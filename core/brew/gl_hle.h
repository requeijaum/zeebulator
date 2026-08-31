#pragma once

#include <cstdint>
#include <vector>

#include "core/brew/gl_backend.h"
#include "core/brew/hle_runtime.h"
#include "core/brew/gl_types.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Real IGL/IEGL implementation backing the HLE vtables. A real game's
// `.mod` statically links Qualcomm's EGL_1x.c/GLES_1x.c wrapper source,
// which dispatches every gl*/egl* call through these two interfaces via
// global gpIGL/gpIEGL pointers set up once at startup -- see TASKS.md /
// ARCHITECTURE.md Phase 5 for how this was discovered and verified
// against the real AEEGL.h. Slot order: AddRef, Release, QueryInterface,
// then 77 gl* methods for IGL (80 slots total) / 25 egl* methods for
// IEGL (28 slots total), in AEEGL.h's declared order.
//
// Unlike every other BREW interface implemented so far (IShell/IDisplay/
// IFile), the real gl*/egl* vtable slots do NOT receive the interface
// object pointer as an implicit first argument -- confirmed from the real
// access macros (e.g. `#define IGL_glClear(p,a) AEEGETPVTBL(p,IGL)->
// glClear(a)`, which passes only `a`, not `p`, through to the function
// pointer). So for those slots, R0 holds the *first real argument*, not
// "po" -- only the inherited AddRef/Release/QueryInterface slots follow
// the usual po-in-R0 convention.
//
// Implemented so far: the EGL lifecycle (get/init/terminate a display,
// choose a config, create a window surface + context, make current, swap
// buffers), eglQueryString (real disassembly of Double Dragon --
// see PHASE8_LOG.md -- shows eglQueryString(EGL_EXTENSIONS)'s result fed
// straight into a strstr-shaped call with no null check, so unlike most
// other unimplemented slots this one can't stay a blind Stub; returns
// honest values -- an empty extensions string, since none are
// implemented -- never null, matching the real EGL spec's guarantee), a
// small set of core GL state/transform calls (clear,
// viewport, matrix stack, translate/rotate/scale, color), and vertex
// arrays + draw calls (glVertexPointer/glColorPointer/glTexCoordPointer/
// glNormalPointer, glEnableClientState/glDisableClientState,
// glDrawArrays/glDrawElements). Pointer arguments in the array-pointer
// calls are emulated ARM addresses, not host pointers -- they're stored
// as-is (pointer/type/size/stride) and only actually read from emulated
// memory at draw-call time, converting every component to a host float
// per the real GLES1.x per-type semantics (see gl_types.h's
// ReadGlComponent) before handing a fully host-native GlVertexArrays to
// GlBackend::DrawArrays. glDrawElements is implemented in terms of the
// same extraction, just keyed by an explicit index list (read from
// emulated memory per its GL_UNSIGNED_BYTE/GL_UNSIGNED_SHORT `type`
// argument) instead of a contiguous [first, first+count) range --
// equivalent output, not indexed rendering on the host side (a known,
// documented simplification, not an oversight).
// Also implemented: texture object management + upload
// (glGenTextures/glDeleteTextures/glBindTexture/glTexParameterx/
// glTexImage2D) -- glTexImage2D's pixel bytes are copied out of emulated
// memory into a host-owned buffer sized from the real (format, type,
// width, height), same emulated-memory-only-at-the-boundary approach as
// the vertex arrays above. glCompressedTexImage2D is implemented too,
// for the one real compressed format Double Dragon actually uses
// (GL_COMPRESSED_RGB_ATI_TC / GL_COMPRESSED_RGBA_ATI_TC -- confirmed via
// real disassembly showing glGenTextures/glBindTexture calls with no
// glTexImage2D ever following them, and this project's own bundled real
// Qualcomm ATITC sample; see TASKS.md/PHASE8_LOG.md Phase 8): decoded
// host-side via core/loader/atitc.h into plain RGBA8, then forwarded to
// GlBackend::TexImage2D like an ordinary uncompressed upload, since a
// real desktop host GL implementation won't have this real mobile-GPU-
// only extension. Real other formats (ETC1, PVRTC, ...) are NOT
// implemented -- this project has no evidence any real target game uses
// them.
// The rest of the fixed-function state (lighting, texture combiners,
// glTexSubImage2D, ...) is still Stubs (see TASKS.md Phase 5 for what's
// next). EGLDisplay/EGLSurface/EGLContext/
// EGLConfig handles are
// simulated as small sentinel integers; this class does not talk to any
// real host EGL implementation itself -- GlBackend is responsible for the
// one real host GL context underneath.
class GlHle {
 public:
  explicit GlHle(GlBackend& backend);

  // Registers this object's methods with `hle` and writes the IGL vtable
  // + object header into `memory`. Returns the object address (the value
  // that should end up in gpIGL).
  uint32_t BuildGl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                    uint32_t object_address);

  // Same, for IEGL / gpIEGL.
  uint32_t BuildEgl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                     uint32_t object_address);

 private:
  // EGL lifecycle.
  void EglGetError(IArmCore& core);
  void EglGetDisplay(IArmCore& core);
  void EglInitialize(IArmCore& core);
  void EglQueryString(IArmCore& core);
  void EglTerminate(IArmCore& core);
  void EglChooseConfig(IArmCore& core);
  void EglCreateWindowSurface(IArmCore& core);
  void EglDestroySurface(IArmCore& core);
  void EglCreateContext(IArmCore& core);
  void EglDestroyContext(IArmCore& core);
  void EglMakeCurrent(IArmCore& core);
  void EglSwapBuffers(IArmCore& core);

  // Core GL state / transform.
  void GlClear(IArmCore& core);
  void GlClearColorx(IArmCore& core);
  void GlViewport(IArmCore& core);
  void GlEnable(IArmCore& core);
  void GlDisable(IArmCore& core);
  void GlAlphaFuncx(IArmCore& core);
  void GlBlendFunc(IArmCore& core);
  void GlDepthFunc(IArmCore& core);
  void GlClearDepthx(IArmCore& core);
  void GlDepthMask(IArmCore& core);
  void GlMatrixMode(IArmCore& core);
  void GlLoadIdentity(IArmCore& core);
  void GlPushMatrix(IArmCore& core);
  void GlPopMatrix(IArmCore& core);
  void GlOrthox(IArmCore& core);
  void GlFrustumx(IArmCore& core);
  void GlTranslatex(IArmCore& core);
  void GlRotatex(IArmCore& core);
  void GlScalex(IArmCore& core);
  void GlColor4x(IArmCore& core);

  // Vertex arrays / draw calls.
  void GlVertexPointer(IArmCore& core);
  void GlColorPointer(IArmCore& core);
  void GlTexCoordPointer(IArmCore& core);
  void GlNormalPointer(IArmCore& core);
  void GlEnableClientState(IArmCore& core);
  void GlDisableClientState(IArmCore& core);
  void GlDrawArrays(IArmCore& core);
  void GlDrawElements(IArmCore& core);

  // Texture object management + upload.
  void GlGenTextures(IArmCore& core);
  void GlDeleteTextures(IArmCore& core);
  void GlBindTexture(IArmCore& core);
  void GlTexParameterx(IArmCore& core);
  void GlTexImage2D(IArmCore& core);
  void GlCompressedTexImage2D(IArmCore& core);

  // One array-pointer binding, as set by glVertexPointer/glColorPointer/
  // glTexCoordPointer/glNormalPointer -- kept exactly as declared
  // (emulated address, not resolved) until a draw call actually needs it.
  struct ArrayState {
    bool enabled = false;
    int size = 0;    // components per vertex; normal arrays are always 3
    GLenum type = 0;
    int stride = 0;  // 0 = tightly packed (size * GlTypeSize(type))
    uint32_t pointer = 0;
  };

  // Resolves `indices` (absolute vertex indices, in draw order -- a
  // contiguous range for glDrawArrays, an explicit list for
  // glDrawElements) against the currently-enabled array state into a
  // fully host-native GlVertexArrays.
  GlVertexArrays ExtractArrays(Memory& memory, const std::vector<uint32_t>& indices) const;

  GlBackend& backend_;
  bool context_current_ = false;
  ArrayState vertex_array_;
  ArrayState color_array_;
  ArrayState texcoord_array_;
  ArrayState normal_array_;
};

}  // namespace zeebulator
