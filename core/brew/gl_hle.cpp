#include "core/brew/gl_hle.h"

#include "core/control/debug_sink.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "core/brew/interface_object.h"
#include "core/loader/atitc.h"
#include "core/loader/obm1.h"
#include "core/brew/draw_stats.h"

namespace zeebulator {

namespace {

// Env-gated GPU/GLES trace (ZEEB_LOG_GPU=1). Off by default.
void GpuLog(const char* fmt, ...) {
  static const bool on = std::getenv("ZEEB_LOG_GPU") != nullptr;
  char buf[512];
  std::va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ::zeebulator::DebugLog(::zeebulator::DebugCat::kGpu, buf);
  if (!on) return;
  std::fprintf(stderr, "[gpu] %s\n", buf);
}

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

// EGL sentinel handles: this class never talks to any real host EGL
// implementation (GlBackend owns the one real GL context), so these just
// need to be stable non-zero values the app can pass back into later
// calls (eglMakeCurrent, eglDestroySurface, ...) without us caring what
// they "mean".
constexpr EGLHandle kDisplayHandle = 1;
constexpr EGLHandle kConfigHandle = 1;
constexpr EGLHandle kSurfaceHandle = 1;
constexpr EGLHandle kContextHandle = 1;

// EGL_SUCCESS, per the standard Khronos EGL error-code namespace (a
// stable industry constant, not Qualcomm-specific text).
constexpr EGLint kEglSuccess = 0x3000;

void WriteEGLintIfNonNull(Memory& memory, uint32_t addr, EGLint value) {
  if (addr != 0) {
    memory.Write32(addr, static_cast<uint32_t>(value));
  }
}

// Scratch space for eglQueryString's return value -- real callers only
// ever read the string immediately after the call (confirmed via real
// disassembly of Double Dragon, see PHASE8_LOG.md: fed straight into a
// strstr-shaped call), so a single reused buffer is enough.
constexpr uint32_t kQueryStringBufferAddr = 0x8001B000;

void WriteCString(Memory& memory, uint32_t addr, const char* text) {
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    memory.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory.Write8(addr + static_cast<uint32_t>(i), 0);
}

}  // namespace

GlHle::GlHle(GlBackend& backend) : backend_(backend) {}

// --- EGL lifecycle -----------------------------------------------------
// None of these slots receive the interface pointer in R0 (see gl_hle.h)
// -- R0 holds the first *declared* argument.

void GlHle::EglQueryInterface(IArmCore& core) {
  // int QueryInterface(IEGL *pMe, AEECLSID iid, void **ppo)
  // R0 is this, R1 is iid, R2 is ppo
  constexpr uint32_t kAeeIidGles10 = 0x0103d8dd;
  constexpr uint32_t kAeeIidGles11 = 0x0103d8ea;
  constexpr uint32_t kAeeIidEgl10 = 0x0103d8ed;
  constexpr uint32_t kAeeIidEgl11 = 0x0103d8ee;
  constexpr uint32_t kAeeIidEglSurfaceManipV1 = 0x010434cc;
  constexpr uint32_t kAeeIidEglSurfaceManip = 0x01051834;
  constexpr uint32_t kAeeIidGlesImageonExtV1 = 0x010459b1;
  constexpr uint32_t kAeeIidGlesImageonExt = 0x01058546;

  uint32_t iid = core.GetRegister(kR1);
  uint32_t ppo = core.GetRegister(kR2);

  uint32_t ret_obj = 0;
  if (iid == kAeeIidGles10 || iid == kAeeIidGles11) {
    ret_obj = gles11_object_ != 0 ? gles11_object_ : gl_object_;
  } else if (iid == kAeeIidEgl10 || iid == kAeeIidEgl11) {
    ret_obj = egl_object_ != 0 ? egl_object_ : core.GetRegister(kR0);
  } else if (iid == kAeeIidEglSurfaceManip || iid == kAeeIidEglSurfaceManipV1) {
    ret_obj = surface_manip_obj_;
  } else if (iid == kAeeIidGlesImageonExt || iid == kAeeIidGlesImageonExtV1) {
    // Can alias surface manip or return surface manip if not distinguished
    ret_obj = surface_manip_obj_;
  }

  if (ppo != 0) {
    core.GetMemory().Write32(ppo, ret_obj);
  }

  if (ret_obj != 0) {
    core.SetRegister(kR0, 0); // SUCCESS
  } else {
    core.SetRegister(kR0, 1); // ECLASSNOTSUPPORT
  }
}

void GlHle::EglGetError(IArmCore& core) { core.SetRegister(kR0, static_cast<uint32_t>(kEglSuccess)); }

void GlHle::EglGetDisplay(IArmCore& core) {
  // EGLDisplay eglGetDisplay(NativeDisplayType display) -- the real arg
  // (R0) is ignored; we always hand back the one simulated display.
  core.SetRegister(kR0, kDisplayHandle);
}

void GlHle::EglInitialize(IArmCore& core) {
  // EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
  WriteEGLintIfNonNull(core.GetMemory(), core.GetRegister(kR1), 1);
  WriteEGLintIfNonNull(core.GetMemory(), core.GetRegister(kR2), 0);
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglQueryString(IArmCore& core) {
  // const char *eglQueryString(EGLDisplay dpy, EGLint name) -- R0 is dpy
  // (ignored), R1 is name. Real EGL 1.x values (confirmed against the
  // real BREW OpenGL ES extension SDK headers, extracted from
  // research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
  // OpenGLES_Extension_...zip -- see PHASE8_LOG.md).
  constexpr EGLint kEglVendor = 0x3053;
  constexpr EGLint kEglVersion = 0x3054;
  constexpr EGLint kEglExtensions = 0x3055;
  constexpr EGLint kEglClientApis = 0x308D;
  auto name = static_cast<EGLint>(core.GetRegister(kR1));
  const char* value = "";
  if (name == kEglVendor) {
    value = "Zeebulator";
  } else if (name == kEglVersion) {
    value = "1.1";
  } else if (name == kEglExtensions) {
    // Extensoes EGL da plataforma Zeebo (Qualcomm Adreno 130 + EGL do BREW).
    // Os titulos consultam por SUBSTRING, com espaco como delimitador -- e
    // assim que a agulha aparece no binario deles.
    //
    // EGL_QUALCOMM_surface_scale: escalonamento de superficie; ports de
    // arcade e titulos 3D checam para habilitar o caminho escalado.
    //
    // EGL_QUALCOMM_COLOR_BUFFER: exigida pelo Double Dragon. Encontrada por
    // medicao, nao por palpite: ZEEB_HLE_PROFILE apontou o helper slot 58
    // (`stristr`, offset 0xe8) devolvendo 0 uma unica vez em
    // ddragonz.mod:0x11d858, com a agulha em 0x0014fcc4 =
    // "EGL_QUALCOMM_COLOR_BUFFER" e o palheiro vindo do slot 7 do IEGL
    // (`eglQueryString`) via 0x123ee4. Devolvendo string vazia para essa
    // extensao, o `beq` em 0x11d860 pulava o bloco 0x11d864-0x11d8ac, que e
    // exatamente onde o jogo cria seu alvo de render (slot 13 + QueryInterface
    // sobre o objeto criado). Sem alvo de render o jogo seguia chamando
    // IDISPLAY_Update todo quadro sobre um framebuffer vazio -- a tela preta.
    // Lista medida, nao adivinhada: `strings` sobre os 62 .mod do corpus
    // No-Intro mostra 25-26 titulos carregando esta MESMA tabela de nomes --
    // e a tabela do SDK/engine descrevendo o que a plataforma anuncia. Os
    // jogos consultam por SUBSTRING com espaco como delimitador.
    // EGL_QUALCOMM_COLOR_BUFFER e distinta de EGL_QUALCOMM_get_color_buffer
    // (o "get_" no meio quebra a substring) e e a que o Double Dragon exige.
    // Ajustavel por ZEEB_EGL_EXTENSIONS para A/B sem recompilar.
    static const char* const kDefaultEglExtensions =
        "EGL_QUALCOMM_surface_scale EGL_QUALCOMM_get_color_buffer "
        "EGL_QUALCOMM_COLOR_BUFFER EGL_EXT_swap_control "
        "EGL_QUALCOMM_surface_transparency EGL_QUALCOMM_surface_rotate "
        "EGL_QUALCOMM_surface_overlay EGL_QUALCOMM_surface_color_key "
        "EGL_QUALCOMM_get_power_level ";
    const char* env = std::getenv("ZEEB_EGL_EXTENSIONS");
    value = (env != nullptr) ? env : kDefaultEglExtensions;
  } else if (name == kEglClientApis) {
    value = "OpenGL_ES";
  }
  WriteCString(core.GetMemory(), kQueryStringBufferAddr, value);
  core.SetRegister(kR0, kQueryStringBufferAddr);
}

void GlHle::EglTerminate(IArmCore& core) {
  if (context_current_) {
    backend_.DestroyContext();
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglChooseConfig(IArmCore& core) {
  // EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
  //                            EGLConfig *configs, EGLint config_size,
  //                            EGLint *num_config)
  uint32_t configs = core.GetRegister(kR2);
  auto config_size = static_cast<int32_t>(core.GetRegister(kR3));
  uint32_t num_config = HleRuntime::ReadStackArg(core, 0);
  if (configs != 0 && config_size >= 1) {
    core.GetMemory().Write32(configs, kConfigHandle);
  }
  WriteEGLintIfNonNull(core.GetMemory(), num_config, 1);
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglCreateWindowSurface(IArmCore& core) {
  core.SetRegister(kR0, kSurfaceHandle);
}

void GlHle::EglDestroySurface(IArmCore& core) { core.SetRegister(kR0, kEglTrue); }

// Shared EGL config/surface attribute table (mirrors zeebx gles::config_attrib;
// used as an RE oracle, no code copied). 640x480 is the console screen.
namespace {
constexpr int32_t kScreenW = 640;
constexpr int32_t kScreenH = 480;
bool EglConfigAttrib(EGLint attribute, int32_t* out) {
  switch (attribute) {
    case 0x3020: *out = 16; return true;       // EGL_BUFFER_SIZE
    case 0x3024: case 0x3022: *out = 5; return true;  // EGL_RED_SIZE / EGL_BLUE_SIZE
    case 0x3023: *out = 6; return true;        // EGL_GREEN_SIZE
    case 0x3021: *out = 0; return true;        // EGL_ALPHA_SIZE
    case 0x3025: *out = 16; return true;       // EGL_DEPTH_SIZE
    case 0x3026: *out = 8; return true;        // EGL_STENCIL_SIZE
    case 0x3027: case 0x302B: case 0x3034: *out = 0x3038; return true;  // CAVEAT/VISUAL_TYPE/TRANSPARENT_TYPE = EGL_NONE
    case 0x3028: *out = 1; return true;        // EGL_CONFIG_ID
    case 0x302C: *out = kScreenW; return true; // EGL_MAX_PBUFFER_WIDTH
    case 0x302A: *out = kScreenH; return true; // EGL_MAX_PBUFFER_HEIGHT
    case 0x302D: *out = kScreenW * kScreenH; return true;  // EGL_MAX_PBUFFER_PIXELS
    case 0x302E: *out = 1; return true;        // EGL_NATIVE_RENDERABLE = EGL_TRUE
    case 0x3033: *out = 0x0007; return true;   // EGL_SURFACE_TYPE = all bits
    case 0x303F: *out = 0x308E; return true;   // EGL_COLOR_BUFFER_TYPE = EGL_RGB_BUFFER
    case 0x3040: *out = 0x0001; return true;   // EGL_RENDERABLE_TYPE = EGL_OPENGL_ES_BIT
    case 0x303B: case 0x303C: *out = 1; return true;  // MIN/MAX_SWAP_INTERVAL
    case 0x3029: case 0x302F: case 0x3031: case 0x3032:  // LEVEL/VISUAL_ID/SAMPLES/SAMPLE_BUFFERS
    case 0x3037: case 0x3038: case 0x3039:     // TRANSPARENT_{RED,GREEN,BLUE}
    case 0x303A: case 0x303D: *out = 0; return true;  // BIND_TO_TEXTURE_{RGB,RGBA}
    default: return false;
  }
}
}  // namespace

void GlHle::EglGetConfigAttrib(IArmCore& core) {
  // EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
  //                               EGLint attribute, EGLint *value)
  EGLint attribute = static_cast<EGLint>(core.GetRegister(kR2));
  uint32_t value = core.GetRegister(kR3);
  int32_t out = 0;
  if (EglConfigAttrib(attribute, &out)) {
    WriteEGLintIfNonNull(core.GetMemory(), value, out);
    core.SetRegister(kR0, kEglTrue);
  } else {
    core.SetRegister(kR0, kEglFalse);
  }
}

void GlHle::EglQuerySurface(IArmCore& core) {
  // EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
  //                            EGLint attribute, EGLint *value)
  // Games read EGL_WIDTH/EGL_HEIGHT here to size their projection -- a blind
  // Stub left it unwritten, so the guest built a degenerate projection and
  // never issued the real matrix setup (ironsight went black).
  constexpr EGLint kEglHeight = 0x3056;
  constexpr EGLint kEglWidth = 0x3057;
  EGLint attribute = static_cast<EGLint>(core.GetRegister(kR2));
  uint32_t value = core.GetRegister(kR3);
  int32_t out = 0;
  bool ok = true;
  if (attribute == kEglWidth) {
    out = kScreenW;
  } else if (attribute == kEglHeight) {
    out = kScreenH;
  } else {
    ok = EglConfigAttrib(attribute, &out);
  }
  if (ok) {
    WriteEGLintIfNonNull(core.GetMemory(), value, out);
    core.SetRegister(kR0, kEglTrue);
  } else {
    core.SetRegister(kR0, kEglFalse);
  }
}

void GlHle::EglCreateContext(IArmCore& core) { core.SetRegister(kR0, kContextHandle); }

void GlHle::EglDestroyContext(IArmCore& core) {
  if (context_current_) {
    backend_.DestroyContext();
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglMakeCurrent(IArmCore& core) {
  // EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
  //                           EGLSurface read, EGLContext ctx)
  uint32_t ctx = core.GetRegister(kR3);
  if (ctx != 0 && !context_current_) {
    context_current_ = backend_.CreateContext();
  } else if (ctx == 0) {
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglGetColorBufferQualcomm(IArmCore& core) {
  // Forma ainda nao confirmada -- medir pelo uso. Registra os quatro
  // registradores de argumento AAPCS e dois da pilha; o chamador real dira
  // quantos parametros existem de fato (ver a tecnica de reconstituicao de
  // ABI em zeebo-lle/notes/MORE_INFO.md 5.4).
  std::fprintf(stderr,
               "[eglColorBuf] r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x "
               "sp0=0x%08x sp1=0x%08x lr=0x%08x\n",
               core.GetRegister(kR0), core.GetRegister(kR1), core.GetRegister(kR2),
               core.GetRegister(kR3), HleRuntime::ReadStackArg(core, 0),
               HleRuntime::ReadStackArg(core, 1), core.GetRegister(kLR));
  core.SetRegister(kR0, 0);
}

void GlHle::EglGetProcAddress(IArmCore& core) {
  // void (*eglGetProcAddress(const char *procname))()
  // R0 is procname (const char*).
  uint32_t name_ptr = core.GetRegister(kR0);
  if (name_ptr == 0) {
    core.SetRegister(kR0, 0);
    return;
  }
  std::string name;
  for (uint32_t off = 0; off < 128; ++off) {
    uint8_t c = core.GetMemory().Read8(name_ptr + off);
    if (c == 0) break;
    name.push_back(static_cast<char>(c));
  }
  auto it = proc_addresses_.find(name);
  if (it != proc_addresses_.end()) {
    core.SetRegister(kR0, it->second);
    return;
  }
  // If not explicitly registered, return 0 (standard EGL behavior).
  core.SetRegister(kR0, 0);
}

void GlHle::EglSwapBuffers(IArmCore& core) {
  ++DrawStats::Instance().gl_swap;
  backend_.SwapBuffers();
  static uint32_t frame = 0;
  GpuLog("SwapBuffers frame=%u", ++frame);
  core.SetRegister(kR0, kEglTrue);
}

// --- Core GL state / transform ------------------------------------------

void GlHle::GlClear(IArmCore& core) {
  GpuLog("Clear mask=0x%x", core.GetRegister(kR0));
  backend_.Clear(core.GetRegister(kR0));
}

void GlHle::GlClearColorx(IArmCore& core) {
  backend_.ClearColor(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

void GlHle::GlViewport(IArmCore& core) {
  GpuLog("Viewport x=%d y=%d w=%d h=%d", core.GetRegister(kR0), core.GetRegister(kR1),
         core.GetRegister(kR2), core.GetRegister(kR3));
  backend_.Viewport(static_cast<int>(core.GetRegister(kR0)),
                     static_cast<int>(core.GetRegister(kR1)),
                     static_cast<int>(core.GetRegister(kR2)),
                     static_cast<int>(core.GetRegister(kR3)));
}

void GlHle::GlEnable(IArmCore& core) { backend_.Enable(core.GetRegister(kR0)); }
void GlHle::GlDisable(IArmCore& core) { backend_.Disable(core.GetRegister(kR0)); }

void GlHle::GlAlphaFuncx(IArmCore& core) {
  // void glAlphaFuncx(GLenum func, GLclampx ref) -- real disassembly
  // (TASKS.md/PHASE8_LOG.md Phase 8) confirms Double Dragon calls this
  // with (GL_NOTEQUAL, 0.0) before drawing real OBM1 sprites, pairing
  // it with GL_ALPHA_TEST/GL_BLEND to discard the real magenta color-
  // key pixels this project's OBM1 decoder outputs as alpha=0.
  backend_.AlphaFunc(core.GetRegister(kR0), FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))));
}

void GlHle::GlBlendFunc(IArmCore& core) {
  // void glBlendFunc(GLenum sfactor, GLenum dfactor) -- confirmed real
  // args (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), the standard real
  // GLES1.x sprite-alpha-blending pair (see GlAlphaFuncx's own comment).
  backend_.BlendFunc(core.GetRegister(kR0), core.GetRegister(kR1));
}

void GlHle::GlDepthFunc(IArmCore& core) { backend_.DepthFunc(core.GetRegister(kR0)); }

void GlHle::GlClearDepthx(IArmCore& core) {
  // void glClearDepthx(GLclampx depth)
  backend_.ClearDepth(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))));
}

void GlHle::GlDepthMask(IArmCore& core) {
  // void glDepthMask(GLboolean flag)
  backend_.DepthMask(core.GetRegister(kR0) != 0);
}

void GlHle::GlMatrixMode(IArmCore& core) { backend_.MatrixMode(core.GetRegister(kR0)); }
void GlHle::GlLoadIdentity(IArmCore&) { backend_.LoadIdentity(); }
void GlHle::GlLoadMatrixx(IArmCore& core) {
  // void glLoadMatrixx(const GLfixed *m) -- 16 GLfixed column-major em r0.
  uint32_t ptr = core.GetRegister(kR0);
  float m[16];
  for (int i = 0; i < 16; ++i)
    m[i] = FixedToFloat(static_cast<GLfixed>(core.GetMemory().Read32(ptr + i * 4u)));
  backend_.LoadMatrix(m);
}
void GlHle::GlMultMatrixx(IArmCore& core) {
  uint32_t ptr = core.GetRegister(kR0);
  float m[16];
  for (int i = 0; i < 16; ++i)
    m[i] = FixedToFloat(static_cast<GLfixed>(core.GetMemory().Read32(ptr + i * 4u)));
  backend_.MultMatrix(m);
}
void GlHle::GlPushMatrix(IArmCore&) { backend_.PushMatrix(); }
void GlHle::GlPopMatrix(IArmCore&) { backend_.PopMatrix(); }

void GlHle::GlOrthox(IArmCore& core) {
  backend_.Ortho(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))),
                  FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 0))),
                  FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 1))));
}

void GlHle::GlFrustumx(IArmCore& core) {
  backend_.Frustum(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))),
                    FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 0))),
                    FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 1))));
}

void GlHle::GlTranslatex(IArmCore& core) {
  backend_.Translate(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                      FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                      FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))));
}

void GlHle::GlRotatex(IArmCore& core) {
  backend_.Rotate(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

void GlHle::GlScalex(IArmCore& core) {
  backend_.Scale(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))));
}

void GlHle::GlColor4x(IArmCore& core) {
  backend_.Color4(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

// glTexEnvx(target, pname, param): so GL_TEXTURE_ENV_MODE (0x2200) importa para
// o pipeline fixed-function. param chega como enum cru (nao 16.16). A variante
// vetorial glTexEnvxv passa um ponteiro em R2 -> ler a primeira palavra.
void GlHle::GlTexEnvx(IArmCore& core) {
  if (core.GetRegister(kR1) == 0x2200) {
    backend_.TexEnvMode(static_cast<GLenum>(core.GetRegister(kR2)));
  }
}

void GlHle::GlTexEnvxv(IArmCore& core) {
  if (core.GetRegister(kR1) == 0x2200) {
    backend_.TexEnvMode(static_cast<GLenum>(core.GetMemory().Read32(core.GetRegister(kR2))));
  }
}

void GlHle::GlDrawTexxOES(IArmCore& core) {
  // void glDrawTexxOES(GLfixed x, GLfixed y, GLfixed z, GLfixed width, GLfixed height)
  // Arguments: R0=x, R1=y, R2=z, R3=width, SP+0=height
  // When drawing 2D textures, we can trigger draw or update frame
  core.SetRegister(kR0, 0);
}

// --- Vertex arrays / draw calls -------------------------------------------

void GlHle::GlVertexPointer(IArmCore& core) {
  // void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  vertex_array_.size = static_cast<int>(core.GetRegister(kR0));
  vertex_array_.type = core.GetRegister(kR1);
  vertex_array_.stride = static_cast<int>(core.GetRegister(kR2));
  vertex_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlColorPointer(IArmCore& core) {
  // void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  color_array_.size = static_cast<int>(core.GetRegister(kR0));
  color_array_.type = core.GetRegister(kR1);
  color_array_.stride = static_cast<int>(core.GetRegister(kR2));
  color_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlTexCoordPointer(IArmCore& core) {
  // void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  texcoord_array_.size = static_cast<int>(core.GetRegister(kR0));
  texcoord_array_.type = core.GetRegister(kR1);
  texcoord_array_.stride = static_cast<int>(core.GetRegister(kR2));
  texcoord_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlNormalPointer(IArmCore& core) {
  // void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer)
  // -- no size argument, a normal is always 3 components.
  normal_array_.size = 3;
  normal_array_.type = core.GetRegister(kR0);
  normal_array_.stride = static_cast<int>(core.GetRegister(kR1));
  normal_array_.pointer = core.GetRegister(kR2);
}

void GlHle::GlEnableClientState(IArmCore& core) {
  // void glEnableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = true; break;
    case kGlColorArray: color_array_.enabled = true; break;
    case kGlTextureCoordArray: texcoord_array_.enabled = true; break;
    case kGlNormalArray: normal_array_.enabled = true; break;
    default: break;
  }
}

void GlHle::GlDisableClientState(IArmCore& core) {
  // void glDisableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = false; break;
    case kGlColorArray: color_array_.enabled = false; break;
    case kGlTextureCoordArray: texcoord_array_.enabled = false; break;
    case kGlNormalArray: normal_array_.enabled = false; break;
    default: break;
  }
}

GlVertexArrays GlHle::ExtractArrays(Memory& memory,
                                     const std::vector<uint32_t>& indices) const {
  GlVertexArrays out;
  out.vertex_count = static_cast<int>(indices.size());

  auto extract = [&](const ArrayState& array, std::vector<float>& dest, bool normalize_ubyte) {
    int component_bytes = GlTypeSize(array.type);
    int stride = array.stride != 0 ? array.stride : array.size * component_bytes;
    dest.reserve(dest.size() + indices.size() * static_cast<size_t>(array.size));
    for (uint32_t index : indices) {
      uint32_t base = array.pointer + index * static_cast<uint32_t>(stride);
      for (int c = 0; c < array.size; ++c) {
        float value = ReadGlComponent(memory, base + static_cast<uint32_t>(c * component_bytes),
                                       array.type);
        if (normalize_ubyte && array.type == kGlUnsignedByte) {
          value /= 255.0f;
        }
        dest.push_back(value);
      }
    }
  };

  if (vertex_array_.enabled) {
    out.has_position = true;
    out.position_size = vertex_array_.size;
    extract(vertex_array_, out.positions, false);
  }
  if (color_array_.enabled) {
    out.has_color = true;
    extract(color_array_, out.colors, true);
  }
  if (texcoord_array_.enabled) {
    out.has_texcoord = true;
    out.texcoord_size = texcoord_array_.size;
    extract(texcoord_array_, out.texcoords, false);
  }
  if (normal_array_.enabled) {
    out.has_normal = true;
    extract(normal_array_, out.normals, false);
  }
  return out;
}

void GlHle::GlDrawArrays(IArmCore& core) {
  // void glDrawArrays(GLenum mode, GLint first, GLsizei count)
  GLenum mode = core.GetRegister(kR0);
  auto first = static_cast<int32_t>(core.GetRegister(kR1));
  auto count = static_cast<int32_t>(core.GetRegister(kR2));

  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count > 0 ? count : 0));
  for (int32_t i = 0; i < count; ++i) {
    indices.push_back(static_cast<uint32_t>(first + i));
  }
  ++DrawStats::Instance().gl_draw_arrays;
  backend_.DrawArrays(mode, ExtractArrays(core.GetMemory(), indices));
  GpuLog("DrawArrays mode=0x%x first=%d count=%d", mode, first, count);
}

void GlHle::GlDrawElements(IArmCore& core) {
  // void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
  GLenum mode = core.GetRegister(kR0);
  auto count = static_cast<int32_t>(core.GetRegister(kR1));
  GLenum type = core.GetRegister(kR2);
  uint32_t indices_ptr = core.GetRegister(kR3);

  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count > 0 ? count : 0));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < count; ++i) {
    uint32_t index = (type == kGlUnsignedShort)
                          ? memory.Read16(indices_ptr + static_cast<uint32_t>(i) * 2)
                          : memory.Read8(indices_ptr + static_cast<uint32_t>(i));
    indices.push_back(index);
  }
  ++DrawStats::Instance().gl_draw_arrays;
  backend_.DrawArrays(mode, ExtractArrays(memory, indices));
  GpuLog("DrawElements mode=0x%x count=%d type=0x%x", mode, count, type);
}

// --- Texture object management + upload -----------------------------------

void GlHle::GlGenTextures(IArmCore& core) {
  // void glGenTextures(GLsizei n, GLuint *textures)
  auto n = static_cast<int32_t>(core.GetRegister(kR0));
  uint32_t textures_ptr = core.GetRegister(kR1);
  if (n <= 0) return;

  std::vector<GLuint> textures(static_cast<size_t>(n), 0);
  backend_.GenTextures(n, textures.data());
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < n; ++i) {
    memory.Write32(textures_ptr + static_cast<uint32_t>(i) * 4, textures[static_cast<size_t>(i)]);
  }
}

void GlHle::GlGetString(IArmCore& core) {
  // const GLubyte *glGetString(GLenum name) -- R0 is name.
  // Values mirror the BREW/AMR GLES 1.1 driver as reverse-engineered from
  // the zeebx reference: several arcade titles (ironsight included) probe
  // GL_EXTENSIONS for GL_OES_draw_texture and abort graphics init if it is
  // absent -- an empty stub here makes them skip the matrix setup entirely.
  constexpr GLenum kGlVendor = 0x1F00;
  constexpr GLenum kGlRenderer = 0x1F01;
  constexpr GLenum kGlVersion = 0x1F02;
  constexpr GLenum kGlExtensions = 0x1F03;
  const char* value = "";
  switch (core.GetRegister(kR0)) {
    case kGlVendor: value = "Zeebulator"; break;
    case kGlRenderer: value = "Zeebulator Software Rasterizer"; break;
    case kGlVersion: value = "OpenGL ES-CM 1.1"; break;
    case kGlExtensions: {
      // Mesma origem medida da lista EGL acima (varredura de `strings` no
      // corpus). Ajustavel por ZEEB_GL_EXTENSIONS para A/B.
      static const char* const kDefaultGlExtensions =
          "GL_OES_draw_texture GL_ATI_imageon_misc "
          "GL_QUALCOMM_vertex_buffer_object GL_OES_vertex_buffer_object "
          "GL_ARB_vertex_buffer_object GL_OES_query_matrix "
          "GL_OES_point_size_array GL_OES_blend_subtract "
          "GL_OES_blend_func_separate GL_OES_blend_equation_separate "
          "GL_EXT_blend_minmax GL_EXT_blend_func_separate "
          "GL_EXT_blend_equation_separate ";
      const char* genv = std::getenv("ZEEB_GL_EXTENSIONS");
      value = (genv != nullptr) ? genv : kDefaultGlExtensions;
      break;
    }
    default: break;
  }
  WriteCString(core.GetMemory(), kQueryStringBufferAddr, value);
  core.SetRegister(kR0, kQueryStringBufferAddr);
}

void GlHle::GlGetIntegerv(IArmCore& core) {
  // void glGetIntegerv(GLenum pname, GLint *params) -- R0 pname, R1 params.
  // Constants mirror the zeebx gles::integer() table.
  uint32_t params = core.GetRegister(kR1);
  if (params == 0) return;
  Memory& memory = core.GetMemory();
  auto write1 = [&](int32_t v) { memory.Write32(params, static_cast<uint32_t>(v)); };
  switch (core.GetRegister(kR0)) {
    case 0x0D33: write1(1024); break;              // GL_MAX_TEXTURE_SIZE
    case 0x84E2: write1(1); break;                 // GL_MAX_TEXTURE_UNITS
    case 0x0D31: write1(8); break;                 // GL_MAX_LIGHTS
    case 0x0D36:                                   // GL_MAX_MODELVIEW_STACK_DEPTH
    case 0x0D38:                                   // GL_MAX_PROJECTION_STACK_DEPTH
    case 0x0D39: write1(16); break;                // GL_MAX_TEXTURE_STACK_DEPTH
    case 0x0D3A:                                   // GL_MAX_VIEWPORT_DIMS (2 values)
      memory.Write32(params, 640);
      memory.Write32(params + 4, 480);
      break;
    case 0x0D50: write1(4); break;                 // GL_SUBPIXEL_BITS
    case 0x0D52:                                   // GL_RED_BITS
    case 0x0D54: write1(5); break;                 // GL_BLUE_BITS
    case 0x0D53: write1(6); break;                 // GL_GREEN_BITS
    case 0x0D55:                                   // GL_ALPHA_BITS
    case 0x86A2: write1(0); break;                 // GL_NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x0D56: write1(16); break;                // GL_DEPTH_BITS
    case 0x0D57: write1(8); break;                 // GL_STENCIL_BITS
    case 0x80E8:                                   // GL_MAX_ELEMENTS_VERTICES
    case 0x80E9: write1(65535); break;             // GL_MAX_ELEMENTS_INDICES
    default: write1(0); break;
  }
}

void GlHle::GlDeleteTextures(IArmCore& core) {
  // void glDeleteTextures(GLsizei n, const GLuint *textures)
  auto n = static_cast<int32_t>(core.GetRegister(kR0));
  uint32_t textures_ptr = core.GetRegister(kR1);
  if (n <= 0) return;

  std::vector<GLuint> textures(static_cast<size_t>(n));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < n; ++i) {
    textures[static_cast<size_t>(i)] = memory.Read32(textures_ptr + static_cast<uint32_t>(i) * 4);
  }
  backend_.DeleteTextures(n, textures.data());
}

void GlHle::GlBindTexture(IArmCore& core) {
  backend_.BindTexture(core.GetRegister(kR0), core.GetRegister(kR1));
}

void GlHle::GlTexParameterx(IArmCore& core) {
  // void glTexParameterx(GLenum target, GLenum pname, GLfixed param) --
  // param is the raw enum integer, not a true fixed-point value (see
  // GlBackend::TexParameter's comment).
  backend_.TexParameter(core.GetRegister(kR0), core.GetRegister(kR1),
                         static_cast<GLint>(core.GetRegister(kR2)));
}

void GlHle::GlTexImage2D(IArmCore& core) {
  // void glTexImage2D(GLenum target, GLint level, GLint internalformat,
  //                    GLsizei width, GLsizei height, GLint border,
  //                    GLenum format, GLenum type, const GLvoid *pixels)
  GLenum target = core.GetRegister(kR0);
  GlTextureImage image;
  image.level = static_cast<int>(core.GetRegister(kR1));
  image.internal_format = core.GetRegister(kR2);
  image.width = static_cast<int>(core.GetRegister(kR3));
  image.height = static_cast<int>(HleRuntime::ReadStackArg(core, 0));
  // border (stack arg 1) is always 0 per the GLES1.x spec -- not
  // forwarded, GlBackend has nothing meaningful to do with it.
  image.format = HleRuntime::ReadStackArg(core, 2);
  image.type = HleRuntime::ReadStackArg(core, 3);
  uint32_t pixels_ptr = HleRuntime::ReadStackArg(core, 4);

  std::vector<uint8_t> pixel_bytes;
  if (pixels_ptr != 0 && image.width > 0 && image.height > 0) {
    size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                    static_cast<size_t>(GlPixelSize(image.format, image.type));
    pixel_bytes.resize(total);
    Memory& memory = core.GetMemory();
    for (size_t i = 0; i < total; ++i) {
      pixel_bytes[i] = memory.Read8(pixels_ptr + static_cast<uint32_t>(i));
    }
    image.pixels = pixel_bytes.data();
  }
  ++DrawStats::Instance().gl_tex_image;
  backend_.TexImage2D(target, image);
}

void GlHle::GlCompressedTexImage2D(IArmCore& core) {
  // void glCompressedTexImage2D(GLenum target, GLint level,
  //                              GLenum internalformat, GLsizei width,
  //                              GLsizei height, GLint border,
  //                              GLsizei imageSize, const GLvoid *data)
  GLenum target = core.GetRegister(kR0);
  int level = static_cast<int>(core.GetRegister(kR1));
  GLenum internal_format = core.GetRegister(kR2);
  int width = static_cast<int>(core.GetRegister(kR3));
  int height = static_cast<int>(HleRuntime::ReadStackArg(core, 0));
  // border (stack arg 1) unused, same as GlTexImage2D.
  uint32_t image_size = HleRuntime::ReadStackArg(core, 2);
  uint32_t data_ptr = HleRuntime::ReadStackArg(core, 3);
  Memory& memory = core.GetMemory();

  // Real disassembly (TASKS.md/PHASE8_LOG.md Phase 8) found Double
  // Dragon's own `data.ggz` contains *only* real OBM1 images (this
  // project's own already-working core/loader/obm1.h format), never
  // real ATITC -- and that real code reuses this same real GL vtable
  // slot to upload them, passing a real OBM1 header's own width/height
  // (extracted after decompression -- see the `unknown_0xdc_fn` fix in
  // mod_runtime.cpp) as this call's width/height, `internalformat` as
  // an internal engine tag (not a real GL enum), and `data` pointing 8
  // bytes past a real OBM1 header ("OI" magic, flag, bpp) that
  // precedes it. Checking those real magic bytes directly (rather than
  // trusting `internalformat`, which is real-but-not-standard here) is
  // how this project's own bundled evidence says to identify a real
  // OBM1 upload -- confirmed byte-for-byte against every real field
  // (magic, flag, bpp, width, height, and even the declared
  // `imageSize` matching the real palette+pixel-data size exactly) in
  // every real call observed this round.
  if (data_ptr >= 8 && memory.Read8(data_ptr - 8) == 'O' && memory.Read8(data_ptr - 7) == 'I') {
    uint32_t total_size = 8 + image_size;
    std::vector<uint8_t> obm1_bytes(total_size);
    for (uint32_t i = 0; i < total_size; ++i) {
      obm1_bytes[i] = memory.Read8(data_ptr - 8 + i);
    }
    DecodedImage decoded_obm1;
    try {
      decoded_obm1 = Obm1Image::Decode(obm1_bytes);
    } catch (const std::exception&) {
      return;  // malformed -- leave the texture object as-is, not a guess
    }
    // Real transparency (TASKS.md/PHASE8_LOG.md Phase 8): interleave
    // the decoder's real alpha channel (palette index 0 -> 0, confirmed
    // as Double Dragon's real magenta color-key) into RGBA -- real code
    // pairs GL_ALPHA_TEST/GL_BLEND with these uploads (see
    // GlAlphaFuncx's own comment), which need a real alpha channel to
    // act on; a plain RGB upload leaves every sprite's color-key border
    // fully opaque.
    size_t pixel_count = decoded_obm1.rgb.size() / 3;
    std::vector<uint8_t> rgba(pixel_count * 4);
    for (size_t i = 0; i < pixel_count; ++i) {
      rgba[i * 4 + 0] = decoded_obm1.rgb[i * 3 + 0];
      rgba[i * 4 + 1] = decoded_obm1.rgb[i * 3 + 1];
      rgba[i * 4 + 2] = decoded_obm1.rgb[i * 3 + 2];
      rgba[i * 4 + 3] = decoded_obm1.alpha[i];
    }
    GlTextureImage image;
    image.level = level;
    image.internal_format = kGlRgba;
    image.width = static_cast<int>(decoded_obm1.width);
    image.height = static_cast<int>(decoded_obm1.height);
    image.format = kGlRgba;
    image.type = kGlUnsignedByte;
    image.pixels = rgba.data();
    backend_.TexImage2D(target, image);
    return;
  }

  AtitcFormat format;
  if (internal_format == kGlCompressedRgbAtiTc) {
    format = AtitcFormat::kRgb;
  } else if (internal_format == kGlCompressedRgbaAtiTc) {
    format = AtitcFormat::kRgba;
  } else {
    // Real other compressed formats (ETC1, PVRTC, ...) aren't
    // implemented -- no evidence any real target game uses them (see
    // this class's own doc comment). Leave the texture object as-is
    // rather than guess.
    return;
  }

  // Defensive bound: without a real OBM1 magic match above, this
  // project has no confirmed-real example of this call site ever
  // carrying genuine ATITC data (see this function's own doc comment
  // above) -- reject implausible dimensions rather than risk a many-
  // hundred-MB decode/allocation on a still-misidentified resource.
  constexpr int kMaxPlausibleTextureDimension = 4096;
  if (data_ptr == 0 || width <= 0 || height <= 0 || width > kMaxPlausibleTextureDimension ||
      height > kMaxPlausibleTextureDimension) {
    return;
  }
  std::vector<uint8_t> compressed(image_size);
  for (uint32_t i = 0; i < image_size; ++i) {
    compressed[i] = memory.Read8(data_ptr + i);
  }

  auto decoded = DecodeAtitc(compressed.data(), compressed.size(), width, height, format);
  if (!decoded.has_value()) return;

  GlTextureImage image;
  image.level = level;
  image.internal_format = kGlRgba;
  image.width = width;
  image.height = height;
  image.format = kGlRgba;
  image.type = kGlUnsignedByte;
  image.pixels = decoded->data();
  backend_.TexImage2D(target, image);
}

// --- Vtable construction -------------------------------------------------

uint32_t GlHle::BuildGl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                         uint32_t object_address) {
  // Slot order verified directly against the real AEEGL.h (extracted from
  // a genuine Qualcomm "OpenGL ES Extension for BREW SDK 4.x" installer --
  // see TASKS.md Phase 5). 80 slots total: AddRef/Release/QueryInterface,
  // then the 77 gl* methods in AEEGL.h's exact declared order.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                       // 0  AddRef
      Stub,                                       // 1  Release
      Stub,                                       // 2  QueryInterface
      Stub,                                       // 3  glActiveTexture
      [this](IArmCore& c) { GlAlphaFuncx(c); },     // 4  glAlphaFuncx
      [this](IArmCore& c) { GlBindTexture(c); },  // 5  glBindTexture
      [this](IArmCore& c) { GlBlendFunc(c); },      // 6  glBlendFunc
      [this](IArmCore& c) { GlClear(c); },        // 7  glClear
      [this](IArmCore& c) { GlClearColorx(c); },  // 8  glClearColorx
      [this](IArmCore& c) { GlClearDepthx(c); },  // 9  glClearDepthx
      Stub,                                       // 10 glClearStencil
      Stub,                                       // 11 glClientActiveTexture
      [this](IArmCore& c) { GlColor4x(c); },      // 12 glColor4x
      Stub,                                       // 13 glColorMask
      [this](IArmCore& c) { GlColorPointer(c); }, // 14 glColorPointer
      [this](IArmCore& c) { GlCompressedTexImage2D(c); },  // 15 glCompressedTexImage2D
      Stub,                                       // 16 glCompressedTexSubImage2D
      Stub,                                       // 17 glCopyTexImage2D
      Stub,                                       // 18 glCopyTexSubImage2D
      Stub,                                       // 19 glCullFace
      [this](IArmCore& c) { GlDeleteTextures(c); }, // 20 glDeleteTextures
      [this](IArmCore& c) { GlDepthFunc(c); },    // 21 glDepthFunc
      [this](IArmCore& c) { GlDepthMask(c); },    // 22 glDepthMask
      Stub,                                       // 23 glDepthRangex
      [this](IArmCore& c) { GlDisable(c); },      // 24 glDisable
      [this](IArmCore& c) { GlDisableClientState(c); },  // 25 glDisableClientState
      [this](IArmCore& c) { GlDrawArrays(c); },   // 26 glDrawArrays
      [this](IArmCore& c) { GlDrawElements(c); }, // 27 glDrawElements
      [this](IArmCore& c) { GlEnable(c); },       // 28 glEnable
      [this](IArmCore& c) { GlEnableClientState(c); },   // 29 glEnableClientState
      Stub,                                       // 30 glFinish
      Stub,                                       // 31 glFlush
      Stub,                                       // 32 glFogx
      Stub,                                       // 33 glFogxv
      Stub,                                       // 34 glFrontFace
      [this](IArmCore& c) { GlFrustumx(c); },     // 35 glFrustumx
      [this](IArmCore& c) { GlGenTextures(c); },  // 36 glGenTextures
      Stub,                                       // 37 glGetError
      [this](IArmCore& c) { GlGetIntegerv(c); },  // 38 glGetIntegerv
      [this](IArmCore& c) { GlGetString(c); },    // 39 glGetString
      Stub,                                       // 40 glHint
      Stub,                                       // 41 glLightModelx
      Stub,                                       // 42 glLightModelxv
      Stub,                                       // 43 glLightx
      Stub,                                       // 44 glLightxv
      Stub,                                       // 45 glLineWidthx
      [this](IArmCore& c) { GlLoadIdentity(c); }, // 46 glLoadIdentity
      [this](IArmCore& c) { GlLoadMatrixx(c); },  // 47 glLoadMatrixx
      Stub,                                       // 48 glLogicOp
      Stub,                                       // 49 glMaterialx
      Stub,                                       // 50 glMaterialxv
      [this](IArmCore& c) { GlMatrixMode(c); },   // 51 glMatrixMode
      [this](IArmCore& c) { GlMultMatrixx(c); },  // 52 glMultMatrixx
      Stub,                                       // 53 glMultiTexCoord4x
      Stub,                                       // 54 glNormal3x
      [this](IArmCore& c) { GlNormalPointer(c); }, // 55 glNormalPointer
      [this](IArmCore& c) { GlOrthox(c); },       // 56 glOrthox
      Stub,                                       // 57 glPixelStorei
      Stub,                                       // 58 glPointSizex
      Stub,                                       // 59 glPolygonOffsetx
      [this](IArmCore& c) { GlPopMatrix(c); },    // 60 glPopMatrix
      [this](IArmCore& c) { GlPushMatrix(c); },   // 61 glPushMatrix
      Stub,                                       // 62 glReadPixels
      [this](IArmCore& c) { GlRotatex(c); },      // 63 glRotatex
      Stub,                                       // 64 glSampleCoveragex
      [this](IArmCore& c) { GlScalex(c); },       // 65 glScalex
      Stub,                                       // 66 glScissor
      Stub,                                       // 67 glShadeModel
      Stub,                                       // 68 glStencilFunc
      Stub,                                       // 69 glStencilMask
      Stub,                                       // 70 glStencilOp
      [this](IArmCore& c) { GlTexCoordPointer(c); }, // 71 glTexCoordPointer
      [this](IArmCore& c) { GlTexEnvx(c); },       // 72 glTexEnvx
      [this](IArmCore& c) { GlTexEnvxv(c); },      // 73 glTexEnvxv
      [this](IArmCore& c) { GlTexImage2D(c); },   // 74 glTexImage2D
      [this](IArmCore& c) { GlTexParameterx(c); }, // 75 glTexParameterx
      Stub,                                       // 76 glTexSubImage2D
      [this](IArmCore& c) { GlTranslatex(c); },   // 77 glTranslatex
      [this](IArmCore& c) { GlVertexPointer(c); }, // 78 glVertexPointer
      [this](IArmCore& c) { GlViewport(c); },     // 79 glViewport
  };
  gl_vtable_addr_ = vtable_address;
  gl_object_ = object_address;
  // Funcoes de extensao alcancaveis so por eglGetProcAddress (nao tem slot
  // proprio na vtable). Medido: ddragonz.mod:0x11d864 pede
  // "eglGetColorBufferQUALCOMM"; devolver 0 fazia o jogo desistir do GL.
  RegisterProcAddress("eglGetColorBufferQUALCOMM",
                      hle.Register([this](IArmCore& c) { EglGetColorBufferQualcomm(c); }));
  // Pre-populate proc_addresses_ with traps for extension lookups
  for (size_t i = 3; i < methods.size(); ++i) {
    // If the method is not a stub, it will have a trap registered
  }
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

uint32_t GlHle::BuildEgl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                          uint32_t object_address) {
  // 28 slots total: AddRef/Release/QueryInterface, then the 25 egl*
  // methods in AEEGL.h's exact declared order.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                                // 0  AddRef
      Stub,                                                // 1  Release
      [this](IArmCore& c) { EglQueryInterface(c); },         // 2  QueryInterface
      [this](IArmCore& c) { EglGetError(c); },              // 3  eglGetError
      [this](IArmCore& c) { EglGetDisplay(c); },             // 4  eglGetDisplay
      [this](IArmCore& c) { EglInitialize(c); },             // 5  eglInitialize
      [this](IArmCore& c) { EglTerminate(c); },              // 6  eglTerminate
      [this](IArmCore& c) { EglQueryString(c); },            // 7  eglQueryString
      [this](IArmCore& c) { EglGetProcAddress(c); },         // 8  eglGetProcAddress
      Stub,                                                // 9  eglGetConfigs
      [this](IArmCore& c) { EglChooseConfig(c); },           // 10 eglChooseConfig
      [this](IArmCore& c) { EglGetConfigAttrib(c); },        // 11 eglGetConfigAttrib
      [this](IArmCore& c) { EglCreateWindowSurface(c); },    // 12 eglCreateWindowSurface
      Stub,                                                // 13 eglCreatePixmapSurface
      Stub,                                                // 14 eglCreatePbufferSurface
      [this](IArmCore& c) { EglDestroySurface(c); },         // 15 eglDestroySurface
      [this](IArmCore& c) { EglQuerySurface(c); },           // 16 eglQuerySurface
      [this](IArmCore& c) { EglCreateContext(c); },          // 17 eglCreateContext
      [this](IArmCore& c) { EglDestroyContext(c); },         // 18 eglDestroyContext
      [this](IArmCore& c) { EglMakeCurrent(c); },            // 19 eglMakeCurrent
      Stub,                                                // 20 eglGetCurrentContext
      Stub,                                                // 21 eglGetCurrentSurface
      Stub,                                                // 22 eglGetCurrentDisplay
      Stub,                                                // 23 eglQueryContext
      Stub,                                                // 24 eglWaitGL
      Stub,                                                // 25 eglWaitNative
      [this](IArmCore& c) { EglSwapBuffers(c); },            // 26 eglSwapBuffers
      Stub,                                                // 27 eglCopyBuffers
  };
  egl_object_ = object_address;
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

uint32_t GlHle::BuildSurfaceManip(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                  uint32_t object_address) {
  // SurfaceScale:
  // int SetSurfaceScale(pMe, dpy, surf, AEEEGLSurfaceScaleRect *src, *dst, AEEEGLBoolean *ret)
  auto set_surface_scale = [](IArmCore& c) {
    uint32_t ret_ptr = c.GetRegister(kR4); // slot 4 / 5th arg
    if (ret_ptr != 0) {
      c.GetMemory().Write32(ret_ptr, kEglTrue);
    }
    c.SetRegister(kR0, 0); // SUCCESS
  };

  auto get_surface_scale_caps = [](IArmCore& c) {
    uint32_t caps = c.GetRegister(kR3);
    if (caps != 0) {
      // 12 fields (factors 16.16): MinX 1<<16, MaxX 8<<16, MinY 1<<16, MaxY 8<<16, MinW 1, MaxW 640, MinH 1, MaxH 480...
      uint32_t fields[12] = {
        1 << 16, 8 << 16, 1 << 16, 8 << 16,
        1, 640, 1, 480,
        1, 640, 1, 480
      };
      for (int i = 0; i < 12; ++i) {
        c.GetMemory().Write32(caps + i * 4, fields[i]);
      }
    }
    uint32_t ret_ptr = c.GetRegister(kR4);
    if (ret_ptr != 0) {
      c.GetMemory().Write32(ret_ptr, kEglTrue);
    }
    c.SetRegister(kR0, 0);
  };

  auto stub_ret_true = [](IArmCore& c) {
    // Write true to ret ptr if provided (typically R4)
    uint32_t ret_ptr = c.GetRegister(kR4);
    if (ret_ptr != 0) {
      c.GetMemory().Write32(ret_ptr, kEglTrue);
    }
    c.SetRegister(kR0, 0);
  };

  // EGL_SURFACE_MANIP has ~26 slots
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                   // 0 AddRef
      Stub,                   // 1 Release
      Stub,                   // 2 QueryInterface
      stub_ret_true,          // 3 SurfaceScaleEnable
      set_surface_scale,      // 4 SetSurfaceScale
      stub_ret_true,          // 5 GetSurfaceScale
      get_surface_scale_caps, // 6 GetSurfaceScaleCaps
      stub_ret_true,          // 7 SurfaceRotateEnable
      stub_ret_true,          // 8 SetSurfaceRotate
      stub_ret_true,          // 9 GetSurfaceRotate
      stub_ret_true,          // 10 GetSurfaceRotateCaps
      stub_ret_true,          // 11 SurfaceTransparencyEnable
      stub_ret_true,          // 12 SetSurfaceTransparency
      stub_ret_true,          // 13 GetSurfaceTransparency
      stub_ret_true,          // 14 SetSurfaceTransparencyMap
      stub_ret_true,          // 15 GetSurfaceTransparencyMap
      stub_ret_true,          // 16 GetSurfaceTransparencyCaps
      stub_ret_true,          // 17 SurfaceColorKeyEnable
      stub_ret_true,          // 18 SetSurfaceColorKey
      stub_ret_true,          // 19 GetSurfaceColorKey
      stub_ret_true,          // 20 CreateCompositeSurface
      stub_ret_true,          // 21 SurfaceOverlayEnable
      stub_ret_true,          // 22 SurfaceOverlayLayerEnable
      stub_ret_true,          // 23 SurfaceOverlayBind
  };
  surface_manip_obj_ = BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
  return surface_manip_obj_;
}

namespace {

class Gles11ArmCoreAdapter : public IArmCore {
 public:
  explicit Gles11ArmCoreAdapter(IArmCore& real) : real_(real) {}
  void Reset() override { real_.Reset(); }
  void Step() override { real_.Step(); }
  uint64_t Run(uint64_t max_instructions) override { return real_.Run(max_instructions); }
  uint32_t GetRegister(int index) const override {
    if (index == kR0) return real_.GetRegister(kR1);
    if (index == kR1) return real_.GetRegister(kR2);
    if (index == kR2) return real_.GetRegister(kR3);
    if (index == kR3) return real_.GetMemory().Read32(real_.GetRegister(kSP));
    if (index == kSP) return real_.GetRegister(kSP) + 4;
    return real_.GetRegister(index);
  }
  void SetRegister(int index, uint32_t value) override {
    real_.SetRegister(index, value);
  }
  uint32_t GetCpsr() const override { return real_.GetCpsr(); }
  void SetCpsr(uint32_t value) override { real_.SetCpsr(value); }
  Memory& GetMemory() override { return real_.GetMemory(); }
  void SetCallOutRange(uint32_t base, uint32_t size) override { real_.SetCallOutRange(base, size); }
  void SetCallOutHandler(CallOutHandler handler) override { real_.SetCallOutHandler(std::move(handler)); }
 private:
  IArmCore& real_;
};

}  // namespace

uint32_t GlHle::BuildGles11(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                            uint32_t object_address) {
  auto GlesMethod = [](std::function<void(IArmCore&)> fn) -> HleRuntime::HleFunction {
    return [fn = std::move(fn)](IArmCore& c) {
      Gles11ArmCoreAdapter adapter(c);
      fn(adapter);
    };
  };

  // 150 slots total for IGLES11 (standard Qualcomm BREW SDK 4.0.2 / zeebx AEE slots):
  // 0..2: AddRef, Release, QueryInterface
  // 3..30: Float API (AlphaFunc..Translatef)
  // 31..149: Fixed/Core API (ActiveTexture..Viewport..)
  std::vector<HleRuntime::HleFunction> methods(150, Stub);
  methods[0] = Stub;  // AddRef
  methods[1] = Stub;  // Release
  methods[2] = Stub;  // QueryInterface

  // Core methods
  methods[31] = Stub;                                                          // 31 ActiveTexture
  methods[32] = GlesMethod([this](IArmCore& c) { GlAlphaFuncx(c); });          // 32 AlphaFuncx
  methods[33] = GlesMethod([this](IArmCore& c) { GlBindTexture(c); });         // 33 BindTexture
  methods[34] = GlesMethod([this](IArmCore& c) { GlBlendFunc(c); });           // 34 BlendFunc
  methods[35] = GlesMethod([this](IArmCore& c) { GlClear(c); });               // 35 Clear
  methods[36] = GlesMethod([this](IArmCore& c) { GlClearColorx(c); });         // 36 ClearColorx
  methods[37] = GlesMethod([this](IArmCore& c) { GlClearDepthx(c); });         // 37 ClearDepthx
  methods[39] = Stub;                                                          // 39 ClientActiveTexture
  methods[40] = GlesMethod([this](IArmCore& c) { GlColor4x(c); });             // 40 Color4x
  methods[42] = GlesMethod([this](IArmCore& c) { GlColorPointer(c); });        // 42 ColorPointer
  methods[43] = GlesMethod([this](IArmCore& c) { GlCompressedTexImage2D(c); });// 43 CompressedTexImage2D
  methods[48] = GlesMethod([this](IArmCore& c) { GlDeleteTextures(c); });      // 48 DeleteTextures
  methods[49] = GlesMethod([this](IArmCore& c) { GlDepthFunc(c); });           // 49 DepthFunc
  methods[50] = GlesMethod([this](IArmCore& c) { GlDepthMask(c); });           // 50 DepthMask
  methods[52] = GlesMethod([this](IArmCore& c) { GlDisable(c); });             // 52 Disable
  methods[53] = GlesMethod([this](IArmCore& c) { GlDisableClientState(c); }); // 53 DisableClientState
  methods[54] = GlesMethod([this](IArmCore& c) { GlDrawArrays(c); });          // 54 DrawArrays
  methods[55] = GlesMethod([this](IArmCore& c) { GlDrawElements(c); });        // 55 DrawElements
  methods[56] = GlesMethod([this](IArmCore& c) { GlEnable(c); });              // 56 Enable
  methods[57] = GlesMethod([this](IArmCore& c) { GlEnableClientState(c); });  // 57 EnableClientState
  methods[64] = GlesMethod([this](IArmCore& c) { GlGenTextures(c); });         // 64 GenTextures
  methods[65] = [](IArmCore& core) {
    uint32_t out_err = core.GetRegister(kR1);
    if (out_err != 0) {
      core.GetMemory().Write32(out_err, 0);
    }
    core.SetRegister(kR0, 0);  // GL_NO_ERROR
  };
  methods[66] = GlesMethod([this](IArmCore& c) { GlGetIntegerv(c); });         // 66 GetIntegerv
  methods[67] = GlesMethod([this](IArmCore& c) { GlGetString(c); });           // 67 GetString
  methods[74] = GlesMethod([this](IArmCore& c) { GlLoadIdentity(c); });        // 74 LoadIdentity
  methods[75] = GlesMethod([this](IArmCore& c) { GlLoadMatrixx(c); });         // 75 LoadMatrixx
  methods[79] = GlesMethod([this](IArmCore& c) { GlMatrixMode(c); });          // 79 MatrixMode
  methods[80] = GlesMethod([this](IArmCore& c) { GlMultMatrixx(c); });         // 80 MultMatrixx
  methods[84] = GlesMethod([this](IArmCore& c) { GlNormalPointer(c); });       // 84 NormalPointer
  methods[85] = GlesMethod([this](IArmCore& c) { GlOrthox(c); });              // 85 Orthox
  methods[89] = GlesMethod([this](IArmCore& c) { GlPopMatrix(c); });           // 89 PopMatrix
  methods[90] = GlesMethod([this](IArmCore& c) { GlPushMatrix(c); });          // 90 PushMatrix
  methods[92] = GlesMethod([this](IArmCore& c) { GlRotatex(c); });             // 92 Rotatex
  methods[95] = GlesMethod([this](IArmCore& c) { GlScalex(c); });              // 95 Scalex
  methods[101] = GlesMethod([this](IArmCore& c) { GlTexCoordPointer(c); });    // 101 TexCoordPointer
  methods[102] = GlesMethod([this](IArmCore& c) { GlTexEnvx(c); });            // 102 TexEnvx
  methods[103] = GlesMethod([this](IArmCore& c) { GlTexEnvxv(c); });           // 103 TexEnvxv
  methods[104] = GlesMethod([this](IArmCore& c) { GlTexImage2D(c); });         // 104 TexImage2D
  methods[105] = GlesMethod([this](IArmCore& c) { GlTexParameterx(c); });      // 105 TexParameterx
  methods[107] = GlesMethod([this](IArmCore& c) { GlTranslatex(c); });         // 107 Translatex
  methods[108] = GlesMethod([this](IArmCore& c) { GlVertexPointer(c); });      // 108 VertexPointer
  methods[109] = GlesMethod([this](IArmCore& c) { GlViewport(c); });           // 109 Viewport

  gles11_object_ = BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
  return gles11_object_;
}

}  // namespace zeebulator
