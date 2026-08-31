#pragma once

#include <SDL.h>

#include <string>

#include "core/backend.h"
#include "core/brew/gl_backend.h"
#include "frontends/standalone/letterbox.h"

namespace zeebulator {

// Implements both Backend (2D IDisplay video/audio/input) and GlBackend
// (real IGL/IEGL rendering) on top of a single real SDL_Window + single
// real SDL_GLContext, created eagerly at construction and held for the
// object's entire lifetime.
//
// Exists specifically to avoid a real, confirmed desktop compositor bug
// (TASKS.md/PHASE8_LOG.md Phase 8, Double Dragon): a real host GL
// context anywhere in this process, coexisting with a *separate* 2D
// SDL_Renderer-backed presentation path (even a different, hidden
// window's own context), reliably breaks this desktop's real compositor
// (Cinnamon/Muffin) into never repainting the real visible window again.
// A minimal, independent reproduction (same investigation) confirmed a
// *single* real GL context used as the sole presentation mechanism for
// the one real visible window does NOT trigger this bug, no matter how
// much real GL activity (repeated clear/draw/swap) it does. So instead
// of a separate SDL_Renderer + a second/hidden GL context
// (Sdl2Backend + Sdl2GlBackend, still used where only one of the two
// presentation needs applies -- see their own doc comments), this class
// presents the 2D IDisplay framebuffer *through* the same real GL
// context real GLES rendering uses: PushVideoFrame uploads it as a
// texture and draws a full-screen textured quad, then swaps -- the
// exact same real drawable, the exact same real context, every time.
//
// This also matches what real Double Dragon disassembly found
// elsewhere in this investigation: `IBITMAP_QueryInterface(...,
// AEECLSID_DIB, ...)`'s result gets cast straight to `NativeWindowType`
// for `eglCreateWindowSurface` -- i.e. on real hardware, GLES's own
// native window surface *is* IDisplay's own device bitmap/surface, not
// a separate one. Real code drawing 2D IDisplay content and real code
// drawing GLES content are, on real hardware, both just drawing to the
// one real display surface -- exactly what this class does here.
class Sdl2UnifiedBackend : public Backend, public GlBackend {
 public:
  Sdl2UnifiedBackend(SDL_Window* window, int width, int height, int audio_sample_rate);
  ~Sdl2UnifiedBackend() override;

  // Backend
  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override;
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                         int sample_rate) override;
  ZPadState PollInput() override;

  // True once a real SDL_GameController was found at construction --
  // lets a caller choose to poll real controller edges (PollInput's own
  // per-tick contract) only when there's a real controller to poll,
  // rather than also picking up PollInput's own keyboard fallback (a
  // separate, dev-only convenience key scheme -- see PollKeyboard's own
  // doc comment) alongside a caller's own, possibly different, keyboard
  // handling.
  bool HasController() const { return controller_ != nullptr; }

  // True once the real app has called eglSwapBuffers (SwapBuffers()
  // below) at least once. Real Double Dragon disassembly (TASKS.md
  // Phase 8) confirmed the app stops calling IDISPLAY_Update entirely
  // once it starts real GL rendering -- so the frontend's own
  // "RepresentLastFrame" keep-alive mechanism (idisplay.h) has nothing
  // useful left to re-present from that point on, and doing so anyway
  // actively fights the app's own real GL swaps for the same drawable
  // (confirmed on the real desktop: real GL content never became
  // visible until the frontend stopped re-pushing the stale 2D
  // snapshot once this goes true).
  bool HasRealGlActivity() const { return gl_swap_seen_; }

  // Everything (the 2D IDisplay quad, the real app's own GLES draws, the
  // overlay) renders into a fixed `width_`x`height_` offscreen surface --
  // see the FBO members below -- and PresentFrame blits that, letterboxed,
  // onto the real window's own current size every frame. So the real
  // window can be any size (including live-resized by the player) without
  // this class needing to be told about it explicitly; SetWindowScale
  // exists only for the common "pick a whole-number preset" hotkey case,
  // driving a real `SDL_SetWindowSize` to `width_*scale x height_*scale`.
  // Clamped to [1,4]; out-of-range values are ignored.
  void SetWindowScale(int scale);
  int WindowScale() const { return window_scale_; }

  // Shows/hides the whole overlay (FPS counter and any active status
  // message below it). Visible by default, matching this class's
  // previous always-on FPS overlay.
  void SetOverlayVisible(bool visible) { overlay_visible_ = visible; }
  bool OverlayVisible() const { return overlay_visible_; }

  // Shows `text` as a second overlay line for a few real seconds, then it
  // clears itself -- transient feedback for a hotkey action (e.g. "Scale:
  // 2x", "Saved slot 1"). No-op while the overlay itself is hidden (see
  // SetOverlayVisible) other than remembering the text for whenever it's
  // shown again within the same window.
  void ShowStatusMessage(const std::string& text);

  // GlBackend -- CreateContext/DestroyContext are no-ops (true/nothing):
  // this class's one real context is created in the constructor and
  // lives until the destructor, regardless of the app's own real
  // eglMakeCurrent/eglDestroyContext/eglTerminate call pattern, since
  // the 2D video path depends on it too.
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
  ZPadState PollController();
  ZPadState PollKeyboard();

  // Loads the handful of real FBO entry points this class needs via
  // SDL_GL_GetProcAddress (portable across platforms/drivers, unlike
  // linking against them directly -- see sdl2_unified_backend.cpp's own
  // doc comment on why) and creates fbo_/fbo_texture_ at width_ x
  // height_. Returns false (leaving fbo_ == 0) if either step fails, in
  // which case PresentFrame falls back to rendering directly into the
  // real window at its native size -- no window-scaling support, but
  // not a crash -- for whatever ancient/broken driver doesn't have
  // real FBO support (effectively unheard of since GL 3.0/2007).
  bool InitFramebuffer();

  // Common tail of both real presentation paths (PushVideoFrame's 2D
  // quad and the real app's own eglSwapBuffers): draws the overlay (see
  // DrawOverlay) into the still-bound offscreen surface, then blits that
  // whole surface (letterboxed to the real window's own current size)
  // onto the real default framebuffer and does the one real
  // SDL_GL_SwapWindow for this frame, then rebinds the offscreen surface
  // so the next frame's rendering (both this class's own and the real
  // app's) keeps targeting it rather than the real window directly.
  void PresentFrame();
  // Updates the rolling FPS estimate and draws it, plus any active
  // status message (see ShowStatusMessage), as small top-left text
  // (immediate-mode GL quads, matching the save/restore-state pattern
  // PushVideoFrame already uses, so it never corrupts real app-owned GL
  // state) -- no-op entirely while overlay_visible_ is false.
  void DrawOverlay();

  SDL_Window* window_;
  SDL_GLContext gl_context_;
  int width_;   // logical/emulated resolution -- always 640x480, real
  int height_;  // window size never changes what this reports to the guest.

  // Offscreen surface everything (2D IDisplay quad, real app GLES draws,
  // overlay) renders into, fixed at width_ x height_ regardless of the
  // real window's own size -- see SetWindowScale's own doc comment.
  // Left bound for the whole of each frame's rendering; only unbound
  // momentarily inside PresentFrame for the final blit.
  GLuint fbo_ = 0;
  GLuint fbo_texture_ = 0;
  // Real depth renderbuffer attached alongside fbo_texture_ -- without
  // one, real app draws made with GL_DEPTH_TEST enabled (confirmed real,
  // load-bearing behavior -- see the SDL_GL_DEPTH_SIZE request on the
  // real window's own GL context, made for exactly this reason before
  // this class existed) have nothing to test against inside this FBO,
  // which silently breaks real depth-gated compositing (confirmed live:
  // this is what produced a real, visible blue-tinted overlay across the
  // whole image once real rendering moved into this FBO).
  GLuint fbo_depth_renderbuffer_ = 0;
  // Real function pointers, loaded via SDL_GL_GetProcAddress -- stored
  // as opaque `void*` here rather than the real `PFNGL...PROC` typedefs
  // (`SDL_opengl_glext.h`) since those need real system GL types
  // (`::GLenum` et al.) this project's headers deliberately don't pull
  // in (see gl_types.h's own `zeebulator::GLenum` et al., used
  // everywhere else in this header instead) -- cast back to the real
  // typedefs only at each call site in sdl2_unified_backend.cpp, where
  // the real platform GL header is already included first.
  void* glGenFramebuffers_ = nullptr;
  void* glBindFramebuffer_ = nullptr;
  void* glFramebufferTexture2D_ = nullptr;
  void* glCheckFramebufferStatus_ = nullptr;
  void* glDeleteFramebuffers_ = nullptr;
  void* glGenRenderbuffers_ = nullptr;
  void* glBindRenderbuffer_ = nullptr;
  void* glRenderbufferStorage_ = nullptr;
  void* glFramebufferRenderbuffer_ = nullptr;
  void* glDeleteRenderbuffers_ = nullptr;

  int window_scale_ = 1;
  // Created eagerly and unconditionally in the constructor, not lazily
  // on first PushVideoFrame -- see sdl2_unified_backend.cpp's own
  // constructor comment: a lazy real glGenTextures call tied to
  // presentation timing rather than deterministic guest execution can
  // consume a different texture ID slot across separate runs, permanently
  // desyncing GlTextureRecordingBackend's own recorded IDs from a cold
  // relaunch's real ones (see core/gl_texture_log.h).
  GLuint video_texture_ = 0;
  bool gl_swap_seen_ = false;

  bool overlay_visible_ = true;
  std::string status_message_;
  Uint32 status_message_expires_ms_ = 0;

  Uint32 fps_last_tick_ms_ = 0;
  int fps_frame_count_ = 0;
  double fps_display_value_ = 0.0;

  SDL_AudioDeviceID audio_device_ = 0;
  int audio_sample_rate_;
  SDL_GameController* controller_ = nullptr;
};

}  // namespace zeebulator
