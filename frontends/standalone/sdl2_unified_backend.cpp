#include "frontends/standalone/sdl2_unified_backend.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// Real FBO entry points (glGenFramebuffers et al.) aren't declared by
// plain GL 1.1 headers above -- this SDL-bundled Khronos registry header
// supplies the real PFNGL...PROC typedefs (portably across platforms,
// unlike GL/glext.h which isn't guaranteed to exist everywhere) used to
// cast what SDL_GL_GetProcAddress returns. Needs the base GL types the
// headers above just provided, so it has to come after them.
#include <SDL_opengl_glext.h>

namespace zeebulator {

namespace {

// Minimal 3x5 dot-matrix font, just the glyphs the FPS overlay needs.
// Each row's 3 bits are columns left..right (bit2=leftmost).
struct FontGlyph {
  char c;
  uint8_t rows[5];
};

constexpr FontGlyph kFont3x5[] = {
    {'0', {0x7, 0x5, 0x5, 0x5, 0x7}}, {'1', {0x2, 0x6, 0x2, 0x2, 0x7}},
    {'2', {0x7, 0x1, 0x7, 0x4, 0x7}}, {'3', {0x7, 0x1, 0x7, 0x1, 0x7}},
    {'4', {0x5, 0x5, 0x7, 0x1, 0x1}}, {'5', {0x7, 0x4, 0x7, 0x1, 0x7}},
    {'6', {0x7, 0x4, 0x7, 0x5, 0x7}}, {'7', {0x7, 0x1, 0x1, 0x1, 0x1}},
    {'8', {0x7, 0x5, 0x7, 0x5, 0x7}}, {'9', {0x7, 0x5, 0x7, 0x1, 0x7}},
    {'A', {0x2, 0x5, 0x7, 0x5, 0x5}}, {'C', {0x7, 0x4, 0x4, 0x4, 0x7}},
    {'D', {0x6, 0x5, 0x5, 0x5, 0x6}}, {'E', {0x7, 0x4, 0x7, 0x4, 0x7}},
    {'F', {0x7, 0x4, 0x7, 0x4, 0x4}}, {'H', {0x5, 0x5, 0x7, 0x5, 0x5}},
    {'I', {0x7, 0x2, 0x2, 0x2, 0x7}}, {'L', {0x4, 0x4, 0x4, 0x4, 0x7}},
    {'N', {0x5, 0x7, 0x7, 0x7, 0x5}}, {'O', {0x7, 0x5, 0x5, 0x5, 0x7}},
    {'P', {0x7, 0x5, 0x7, 0x4, 0x4}}, {'R', {0x6, 0x5, 0x6, 0x5, 0x5}},
    {'S', {0x7, 0x4, 0x7, 0x1, 0x7}}, {'T', {0x7, 0x2, 0x2, 0x2, 0x2}},
    {'U', {0x5, 0x5, 0x5, 0x5, 0x7}}, {'V', {0x5, 0x5, 0x5, 0x5, 0x2}},
    {'W', {0x5, 0x5, 0x5, 0x7, 0x5}}, {'X', {0x5, 0x5, 0x2, 0x5, 0x5}},
    {':', {0x0, 0x2, 0x0, 0x2, 0x0}}, {' ', {0x0, 0x0, 0x0, 0x0, 0x0}},
};

const FontGlyph* FindGlyph(char c) {
  for (const auto& glyph : kFont3x5) {
    if (glyph.c == c) return &glyph;
  }
  return nullptr;
}

}  // namespace

Sdl2UnifiedBackend::Sdl2UnifiedBackend(SDL_Window* window, int width, int height,
                                        int audio_sample_rate)
    : window_(window),
      gl_context_(SDL_GL_CreateContext(window)),
      width_(width),
      height_(height),
      audio_sample_rate_(audio_sample_rate) {
  if (gl_context_ == nullptr) {
    std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
  } else {
    SDL_GL_MakeCurrent(window_, gl_context_);
    // Explicit real vsync: tried both 0 and 1 directly against the real
    // desktop compositor bug this class exists to avoid (TASKS.md Phase
    // 8) -- 1 measurably reduced (did not fully eliminate) a separate,
    // much smaller residual real compositor quirk (brief, periodic
    // black flashes, unrelated to and far less severe than the
    // permanent blackout the single-context design itself fixes) versus
    // 0 or leaving it at whatever the driver defaults to.
    if (SDL_GL_SetSwapInterval(1) != 0) {
      std::fprintf(stderr, "SDL_GL_SetSwapInterval(1) failed: %s\n", SDL_GetError());
    }

    // Created here, eagerly, rather than lazily on the first
    // PushVideoFrame call as before -- real width/height there are
    // always width_/height_ anyway (the guest's own IDisplay always
    // operates at this fixed logical resolution), so there was never a
    // real reason to wait. This also fixes a real, confirmed-live bug:
    // this raw glGenTextures call happens entirely outside the
    // GlBackend interface, invisible to GlTextureRecordingBackend's own
    // recording (core/gl_texture_log.h, TASKS_TOOLING.md Phase B stage
    // 2) -- a *lazily* created video_texture_ could get allocated at a
    // different relative point across two separate real runs (its
    // creation was tied to real frame-presentation timing, not guest
    // instruction execution), consuming a real GL texture ID at a
    // different moment each time and desyncing every real texture ID a
    // save's replay expects from then on. Unconditional here (not
    // inside InitFramebuffer, which can return early on an old/broken
    // driver -- see its own doc comment) since plain glGenTextures/
    // glTexImage2D need no extension loading and can't fail that way.
    glGenTextures(1, &video_texture_);
    glBindTexture(GL_TEXTURE_2D, video_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, /*border=*/0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  SDL_AudioSpec desired{};
  desired.freq = audio_sample_rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  desired.samples = 1024;

  SDL_AudioSpec obtained{};
  audio_device_ = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &desired, &obtained,
                                       /*allowed_changes=*/0);
  if (audio_device_ == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
  } else {
    SDL_PauseAudioDevice(audio_device_, 0);  // start the device unpaused
  }

  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      controller_ = SDL_GameControllerOpen(i);
      if (controller_ != nullptr) break;
    }
  }

  if (gl_context_ != nullptr) InitFramebuffer();
}

Sdl2UnifiedBackend::~Sdl2UnifiedBackend() {
  if (controller_ != nullptr) SDL_GameControllerClose(controller_);
  if (audio_device_ != 0) SDL_CloseAudioDevice(audio_device_);
  if (gl_context_ != nullptr) {
    if (video_texture_ != 0) glDeleteTextures(1, &video_texture_);
    if (fbo_texture_ != 0) glDeleteTextures(1, &fbo_texture_);
    if (fbo_depth_renderbuffer_ != 0 && glDeleteRenderbuffers_ != nullptr) {
      reinterpret_cast<PFNGLDELETERENDERBUFFERSPROC>(glDeleteRenderbuffers_)(
          1, &fbo_depth_renderbuffer_);
    }
    if (fbo_ != 0 && glDeleteFramebuffers_ != nullptr) {
      reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(glDeleteFramebuffers_)(1, &fbo_);
    }
    SDL_GL_DeleteContext(gl_context_);
  }
}

// See this class's own doc comment on why FBO entry points are loaded
// through SDL_GL_GetProcAddress rather than linked against directly:
// plain GL/gl.h (or OpenGL/gl.h on macOS) only declares GL 1.1, and this
// project doesn't otherwise need a full GL loader (GLEW/GLAD) -- a
// direct link would also just fail to resolve on some platforms/drivers
// entirely, whereas SDL_GL_GetProcAddress is SDL's own portable answer
// to exactly this (works identically on every platform SDL supports).
// The 5 members these get stored into are opaque `void*` (see the
// class's own doc comment on them) -- cast back to the real PFNGL...PROC
// type at each call site below and in PresentFrame.
bool Sdl2UnifiedBackend::InitFramebuffer() {
  glGenFramebuffers_ = SDL_GL_GetProcAddress("glGenFramebuffers");
  glBindFramebuffer_ = SDL_GL_GetProcAddress("glBindFramebuffer");
  glFramebufferTexture2D_ = SDL_GL_GetProcAddress("glFramebufferTexture2D");
  glCheckFramebufferStatus_ = SDL_GL_GetProcAddress("glCheckFramebufferStatus");
  glDeleteFramebuffers_ = SDL_GL_GetProcAddress("glDeleteFramebuffers");
  glGenRenderbuffers_ = SDL_GL_GetProcAddress("glGenRenderbuffers");
  glBindRenderbuffer_ = SDL_GL_GetProcAddress("glBindRenderbuffer");
  glRenderbufferStorage_ = SDL_GL_GetProcAddress("glRenderbufferStorage");
  glFramebufferRenderbuffer_ = SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
  glDeleteRenderbuffers_ = SDL_GL_GetProcAddress("glDeleteRenderbuffers");
  if (glGenFramebuffers_ == nullptr || glBindFramebuffer_ == nullptr ||
      glFramebufferTexture2D_ == nullptr || glCheckFramebufferStatus_ == nullptr ||
      glDeleteFramebuffers_ == nullptr || glGenRenderbuffers_ == nullptr ||
      glBindRenderbuffer_ == nullptr || glRenderbufferStorage_ == nullptr ||
      glFramebufferRenderbuffer_ == nullptr || glDeleteRenderbuffers_ == nullptr) {
    std::fprintf(stderr,
                 "Sdl2UnifiedBackend: real framebuffer-object entry points unavailable -- "
                 "window scaling/resizing disabled\n");
    return false;
  }
  auto GenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(glGenFramebuffers_);
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  auto FramebufferTexture2D =
      reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(glFramebufferTexture2D_);
  auto CheckFramebufferStatus =
      reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(glCheckFramebufferStatus_);
  auto DeleteFramebuffers = reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(glDeleteFramebuffers_);
  auto GenRenderbuffers = reinterpret_cast<PFNGLGENRENDERBUFFERSPROC>(glGenRenderbuffers_);
  auto BindRenderbuffer = reinterpret_cast<PFNGLBINDRENDERBUFFERPROC>(glBindRenderbuffer_);
  auto RenderbufferStorage =
      reinterpret_cast<PFNGLRENDERBUFFERSTORAGEPROC>(glRenderbufferStorage_);
  auto FramebufferRenderbuffer =
      reinterpret_cast<PFNGLFRAMEBUFFERRENDERBUFFERPROC>(glFramebufferRenderbuffer_);
  auto DeleteRenderbuffers =
      reinterpret_cast<PFNGLDELETERENDERBUFFERSPROC>(glDeleteRenderbuffers_);

  glGenTextures(1, &fbo_texture_);
  glBindTexture(GL_TEXTURE_2D, fbo_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, /*border=*/0, GL_RGB, GL_UNSIGNED_BYTE,
               nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);

  // Real depth buffer, matching the real 24-bit one already requested
  // for the window's own default-framebuffer GL context (see this
  // member's own doc comment on why this FBO needs one too).
  GenRenderbuffers(1, &fbo_depth_renderbuffer_);
  BindRenderbuffer(GL_RENDERBUFFER, fbo_depth_renderbuffer_);
  RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
  BindRenderbuffer(GL_RENDERBUFFER, 0);

  GenFramebuffers(1, &fbo_);
  BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_texture_, 0);
  FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                           fbo_depth_renderbuffer_);
  if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr,
                 "Sdl2UnifiedBackend: real framebuffer incomplete -- window scaling/resizing "
                 "disabled\n");
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    DeleteFramebuffers(1, &fbo_);
    DeleteRenderbuffers(1, &fbo_depth_renderbuffer_);
    glDeleteTextures(1, &fbo_texture_);
    fbo_ = 0;
    fbo_texture_ = 0;
    fbo_depth_renderbuffer_ = 0;
    return false;
  }
  // Left bound -- every subsequent real GL call (this class's own 2D
  // quad/overlay and the real app's own GLES draws) targets this
  // offscreen surface until PresentFrame's final blit, see its own doc
  // comment.
  return true;
}

void Sdl2UnifiedBackend::PushVideoFrame(const void* framebuffer, int width, int height,
                                         PixelFormat format) {
  (void)format;  // IDisplayHle's framebuffer is always RGB565 for now.
  if (gl_context_ == nullptr) return;
  SDL_GL_MakeCurrent(window_, gl_context_);

  glBindTexture(GL_TEXTURE_2D, video_texture_);
  // RGB565 maps directly onto GL's own packed GL_UNSIGNED_SHORT_5_6_5
  // format -- no pixel conversion needed. Texel row 0 (the first bytes
  // in `framebuffer`) becomes texture coordinate t=0, matching
  // `framebuffer`'s own row 0 = top of the real IDisplay image, so the
  // quad below can use un-flipped texcoords.
  //
  // Storage was already allocated once, eagerly, in InitFramebuffer (see
  // its own doc comment on why) -- every frame here just updates it in
  // place via glTexSubImage2D rather than reallocating, avoiding a real
  // driver footgun (repeated glTexImage2D calls at the ~30-60Hz this
  // gets pushed at can stall or transiently corrupt a frame on some
  // drivers); glTexSubImage2D is the correct, standard pattern for a
  // streaming texture that never changes size.
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                   framebuffer);

  // Real app GL state (matrices, enables, ...) may currently hold
  // whatever the app itself last set -- save/restore around the quad so
  // presenting the 2D surface never corrupts real app-owned GL state.
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glEnable(GL_TEXTURE_2D);
  glViewport(0, 0, width_, height_);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);  // (0,0) top-left, (1,1) bottom-right

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glBegin(GL_TRIANGLE_FAN);
  glTexCoord2f(0.0f, 0.0f);
  glVertex2f(0.0f, 0.0f);
  glTexCoord2f(1.0f, 0.0f);
  glVertex2f(1.0f, 0.0f);
  glTexCoord2f(1.0f, 1.0f);
  glVertex2f(1.0f, 1.0f);
  glTexCoord2f(0.0f, 1.0f);
  glVertex2f(0.0f, 1.0f);
  glEnd();

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();

  PresentFrame();
}

namespace {

constexpr int kFontPixel = 3;    // real screen pixels per font "pixel"
constexpr int kFontGap = 1;      // real screen pixels between font pixels
constexpr int kFontCharGap = 6;  // extra real screen pixels between glyphs
constexpr int kFontLineHeight = 5 * (kFontPixel + kFontGap) + 6;

// Draws one line of text, top-left origin at (origin_x, origin_y) in
// whatever coordinate space the caller's already set up (real screen
// pixels for the overlay below) -- assumes glBegin/glEnd-style immediate
// drawing is already valid (texturing disabled, color already set).
// Characters with no glyph (see kFont3x5) are silently skipped rather
// than drawn as a placeholder or rejected -- degrades gracefully for any
// status text that ends up including a character this small font simply
// doesn't have, rather than needing every caller to pre-validate its
// own strings against the font's coverage.
void DrawText(int origin_x, int origin_y, const char* text) {
  int pen_x = origin_x;
  for (const char* p = text; *p != '\0'; ++p) {
    const FontGlyph* glyph = FindGlyph(*p);
    if (glyph != nullptr) {
      for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
          if (((glyph->rows[row] >> (2 - col)) & 1) == 0) continue;
          int x = pen_x + col * (kFontPixel + kFontGap);
          int y = origin_y + row * (kFontPixel + kFontGap);
          glBegin(GL_QUADS);
          glVertex2i(x, y);
          glVertex2i(x + kFontPixel, y);
          glVertex2i(x + kFontPixel, y + kFontPixel);
          glVertex2i(x, y + kFontPixel);
          glEnd();
        }
      }
    }
    pen_x += 3 * (kFontPixel + kFontGap) + kFontCharGap;
  }
}

}  // namespace

// Drawn last, into the still-bound offscreen surface (see InitFramebuffer's
// own doc comment), on top of whatever real content (2D quad above or the
// real app's own GLES draws via SwapBuffers) is already there for this
// frame -- same save/restore-state pattern as the PushVideoFrame quad
// above, so it never leaks state back to real app or video-quad rendering
// next frame. Entirely skipped (nothing drawn, including the FPS line)
// while overlay_visible_ is false.
void Sdl2UnifiedBackend::DrawOverlay() {
  if (!overlay_visible_) return;

  char fps_text[16];
  std::snprintf(fps_text, sizeof(fps_text), "FPS:%d", static_cast<int>(fps_display_value_ + 0.5));

  bool show_status =
      !status_message_.empty() && SDL_GetTicks() < status_message_expires_ms_;

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, width_, height_, 0.0, -1.0, 1.0);  // (0,0) top-left, in real screen pixels

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  constexpr int kOriginX = 6;
  constexpr int kOriginY = 6;

  glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
  DrawText(kOriginX, kOriginY, fps_text);

  if (show_status) {
    glColor4f(0.4f, 1.0f, 1.0f, 1.0f);
    DrawText(kOriginX, kOriginY + kFontLineHeight, status_message_.c_str());
  }

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();
}

void Sdl2UnifiedBackend::ShowStatusMessage(const std::string& text) {
  status_message_ = text;
  constexpr Uint32 kStatusMessageDurationMs = 2500;
  status_message_expires_ms_ = SDL_GetTicks() + kStatusMessageDurationMs;
}

// Real display refresh rate is capped well below 1000Hz, so a 500ms
// window is both frequent-enough to feel live and long-enough to
// average out real single-frame jitter.
void Sdl2UnifiedBackend::PresentFrame() {
  ++fps_frame_count_;
  Uint32 now = SDL_GetTicks();
  if (fps_last_tick_ms_ == 0) {
    fps_last_tick_ms_ = now;
  } else if (now - fps_last_tick_ms_ >= 500) {
    fps_display_value_ = fps_frame_count_ * 1000.0 / (now - fps_last_tick_ms_);
    fps_frame_count_ = 0;
    fps_last_tick_ms_ = now;
  }
  DrawOverlay();

  if (fbo_ == 0) {
    // See InitFramebuffer's own doc comment: no real FBO support, so
    // everything above was already rendered directly into the real
    // window at its native size -- nothing left to blit.
    SDL_GL_SwapWindow(window_);
    return;
  }

  // Blit the offscreen surface (2D quad/real app draws/overlay, all
  // still at the fixed width_ x height_ it's always rendered at) onto
  // the real window, letterboxed to whatever size that window actually
  // is right now -- see letterbox.h. This is the one point in a frame
  // where the real default framebuffer is bound instead of fbo_.
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  BindFramebuffer(GL_FRAMEBUFFER, 0);

  int drawable_width = 0;
  int drawable_height = 0;
  SDL_GL_GetDrawableSize(window_, &drawable_width, &drawable_height);
  ViewportRect rect = ComputeLetterboxedViewport(drawable_width, drawable_height, width_, height_);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(rect.x, rect.y, rect.width, rect.height);

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, fbo_texture_);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  // Unlike the video texture in PushVideoFrame (loaded directly from
  // pixel data via glTexImage2D, row 0 = top -- see its own doc
  // comment), fbo_texture_ was filled by rendering *into* it, which
  // fills it in OpenGL's standard bottom-left-origin texture convention
  // (v=0 = bottom of what was drawn). So unlike that other quad, this
  // one flips v (not u) to compensate -- texcoord v=0 pairs with the
  // *bottom* screen vertex here, not the top.
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glBegin(GL_TRIANGLE_FAN);
  glTexCoord2f(0.0f, 1.0f);
  glVertex2f(0.0f, 0.0f);
  glTexCoord2f(1.0f, 1.0f);
  glVertex2f(1.0f, 0.0f);
  glTexCoord2f(1.0f, 0.0f);
  glVertex2f(1.0f, 1.0f);
  glTexCoord2f(0.0f, 0.0f);
  glVertex2f(0.0f, 1.0f);
  glEnd();

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();

  SDL_GL_SwapWindow(window_);

  // Rebind for the next frame's rendering (this class's own and the
  // real app's) -- see InitFramebuffer's own doc comment.
  BindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void Sdl2UnifiedBackend::SetWindowScale(int scale) {
  if (scale < 1 || scale > 4) return;
  window_scale_ = scale;
  SDL_SetWindowSize(window_, width_ * scale, height_ * scale);
}

void Sdl2UnifiedBackend::PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                           int sample_rate) {
  if (audio_device_ == 0 || sample_rate != audio_sample_rate_) return;
  SDL_QueueAudio(audio_device_, interleaved_stereo,
                  static_cast<uint32_t>(frame_count * 2 * sizeof(int16_t)));
}

// Standard SDL_GameController button naming *should* already match an
// Xbox-layout controller's own physical layout (A=bottom, B=right,
// X=left, Y=top) -- that was this mapping's original assumption, unverified
// against real hardware. Live-tested against a real Xbox Wireless
// Controller (Bluetooth) on this project's real dev desktop and found
// A/X genuinely swapped: pressing the physical West (X) button fired
// SDL_CONTROLLER_BUTTON_A, and physical South (A) fired
// SDL_CONTROLLER_BUTTON_X -- a known real quirk of some Xbox Wireless
// Controller firmware/Bluetooth HID report layouts not matching SDL's
// built-in gamecontrollerdb entry for this device. B/Y were not
// reported as affected, so only A/X are swapped here to compensate.
ZPadState Sdl2UnifiedBackend::PollController() {
  ZPadState state;
  auto Set = [&](SDL_GameControllerButton button, uint16_t mask) {
    if (SDL_GameControllerGetButton(controller_, button)) state.buttons |= mask;
  };
  Set(SDL_CONTROLLER_BUTTON_DPAD_UP, ZPadState::kDpadUp);
  Set(SDL_CONTROLLER_BUTTON_DPAD_DOWN, ZPadState::kDpadDown);
  Set(SDL_CONTROLLER_BUTTON_DPAD_LEFT, ZPadState::kDpadLeft);
  Set(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ZPadState::kDpadRight);
  Set(SDL_CONTROLLER_BUTTON_START, ZPadState::kStartHome);
  Set(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ZPadState::kShoulderL);
  Set(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ZPadState::kShoulderR);
  Set(SDL_CONTROLLER_BUTTON_A, ZPadState::kButtonWest);
  Set(SDL_CONTROLLER_BUTTON_X, ZPadState::kButtonSouth);
  Set(SDL_CONTROLLER_BUTTON_Y, ZPadState::kButtonNorth);
  Set(SDL_CONTROLLER_BUTTON_B, ZPadState::kButtonEast);

  // SDL's axis range (-32768..32767) already matches ZPadState's
  // int16_t sticks directly -- no rescaling needed.
  state.left_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
  state.left_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
  state.right_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
  state.right_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
  return state;
}

// Fallback default when no real gamepad is connected: arrow keys for
// the D-pad, Z/X/A/S for the four face buttons (a common non-gamepad
// convention, e.g. RetroArch's own default keyboard binds), Q/E for the
// shoulder buttons, Return for Start/Home. Not derived from any real
// Zeebo device (there's no physical keyboard on one) -- purely this
// frontend's own dev/debug convenience, unlike the controller mapping
// above.
ZPadState Sdl2UnifiedBackend::PollKeyboard() {
  ZPadState state;
  const uint8_t* keys = SDL_GetKeyboardState(nullptr);
  auto Set = [&](SDL_Scancode key, uint16_t mask) {
    if (keys[key]) state.buttons |= mask;
  };
  Set(SDL_SCANCODE_UP, ZPadState::kDpadUp);
  Set(SDL_SCANCODE_DOWN, ZPadState::kDpadDown);
  Set(SDL_SCANCODE_LEFT, ZPadState::kDpadLeft);
  Set(SDL_SCANCODE_RIGHT, ZPadState::kDpadRight);
  Set(SDL_SCANCODE_RETURN, ZPadState::kStartHome);
  Set(SDL_SCANCODE_Q, ZPadState::kShoulderL);
  Set(SDL_SCANCODE_E, ZPadState::kShoulderR);
  Set(SDL_SCANCODE_A, ZPadState::kButtonWest);
  Set(SDL_SCANCODE_Z, ZPadState::kButtonSouth);
  Set(SDL_SCANCODE_S, ZPadState::kButtonNorth);
  Set(SDL_SCANCODE_X, ZPadState::kButtonEast);
  return state;
}

ZPadState Sdl2UnifiedBackend::PollInput() {
  return controller_ != nullptr ? PollController() : PollKeyboard();
}

bool Sdl2UnifiedBackend::CreateContext() { return gl_context_ != nullptr; }
void Sdl2UnifiedBackend::DestroyContext() {}

void Sdl2UnifiedBackend::SwapBuffers() {
  if (gl_context_ == nullptr) return;
  gl_swap_seen_ = true;
  SDL_GL_MakeCurrent(window_, gl_context_);
  PresentFrame();
}

void Sdl2UnifiedBackend::Clear(GLbitfield mask) { glClear(mask); }
void Sdl2UnifiedBackend::ClearColor(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
}
void Sdl2UnifiedBackend::Viewport(int x, int y, int width, int height) {
  glViewport(x, y, width, height);
}
void Sdl2UnifiedBackend::Enable(GLenum cap) { glEnable(cap); }
void Sdl2UnifiedBackend::Disable(GLenum cap) { glDisable(cap); }
void Sdl2UnifiedBackend::MatrixMode(GLenum mode) { glMatrixMode(mode); }
void Sdl2UnifiedBackend::LoadIdentity() { glLoadIdentity(); }
void Sdl2UnifiedBackend::PushMatrix() { glPushMatrix(); }
void Sdl2UnifiedBackend::PopMatrix() { glPopMatrix(); }
void Sdl2UnifiedBackend::Ortho(float left, float right, float bottom, float top,
                                float near_plane, float far_plane) {
  glOrtho(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2UnifiedBackend::Frustum(float left, float right, float bottom, float top,
                                  float near_plane, float far_plane) {
  glFrustum(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2UnifiedBackend::Translate(float x, float y, float z) { glTranslatef(x, y, z); }
void Sdl2UnifiedBackend::Rotate(float angle_degrees, float x, float y, float z) {
  glRotatef(angle_degrees, x, y, z);
}
void Sdl2UnifiedBackend::Scale(float x, float y, float z) { glScalef(x, y, z); }
void Sdl2UnifiedBackend::Color4(float r, float g, float b, float a) { glColor4f(r, g, b, a); }
void Sdl2UnifiedBackend::AlphaFunc(GLenum func, float ref) { glAlphaFunc(func, ref); }
void Sdl2UnifiedBackend::BlendFunc(GLenum sfactor, GLenum dfactor) { glBlendFunc(sfactor, dfactor); }
void Sdl2UnifiedBackend::DepthFunc(GLenum func) { glDepthFunc(func); }
void Sdl2UnifiedBackend::ClearDepth(float depth) { glClearDepth(static_cast<GLdouble>(depth)); }
void Sdl2UnifiedBackend::DepthMask(bool flag) { glDepthMask(flag ? GL_TRUE : GL_FALSE); }

void Sdl2UnifiedBackend::DrawArrays(GLenum mode, const GlVertexArrays& arrays) {
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

void Sdl2UnifiedBackend::GenTextures(GLsizei n, GLuint* textures) { glGenTextures(n, textures); }
void Sdl2UnifiedBackend::DeleteTextures(GLsizei n, const GLuint* textures) {
  glDeleteTextures(n, textures);
}
void Sdl2UnifiedBackend::BindTexture(GLenum target, GLuint texture) {
  glBindTexture(target, texture);
}
void Sdl2UnifiedBackend::TexParameter(GLenum target, GLenum pname, GLint param) {
  glTexParameteri(target, pname, param);
}
void Sdl2UnifiedBackend::TexImage2D(GLenum target, const GlTextureImage& image) {
  glTexImage2D(target, image.level, static_cast<GLint>(image.internal_format), image.width,
               image.height, /*border=*/0, image.format, image.type, image.pixels);
}

}  // namespace zeebulator
