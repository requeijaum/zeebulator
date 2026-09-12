#include "frontends/standalone/sdl2_unified_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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

// GL 1.3 (multitextura) e nomes de glPixelStorei. Declarados como literais
// para nao depender do que cada <GL/gl.h> resolve declarar.
constexpr unsigned int kHostTexture0 = 0x84C0;
constexpr unsigned int kHostTextureEnv = 0x2300;
constexpr unsigned int kHostTextureEnvMode = 0x2200;
constexpr unsigned int kHostUnpackAlignment = 0x0CF5;
constexpr unsigned int kHostPackAlignment = 0x0D05;

// Assinaturas de glActiveTexture/glClientActiveTexture declaradas aqui: o
// <SDL_opengl_glext.h> so garante PFNGLACTIVETEXTUREPROC, e a variante de
// cliente aparece apenas como ...ARBPROC em algumas instalacoes. Sao funcoes
// GL padrao de um unico argumento GLenum, entao o typedef proprio e exato.
using HostTextureUnitFn = void(APIENTRY*)(unsigned int);

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

    // One-time GL device report: makes it obvious whether we're on the AMD
    // hardware driver (radeonsi) or fell back to software (llvmpipe). Fetched
    // via SDL_GL_GetProcAddress so we don't need to link libGL directly.
    {
      using GetStringFn = const unsigned char* (*)(unsigned int);
      auto glGetStringFn =
          reinterpret_cast<GetStringFn>(SDL_GL_GetProcAddress("glGetString"));
      if (glGetStringFn) {
        const unsigned int kVendor = 0x1F00, kRenderer = 0x1F01, kVersion = 0x1F02;
        const unsigned char* vend = glGetStringFn(kVendor);
        const unsigned char* rend = glGetStringFn(kRenderer);
        const unsigned char* vers = glGetStringFn(kVersion);
        std::fprintf(stderr, "[gl] driver=%s | renderer=%s | version=%s | sdl_video=%s\n",
                     vend ? reinterpret_cast<const char*>(vend) : "?",
                     rend ? reinterpret_cast<const char*>(rend) : "?",
                     vers ? reinterpret_cast<const char*>(vers) : "?",
                     SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
      }
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
    // Todo upload que passa por este backend chega com as linhas compactadas
    // (GlHle ja aplicou o GL_UNPACK_ALIGNMENT do guest ao ler a memoria
    // emulada), entao o default 4 do host so poderia corromper linhas.
    glPixelStorei(kHostUnpackAlignment, 1);
  }

  SDL_AudioSpec desired{};
  desired.freq = audio_sample_rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  // 1024 frames at the Zeebo mixer rate (22050 Hz) is only 46 ms. A single
  // CPU-heavy frame can exceed that and starve SDL's device queue.
  desired.samples = 4096;

  SDL_AudioSpec obtained{};
  // Aceitar uma taxa diferente da pedida. Com allowed_changes=0 o dispositivo
  // tinha de entregar exatamente 22050 Hz; em qualquer maquina que nao oferecesse
  // essa taxa a abertura falhava e o emulador ficava mudo. Pior: mesmo quando
  // abria, PushAudioSamples descartava em silencio todo bloco cuja taxa nao
  // batesse -- som sumindo sem uma linha de log, indistinguivel de um jogo que
  // nao toca nada. Agora a diferenca de taxa e reamostrada, nao descartada.
  audio_device_ = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &desired, &obtained,
                                       SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (audio_device_ == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
  } else {
    device_sample_rate_ = obtained.freq;
    if (obtained.freq != audio_sample_rate) {
      std::fprintf(stderr,
                   "[audio] dispositivo abriu a %d Hz (pedimos %d Hz); "
                   "reamostrando na saida\n",
                   obtained.freq, audio_sample_rate);
    }
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
  // Multitextura (GL 1.3). Opcional: se o driver nao expuser, guardamos o
  // pedido do jogo e seguimos na unidade 0 -- nunca fingimos que trocamos.
  glActiveTexture_ = SDL_GL_GetProcAddress("glActiveTexture");
  glClientActiveTexture_ = SDL_GL_GetProcAddress("glClientActiveTexture");
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
  // O quad 2D SEMPRE vai para o FBO de apresentacao. Se o alvo offscreen do
  // pbuffer estiver ligado (o jogo esta desenhando o palco 3D), desviar o 2D
  // para la contaminaria o readback do palco com a propria tela -- foi
  // exatamente essa realimentacao que fez o "3D" virar uma copia da tela.
  auto BindFramebufferForPresent =
      reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  const bool restore_pbuffer = pbuffer_bound_ && pbuffer_fbo_ != 0;
  if (BindFramebufferForPresent != nullptr && fbo_ != 0) {
    BindFramebufferForPresent(GL_FRAMEBUFFER, fbo_);
  }

  // O quad 2D usa a unidade de textura 0. Se o jogo deixou outra unidade ativa
  // (glActiveTexture, 4240 chamadas medidas na Z-Wheel), o glBindTexture
  // abaixo iria para a unidade errada e a tela 2D sairia preta.
  SelectHostUnitZero();
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
  RestoreGuestTextureUnits();
  // Devolve o alvo do jogo, para que os proximos comandos GL dele continuem
  // caindo no pbuffer e nao no FBO de apresentacao.
  if (restore_pbuffer && BindFramebufferForPresent != nullptr) {
    BindFramebufferForPresent(GL_FRAMEBUFFER, pbuffer_fbo_);
    glViewport(0, 0, pbuffer_w_, pbuffer_h_);
  }
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

  // Texto do overlay: unidade 0, sem textura. glDisable(GL_TEXTURE_2D) so vale
  // para a unidade ativa, entao trocar para a 0 antes e obrigatorio.
  SelectHostUnitZero();
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

  // Toda a apresentacao abaixo (blit do FBO) e desenho NOSSO na unidade 0.
  SelectHostUnitZero();

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
  // Devolve a unidade de textura que o jogo tinha escolhido.
  RestoreGuestTextureUnits();
}

bool Sdl2UnifiedBackend::BindOffscreenTarget(int width, int height) {
  // Cria (uma vez por tamanho) um FBO proprio com cor RGBA8 + profundidade e o
  // deixa ligado. Enquanto estiver ligado, TODO desenho do jogo cai aqui, nao
  // no FBO de apresentacao -- e o quad 2D nunca encosta neste alvo.
  if (gl_context_ == nullptr || width <= 0 || height <= 0) return false;
  // Chave de bissecao (ZEEB_NO_PBUFFER_FBO=1): desliga o alvo offscreen para
  // comparar A/B contra o comportamento anterior sem recompilar.
  if (std::getenv("ZEEB_NO_PBUFFER_FBO") != nullptr) return false;
  auto GenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(glGenFramebuffers_);
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  auto FramebufferTexture2D =
      reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(glFramebufferTexture2D_);
  auto CheckFramebufferStatus =
      reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(glCheckFramebufferStatus_);
  auto GenRenderbuffers = reinterpret_cast<PFNGLGENRENDERBUFFERSPROC>(glGenRenderbuffers_);
  auto BindRenderbuffer = reinterpret_cast<PFNGLBINDRENDERBUFFERPROC>(glBindRenderbuffer_);
  auto RenderbufferStorage =
      reinterpret_cast<PFNGLRENDERBUFFERSTORAGEPROC>(glRenderbufferStorage_);
  auto FramebufferRenderbuffer =
      reinterpret_cast<PFNGLFRAMEBUFFERRENDERBUFFERPROC>(glFramebufferRenderbuffer_);
  if (GenFramebuffers == nullptr || BindFramebuffer == nullptr ||
      FramebufferTexture2D == nullptr || CheckFramebufferStatus == nullptr ||
      GenRenderbuffers == nullptr || BindRenderbuffer == nullptr ||
      RenderbufferStorage == nullptr || FramebufferRenderbuffer == nullptr) {
    return false;  // sem FBO real nao existe pbuffer honesto; nao fingir
  }
  if (pbuffer_fbo_ == 0 || pbuffer_w_ != width || pbuffer_h_ != height) {
    if (pbuffer_fbo_ != 0) {
      auto DeleteFramebuffers =
          reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(glDeleteFramebuffers_);
      auto DeleteRenderbuffers =
          reinterpret_cast<PFNGLDELETERENDERBUFFERSPROC>(glDeleteRenderbuffers_);
      if (DeleteFramebuffers != nullptr) DeleteFramebuffers(1, &pbuffer_fbo_);
      if (DeleteRenderbuffers != nullptr && pbuffer_depth_ != 0) {
        DeleteRenderbuffers(1, &pbuffer_depth_);
      }
      if (pbuffer_texture_ != 0) glDeleteTextures(1, &pbuffer_texture_);
      pbuffer_fbo_ = pbuffer_texture_ = pbuffer_depth_ = 0;
    }
    GenFramebuffers(1, &pbuffer_fbo_);
    glGenTextures(1, &pbuffer_texture_);
    glBindTexture(GL_TEXTURE_2D, pbuffer_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GenRenderbuffers(1, &pbuffer_depth_);
    BindRenderbuffer(GL_RENDERBUFFER, pbuffer_depth_);
    RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    BindFramebuffer(GL_FRAMEBUFFER, pbuffer_fbo_);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pbuffer_texture_, 0);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, pbuffer_depth_);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      std::fprintf(stderr,
                   "Sdl2UnifiedBackend: FBO de pbuffer %dx%d incompleto -- readback do palco 3D "
                   "fica indisponivel\n",
                   width, height);
      BindFramebuffer(GL_FRAMEBUFFER, fbo_);
      pbuffer_fbo_ = 0;
      return false;
    }
    pbuffer_w_ = width;
    pbuffer_h_ = height;
  }
  BindFramebuffer(GL_FRAMEBUFFER, pbuffer_fbo_);
  glViewport(0, 0, width, height);
  pbuffer_bound_ = true;
  return true;
}

void Sdl2UnifiedBackend::UnbindOffscreenTarget() {
  if (!pbuffer_bound_) return;
  pbuffer_bound_ = false;
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  if (BindFramebuffer != nullptr) BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, width_, height_);
}

bool Sdl2UnifiedBackend::ReadPixelsRgba(int x, int y, int width, int height,
                                        std::vector<uint8_t>& out) {
  // Le um retangulo do FBO onde o guest desenha. glReadPixels devolve as
  // linhas de baixo para cima; invertemos para origem no topo, que e como
  // tanto o IDisplay quanto o BitBlt do guest enxergam a memoria.
  if (width <= 0 || height <= 0) return false;
  const bool from_pbuffer = pbuffer_bound_ && pbuffer_fbo_ != 0;
  const GLuint source_fbo = from_pbuffer ? pbuffer_fbo_ : fbo_;
  const int source_w = from_pbuffer ? pbuffer_w_ : width_;
  const int source_h = from_pbuffer ? pbuffer_h_ : height_;
  if (source_fbo == 0) return false;
  if (x < 0 || y < 0 || x + width > source_w || y + height > source_h) return false;
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  if (BindFramebuffer == nullptr) return false;
  BindFramebuffer(GL_FRAMEBUFFER, source_fbo);
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  out.resize(row_bytes * static_cast<size_t>(height));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
  std::vector<uint8_t> tmp(row_bytes);
  for (int row = 0; row < height / 2; ++row) {
    uint8_t* top = out.data() + static_cast<size_t>(row) * row_bytes;
    uint8_t* bot = out.data() + static_cast<size_t>(height - 1 - row) * row_bytes;
    std::memcpy(tmp.data(), top, row_bytes);
    std::memcpy(top, bot, row_bytes);
    std::memcpy(bot, tmp.data(), row_bytes);
  }
  return true;
}

bool Sdl2UnifiedBackend::CaptureFrameRgba(std::vector<uint8_t>& out, int* out_w, int* out_h) {
  if (out_w) *out_w = width_;
  if (out_h) *out_h = height_;
  if (fbo_ == 0) return false;  // no-FBO path: caller falls back to IDisplay fb
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  const size_t row_bytes = static_cast<size_t>(width_) * 4;
  out.resize(row_bytes * static_cast<size_t>(height_));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
  // glReadPixels returns rows bottom-to-top; flip to top-origin in place.
  std::vector<uint8_t> tmp(row_bytes);
  for (int y = 0; y < height_ / 2; ++y) {
    uint8_t* top = out.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t* bot = out.data() + static_cast<size_t>(height_ - 1 - y) * row_bytes;
    std::memcpy(tmp.data(), top, row_bytes);
    std::memcpy(top, bot, row_bytes);
    std::memcpy(bot, tmp.data(), row_bytes);
  }
  return true;
}

void Sdl2UnifiedBackend::SetWindowScale(int scale) {
  if (scale < 1 || scale > 4) return;
  window_scale_ = scale;
  SDL_SetWindowSize(window_, width_ * scale, height_ * scale);
}

void Sdl2UnifiedBackend::PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                           int sample_rate) {
  if (audio_device_ == 0 || frame_count == 0) return;
  const int out_rate = device_sample_rate_ > 0 ? device_sample_rate_ : audio_sample_rate_;
  auto log_queue = [&] {
    if (std::getenv("ZEEB_LOG_AUDIO_QUEUE") == nullptr) return;
    static uint64_t pushes = 0;
    ++pushes;
    if (pushes == 1 || pushes % 60 == 0) {
      uint32_t bytes = SDL_GetQueuedAudioSize(audio_device_);
      uint32_t frames = bytes / (2 * sizeof(int16_t));
      std::fprintf(stderr, "[audio-queue] pushes=%llu queued=%u frames=%u ms=%u rate=%d\n",
                   static_cast<unsigned long long>(pushes), bytes, frames,
                   out_rate > 0 ? static_cast<unsigned>(frames * 1000 / out_rate) : 0, out_rate);
    }
  };
  if (!audio_prebuffered_) {
    // Establish 8192 frames (~371 ms at the native 22050 Hz) once. Initial
    // asset decode has measured stalls around 170 ms, larger than a device
    // callback alone; this queue headroom absorbs them without pitch changes.
    constexpr size_t kInitialAudioPrebufferFrames = 8192;
    std::vector<int16_t> silence(kInitialAudioPrebufferFrames * 2, 0);
    SDL_QueueAudio(audio_device_, silence.data(),
                   static_cast<uint32_t>(silence.size() * sizeof(int16_t)));
    audio_prebuffered_ = true;
  }
  if (sample_rate == out_rate) {
    SDL_QueueAudio(audio_device_, interleaved_stereo,
                    static_cast<uint32_t>(frame_count * 2 * sizeof(int16_t)));
    log_queue();
    return;
  }
  // Reamostragem linear estereo para a taxa real do dispositivo. Antes deste
  // ramo o bloco era simplesmente descartado quando as taxas divergiam.
  const double ratio = static_cast<double>(sample_rate) / static_cast<double>(out_rate);
  const size_t out_frames = static_cast<size_t>(static_cast<double>(frame_count) / ratio);
  if (out_frames == 0) return;
  audio_resample_buffer_.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; ++i) {
    const double src = static_cast<double>(i) * ratio;
    const size_t f0 = static_cast<size_t>(src);
    const size_t f1 = std::min(f0 + 1, frame_count - 1);
    const double frac = src - static_cast<double>(f0);
    for (int ch = 0; ch < 2; ++ch) {
      const double a = interleaved_stereo[f0 * 2 + ch];
      const double b = interleaved_stereo[f1 * 2 + ch];
      audio_resample_buffer_[i * 2 + ch] = static_cast<int16_t>(a + (b - a) * frac);
    }
  }
  SDL_QueueAudio(audio_device_, audio_resample_buffer_.data(),
                  static_cast<uint32_t>(audio_resample_buffer_.size() * sizeof(int16_t)));
  log_queue();
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
  // So o eglSwapBuffers do proprio jogo chega aqui (GlHle::EglSwapBuffers).
  gl_swap_seen_ = true;
  SDL_GL_MakeCurrent(window_, gl_context_);
  PresentFrame();
}

void Sdl2UnifiedBackend::PresentGlFrameWithoutSwapMark() {
  if (gl_context_ == nullptr) return;
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
void Sdl2UnifiedBackend::LoadMatrix(const float m[16]) { glLoadMatrixf(m); }
void Sdl2UnifiedBackend::MultMatrix(const float m[16]) { glMultMatrixf(m); }
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

// --- Estado fixed-function que a Z-Wheel usa de verdade --------------------
// Cada metodo abaixo existia como Stub silencioso ate o histograma por slot
// da vtable IGL mostrar o jogo chamando todos eles (ver gl_backend.h).

// glTexEnvx(GL_TEXTURE_ENV_MODE, ...) -- 4135 chamadas medidas em ~28 s. Este
// metodo ja existia em GlBackend, mas NINGUEM no caminho do SDL o
// implementava: o host ficava sempre em GL_MODULATE. Um jogo que pede
// GL_REPLACE e recebe MODULATE ve a textura multiplicada pela cor de vertice/
// iluminacao corrente -- se essa cor for escura, a textura vira preta.
void Sdl2UnifiedBackend::TexEnvMode(GLenum mode) {
  glTexEnvi(kHostTextureEnv, kHostTextureEnvMode, static_cast<GLint>(mode));
}

// 213 chamadas medidas. O jogo escolhe QUAL face descartar; ignorar isso
// deixava o host no default GL_BACK, e com a orientacao/matriz do jogo isso
// mostra faces de tras junto com as da frente (sintoma: "a roda tem 2 raios").
void Sdl2UnifiedBackend::CullFace(GLenum mode) { glCullFace(mode); }
// Zero chamadas medidas na Z-Wheel (slot 34 nunca disparou): o jogo fica no
// default GL_CCW. Implementado porque o slot existe.
void Sdl2UnifiedBackend::FrontFace(GLenum mode) { glFrontFace(mode); }
// 319 chamadas. GL_FLAT vs GL_SMOOTH muda a interpolacao de cor por face.
void Sdl2UnifiedBackend::ShadeModel(GLenum mode) { glShadeModel(mode); }

void Sdl2UnifiedBackend::ActiveTexture(GLenum texture) {
  guest_active_texture_ = texture;
  if (glActiveTexture_ == nullptr) return;
  reinterpret_cast<HostTextureUnitFn>(glActiveTexture_)(texture);
}

void Sdl2UnifiedBackend::ClientActiveTexture(GLenum texture) {
  guest_client_active_texture_ = texture;
  if (glClientActiveTexture_ == nullptr) return;
  reinterpret_cast<HostTextureUnitFn>(glClientActiveTexture_)(texture);
}

void Sdl2UnifiedBackend::SelectHostUnitZero() {
  if (glActiveTexture_ != nullptr && guest_active_texture_ != kHostTexture0) {
    reinterpret_cast<HostTextureUnitFn>(glActiveTexture_)(kHostTexture0);
  }
  if (glClientActiveTexture_ != nullptr && guest_client_active_texture_ != kHostTexture0) {
    reinterpret_cast<HostTextureUnitFn>(glClientActiveTexture_)(kHostTexture0);
  }
}

void Sdl2UnifiedBackend::RestoreGuestTextureUnits() {
  if (glActiveTexture_ != nullptr && guest_active_texture_ != kHostTexture0) {
    reinterpret_cast<HostTextureUnitFn>(glActiveTexture_)(guest_active_texture_);
  }
  if (glClientActiveTexture_ != nullptr && guest_client_active_texture_ != kHostTexture0) {
    reinterpret_cast<HostTextureUnitFn>(glClientActiveTexture_)(
        guest_client_active_texture_);
  }
}

// 25 chamadas medidas. O efeito REAL do alinhamento de desempacotamento e
// consumido em GlHle (que anda linha a linha na memoria do guest e entrega os
// pixels ja compactados); aqui guardamos o pedido para poder restaura-lo e
// encaminhamos o lado do EMPACOTAMENTO (glReadPixels), que e do host.
void Sdl2UnifiedBackend::PixelStorei(GLenum pname, GLint param) {
  if (pname == kHostUnpackAlignment) {
    guest_unpack_alignment_ = param;
    return;  // o upload usa 1: ver TexImage2D.
  }
  if (pname == kHostPackAlignment) {
    glPixelStorei(pname, param);
    return;
  }
  glPixelStorei(pname, param);
}

// 848 chamadas de glMaterialxv e 106 de glLightxv medidas, junto com 424
// glNormalPointer: o jogo usa iluminacao de verdade. Sem material/luz o host
// fica no default (material difuso cinza 0.8, luz 0 apagada), e qualquer
// superficie iluminada sai com a cor errada -- no limite, quase preta.
void Sdl2UnifiedBackend::Materialfv(GLenum face, GLenum pname, const float* values, int count) {
  if (values == nullptr || count <= 0) return;
  if (count == 1) {
    glMaterialf(face, pname, values[0]);  // GL_SHININESS
  } else {
    glMaterialfv(face, pname, values);
  }
}

void Sdl2UnifiedBackend::Lightfv(GLenum light, GLenum pname, const float* values, int count) {
  if (values == nullptr || count <= 0) return;
  if (count == 1) {
    glLightf(light, pname, values[0]);  // GL_SPOT_EXPONENT/CUTOFF, atenuacoes
  } else {
    glLightfv(light, pname, values);
  }
}

void Sdl2UnifiedBackend::LightModelfv(GLenum pname, const float* values, int count) {
  if (values == nullptr || count <= 0) return;
  if (count == 1) {
    glLightModelf(pname, values[0]);
  } else {
    glLightModelfv(pname, values);
  }
}

// 212 chamadas de cada. O FBO de apresentacao ainda nao tem anexo de stencil,
// entao o teste em si nao recorta nada; mesmo assim o estado e programado de
// verdade no host (nao e mais engolido) e passa a valer assim que houver
// buffer de stencil.
void Sdl2UnifiedBackend::StencilFunc(GLenum func, GLint ref, GLuint mask) {
  glStencilFunc(func, ref, mask);
}
void Sdl2UnifiedBackend::StencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
  glStencilOp(sfail, dpfail, dppass);
}

// 424 chamadas. So dica de qualidade, mas e instrucao do jogo.
void Sdl2UnifiedBackend::Hint(GLenum target, GLenum mode) { glHint(target, mode); }

// 212 chamadas. O jogo espera bloquear ate o GPU terminar (ele le o color
// buffer logo depois, via eglGetColorBufferQUALCOMM).
void Sdl2UnifiedBackend::Finish() { glFinish(); }

// 51 chamadas. Devolver 0 fixo escondia exatamente os erros de upload de
// textura que procuramos -- agora e o erro REAL do driver.
GLenum Sdl2UnifiedBackend::GetError() { return static_cast<GLenum>(glGetError()); }

void Sdl2UnifiedBackend::DrawArrays(GLenum mode, const GlVertexArrays& arrays) {
  // GlVertexArrays traz um conjunto de coordenadas POR UNIDADE de textura
  // (medido: a Z-Wheel usa GL_TEXTURE0 e GL_TEXTURE1). Os arrays de posicao,
  // cor e normal sao globais; os de coordenada sao por unidade, entao cada um
  // e programado com a sua unidade de cliente selecionada.
  SelectHostUnitZero();
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
  // Unidade 1: so mexe nela se o driver tiver multitextura. Sem isso o
  // array da unidade 1 acabaria programado na unidade 0 (que e justamente o
  // bug que estamos consertando).
  auto ClientActiveTextureFn = reinterpret_cast<HostTextureUnitFn>(glClientActiveTexture_);
  if (ClientActiveTextureFn != nullptr) {
    ClientActiveTextureFn(kHostTexture0 + 1);
    if (arrays.has_texcoord1) {
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(arrays.texcoord1_size, GL_FLOAT, 0, arrays.texcoords1.data());
    } else {
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    ClientActiveTextureFn(kHostTexture0);
  }
  if (arrays.has_normal) {
    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, 0, arrays.normals.data());
  } else {
    glDisableClientState(GL_NORMAL_ARRAY);
  }
  glDrawArrays(mode, 0, arrays.vertex_count);
  RestoreGuestTextureUnits();
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
  GLenum err = glGetError();
  if (err != GL_NO_ERROR && std::getenv("ZEEB_LOG_GPU")) {
    std::fprintf(stderr, "[gl_err_texparam] target=0x%x pname=0x%x param=0x%x err=0x%x\n",
                 target, pname, param, err);
  }
}
void Sdl2UnifiedBackend::TexImage2D(GLenum target, const GlTextureImage& image) {
  // GlHle SEMPRE entrega linhas compactadas (ele mesmo aplica o
  // GL_UNPACK_ALIGNMENT do guest ao ler a memoria emulada), entao o upload
  // tem de usar alinhamento 1. Com o default 4 do host, qualquer textura
  // RGB/565 de largura nao multipla de 4/2 era lida deslocada -- linhas
  // escorregando e canais trocados, que e exatamente o tipo de sintoma
  // relatado ("texturas azuis pretas").
  glPixelStorei(kHostUnpackAlignment, 1);
  glTexImage2D(target, image.level, static_cast<GLint>(image.internal_format), image.width,
               image.height, /*border=*/0, image.format, image.type, image.pixels);
  if (std::getenv("ZEEB_LOG_GPU") != nullptr) {
    GLenum err = static_cast<GLenum>(glGetError());
    std::fprintf(stderr,
                 "[tex_upload] %dx%d internal=0x%x format=0x%x type=0x%x pixels=%s err=0x%x\n",
                 image.width, image.height, image.internal_format, image.format, image.type,
                 image.pixels != nullptr ? "sim" : "nulo", err);
  }
}

void Sdl2UnifiedBackend::TexSubImage2D(GLenum target, const GlTextureSubImage& image) {
  glPixelStorei(kHostUnpackAlignment, 1);  // mesmo motivo de TexImage2D
  glTexSubImage2D(target, image.level, image.xoffset, image.yoffset, image.width,
                  image.height, image.format, image.type, image.pixels);
}

bool Sdl2UnifiedBackend::CaptureScreenshot(const std::string& path) {
  if (gl_context_ == nullptr) return false;
  SDL_GL_MakeCurrent(window_, gl_context_);
  auto BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glBindFramebuffer_);
  if (fbo_ != 0 && BindFramebuffer != nullptr) {
    BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(width_) * height_ * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
  if (fbo_ != 0 && BindFramebuffer != nullptr) {
    BindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width_, height_);
  // Inverte as linhas (OpenGL le de baixo para cima, PPM escreve de cima para baixo)
  for (int y = height_ - 1; y >= 0; --y) {
    std::fwrite(pixels.data() + static_cast<size_t>(y) * width_ * 3, 1, static_cast<size_t>(width_) * 3, f);
  }
  std::fclose(f);
  return true;
}

}  // namespace zeebulator
