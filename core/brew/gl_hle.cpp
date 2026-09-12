#include "core/brew/gl_hle.h"

#include "core/control/debug_sink.h"

#include <array>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "core/brew/interface_object.h"
#include "core/loader/atitc.h"
#include "core/loader/obm1.h"
#include "core/brew/draw_stats.h"

namespace zeebulator {

namespace {
// Histograma de chamadas GL/GLES por slot (ZEEB_GL_TRACE=1).
// MOTIVO: a vtable do IGLES11 tem 150 slots e a maioria nasceu como Stub
// silencioso (devolve AEE_SUCCESS e nao faz nada). Sem contar slot a slot nao
// da para afirmar que "o backend segue as instrucoes do jogo" -- da so para
// torcer. O destrutor global despeja o histograma no fim do processo.
struct GlCallStats {
  std::array<uint64_t, 160> gles{};
  std::array<bool, 160> gles_stub{};
  std::array<uint64_t, 96> gl{};
  std::array<bool, 96> gl_stub{};
  ~GlCallStats() {
    if (std::getenv("ZEEB_GL_TRACE") == nullptr) return;
    std::fprintf(stderr, "\n=== histograma de chamadas GL (slot: chamadas [STUB]) ===\n");
    for (size_t i = 0; i < gles.size(); ++i) {
      if (gles[i] == 0) continue;
      std::fprintf(stderr, "IGLES11 slot %3zu: %10llu %s\n", i,
                   static_cast<unsigned long long>(gles[i]), gles_stub[i] ? "STUB" : "");
    }
    for (size_t i = 0; i < gl.size(); ++i) {
      if (gl[i] == 0) continue;
      std::fprintf(stderr, "IGL     slot %3zu: %10llu %s\n", i,
                   static_cast<unsigned long long>(gl[i]), gl_stub[i] ? "STUB" : "");
    }
  }
};
GlCallStats g_gl_call_stats;
}  // namespace


namespace {

float FloatArg(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

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
constexpr int32_t kMaxDrawVertices = 1 << 20;
constexpr int32_t kMaxTextureObjectsPerCall = 1 << 16;
constexpr uint64_t kMaxTextureUploadBytes = 64ull * 1024ull * 1024ull;

void WriteCString(Memory& memory, uint32_t addr, const char* text) {
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    memory.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory.Write8(addr + static_cast<uint32_t>(i), 0);
}

// Copia os pixels de uma textura da memoria do guest para um buffer do host,
// respeitando o GL_UNPACK_ALIGNMENT pedido via glPixelStorei (default 4, por
// especificacao). Cada linha da imagem no guest ocupa
// align_up(width*bytes_por_pixel, alignment) bytes; a copia sai SEMPRE
// compactada (stride = width*bytes_por_pixel), que e o contrato de
// GlTextureImage::pixels -- por isso o backend faz o upload com alinhamento 1.
//
// MOTIVO MEDIDO: glPixelStorei era Stub (25 chamadas medidas na Z-Wheel em
// ~28 s). Com alinhamento 4 e uma textura RGB de largura nao multipla de 4, ou
// 565 de largura impar, ler width*bpp contiguos por linha desloca a imagem
// linha a linha -- cores "escorregam" de canal e o resultado nao tem relacao
// com o que o jogo mandou desenhar.
bool CopyGuestPixels(Memory& memory, uint32_t pixels_ptr, int width, int height, GLenum format,
                     GLenum type, int unpack_alignment, std::vector<uint8_t>& out) {
  const int pixel_size = GlPixelSize(format, type);
  if (pixel_size <= 0 || width <= 0 || height <= 0) return false;
  const uint64_t row_bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(pixel_size);
  const uint64_t alignment =
      (unpack_alignment == 1 || unpack_alignment == 2 || unpack_alignment == 4 ||
       unpack_alignment == 8)
          ? static_cast<uint64_t>(unpack_alignment)
          : 4ull;
  const uint64_t padded_row = ((row_bytes + alignment - 1) / alignment) * alignment;
  const uint64_t tight_total = row_bytes * static_cast<uint64_t>(height);
  const uint64_t guest_total = padded_row * static_cast<uint64_t>(height);
  if (tight_total > kMaxTextureUploadBytes ||
      static_cast<uint64_t>(pixels_ptr) + guest_total > 0x100000000ull) {
    return false;
  }
  try {
    out.resize(static_cast<size_t>(tight_total));
  } catch (const std::exception&) {
    return false;
  }
  for (int row = 0; row < height; ++row) {
    const uint32_t src = pixels_ptr + static_cast<uint32_t>(padded_row * static_cast<uint64_t>(row));
    uint8_t* dst = out.data() + static_cast<size_t>(row_bytes) * static_cast<size_t>(row);
    for (uint64_t i = 0; i < row_bytes; ++i) {
      dst[i] = memory.Read8(src + static_cast<uint32_t>(i));
    }
  }
  return true;
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
    core.SetRegister(kR0, 3); // ECLASSNOTSUPPORT, AEEError.h
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
    // Anuncie apenas extensoes cujo caminho funcional existe. Anunciar
    // rotate/overlay/transparency/scale com stubs de sucesso fazia o guest
    // escolher um caminho acelerado que nao produzia estado nem pixels.
    static const char* const kDefaultEglExtensions =
        "EGL_QUALCOMM_get_color_buffer EGL_QUALCOMM_COLOR_BUFFER";
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
  egl_surfaces_[kSurfaceHandle] = EglSurfaceState{640, 480, 0};
  core.SetRegister(kR0, kSurfaceHandle);
}

namespace {
// Log dedicado do caminho EGL/pbuffer (ZEEB_LOG_EGL=1). Existe porque o
// palco 3D da Z-Wheel depende de uma cadeia inteira (criar pbuffer ->
// tornar corrente -> desenhar -> pedir o color buffer -> BitBlt) e sem
// observar cada elo nao da para saber qual elo esta quebrado.
void EglLog(const char* fmt, ...) {
  static const bool on = std::getenv("ZEEB_LOG_EGL") != nullptr;
  if (!on) return;
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[egl] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}
}  // namespace

void GlHle::EglCreatePbufferSurface(IArmCore& core) {
  // EGLSurface eglCreatePbufferSurface(EGLDisplay,EGLConfig,const EGLint*).
  // A lista termina em EGL_NONE; Z-Wheel pede explicitamente 640x330.
  constexpr uint32_t kEglNone = 0x3038;
  constexpr uint32_t kEglHeight = 0x3056;
  constexpr uint32_t kEglWidth = 0x3057;
  const uint32_t attrs = core.GetRegister(kR2);
  int32_t width = 0, height = 0;
  if (attrs != 0) {
    for (uint32_t i = 0; i < 64; ++i) {
      const uint32_t name = core.GetMemory().Read32(attrs + i * 8);
      if (name == kEglNone) break;
      const int32_t value = static_cast<int32_t>(core.GetMemory().Read32(attrs + i * 8 + 4));
      if (name == kEglWidth) width = value;
      if (name == kEglHeight) height = value;
    }
  }
  const uint64_t bytes = static_cast<uint64_t>(std::max(width, 0)) *
                         static_cast<uint64_t>(std::max(height, 0)) * 2u;
  if (width <= 0 || height <= 0 || width > 640 || height > 480 ||
      bytes > kMaxTextureUploadBytes ||
      static_cast<uint64_t>(next_pbuffer_pixels_) + bytes > 0x8c000000ull) {
    core.SetRegister(kR0, 0);  // EGL_NO_SURFACE
    return;
  }
  const uint32_t pixels = next_pbuffer_pixels_;
  next_pbuffer_pixels_ = static_cast<uint32_t>((static_cast<uint64_t>(pixels) + bytes + 0xfffu) &
                                               ~0xfffull);
  for (uint32_t off = 0; off < bytes; off += 2) core.GetMemory().Write16(pixels + off, 0);
  const uint32_t handle = next_egl_surface_++;
  egl_surfaces_[handle] = EglSurfaceState{width, height, pixels};
  EglLog("CreatePbufferSurface %dx%d handle=%u pixels=0x%08x", width, height, handle, pixels);
  core.SetRegister(kR0, handle);
}

void GlHle::EglDestroySurface(IArmCore& core) {
  const uint32_t surface = core.GetRegister(kR1);
  if (surface == current_draw_surface_) current_draw_surface_ = 0;
  const bool erased = egl_surfaces_.erase(surface) != 0;
  core.SetRegister(kR0, erased ? kEglTrue : kEglFalse);
}

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
  const uint32_t surface = core.GetRegister(kR1);
  auto it = egl_surfaces_.find(surface);
  if (it == egl_surfaces_.end()) ok = false;
  if (ok && attribute == kEglWidth) {
    out = it->second.width;
  } else if (ok && attribute == kEglHeight) {
    out = it->second.height;
  } else if (ok) {
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
  const uint32_t draw = core.GetRegister(kR1);
  const uint32_t read = core.GetRegister(kR2);
  const uint32_t ctx = core.GetRegister(kR3);
  if (ctx == 0) {
    current_draw_surface_ = 0;
    context_current_ = false;
    core.SetRegister(kR0, kEglTrue);
    return;
  }
  if (ctx != kContextHandle || egl_surfaces_.find(draw) == egl_surfaces_.end() ||
      egl_surfaces_.find(read) == egl_surfaces_.end()) {
    core.SetRegister(kR0, kEglFalse);
    return;
  }
  if (!context_current_ && !backend_.CreateContext()) {
    core.SetRegister(kR0, kEglFalse);
    return;
  }
  context_current_ = true;
  current_draw_surface_ = draw;
  // Superficie pbuffer = alvo offscreen proprio. Superficie de janela = FBO de
  // apresentacao. Sem essa separacao o jogo desenha o palco 3D no mesmo lugar
  // onde nos desenhamos a tela 2D, e o readback vira realimentacao.
  const auto& surf = egl_surfaces_[draw];
  if (surf.color_buffer != 0) {
    const bool bound = backend_.BindOffscreenTarget(surf.width, surf.height);
    EglLog("MakeCurrent pbuffer %dx%d alvo_offscreen=%s", surf.width, surf.height,
           bound ? "ok" : "INDISPONIVEL");
  } else {
    backend_.UnbindOffscreenTarget();
  }
  EglLog("MakeCurrent draw=%u read=%u ctx=0x%08x -> TRUE", draw, read, ctx);
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglGetColorBufferQualcomm(IArmCore& core) {
  // SDK platform/ui/inc/deprecated/gles/EGLext.h:
  //   void *eglGetColorBufferQUALCOMM(void)
  // Sem argumentos. Devolve o RGB565 cru da superficie corrente.
  //
  // O ponteiro sozinho nao basta: o guest (Z-Wheel, tectoy.mod 0x133b40 cria
  // um pbuffer 640x330) faz BitBlt DESSE buffer para compor o palco 3D com a
  // interface 2D. Enquanto nao haviamos feito readback do GL do host, o
  // buffer ficava zerado e o palco 3D simplesmente sumia da tela. Agora
  // trazemos os pixels reais do FBO do host e convertemos para RGB565.
  const auto it = egl_surfaces_.find(current_draw_surface_);
  if (it == egl_surfaces_.end()) {
    EglLog("GetColorBufferQUALCOMM sem superficie corrente (draw=%u) -> 0", current_draw_surface_);
    core.SetRegister(kR0, 0);
    return;
  }
  const bool synced = SyncSurfaceColorBuffer(core.GetMemory(), it->second);
  EglLog("GetColorBufferQUALCOMM surface=%u %dx%d buf=0x%08x readback=%s lr=0x%08x",
         current_draw_surface_, it->second.width, it->second.height, it->second.color_buffer,
         synced ? "ok" : "FALHOU", core.GetRegister(kLR));
  core.SetRegister(kR0, it->second.color_buffer);
}

bool GlHle::SyncSurfaceColorBuffer(Memory& memory, const EglSurfaceState& surface) {
  // Readback real: RGBA8888 (origem no topo) -> RGB565 no ponteiro do guest.
  //
  // Chave de bissecao ZEEB_NO_COLORBUF_READBACK=1: desliga o readback e deixa o
  // color buffer com o que o PROPRIO jogo escreveu nele. MOTIVO MEDIDO: o palco
  // 3D nunca teve pixel azul (0 de 211200) em nenhuma captura, nem antes nem
  // depois das correcoes de GL, mas a tela tinha azul dominante nas capturas em
  // que NAO havia readback. Isso indica que o fundo azul e desenhado pelo jogo
  // nesse mesmo buffer, e que sobrescrever o buffer inteiro o apaga. Sem esta
  // chave a hipotese nao pode ser testada A/B.
  if (std::getenv("ZEEB_NO_COLORBUF_READBACK") != nullptr) return false;
  if (surface.color_buffer == 0 || surface.width <= 0 || surface.height <= 0) return false;
  // O guest desenha na regiao de viewport que ele mesmo pediu. Se ainda nao
  // houve glViewport, assumimos a origem do FBO com o tamanho da superficie.
  // O alvo offscreen tem EXATAMENTE o tamanho da superficie, entao lemos o
  // retangulo inteiro a partir da origem. Usar o ultimo glViewport aqui era
  // errado: glReadPixels tem origem embaixo, e pedir (0,0,640,330) num FBO de
  // 640x480 devolvia as 330 linhas DE BAIXO (comprovado: batia 100% com as
  // linhas 150..480 da tela apresentada).
  const int rect_x = 0;
  const int rect_y = 0;
  const int rect_w = surface.width;
  const int rect_h = surface.height;
  std::vector<uint8_t> rgba;
  if (!backend_.ReadPixelsRgba(rect_x, rect_y, rect_w, rect_h, rgba)) return false;
  if (rgba.size() < static_cast<size_t>(rect_w) * static_cast<size_t>(rect_h) * 4u) return false;
  ++DrawStats::Instance().gl_color_buffer_readback;
  // Diagnostico de ordem de canais: grava o RGBA CRU vindo do host, antes de
  // qualquer conversao nossa. Comparando este arquivo com o bitmap que o guest
  // acaba exibindo, da para provar em qual etapa R e B trocam de lugar.
  if (const char* dump = std::getenv("ZEEB_EGL_DUMP")) {
    static uint64_t dump_calls = 0;
    // Reescreve o mesmo arquivo a cada 30 leituras: o primeiro quadro ainda e
    // a tela branca inicial, e o interessante e o estado ja em regime.
    if ((dump_calls++ % 30) == 0) {
      if (FILE* f = std::fopen(dump, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", rect_w, rect_h);
        for (int y = 0; y < rect_h; ++y) {
          for (int x = 0; x < rect_w; ++x) {
            const size_t p = (static_cast<size_t>(y) * static_cast<size_t>(rect_w) +
                              static_cast<size_t>(x)) * 4u;
            std::fputc(rgba[p + 0], f);
            std::fputc(rgba[p + 1], f);
            std::fputc(rgba[p + 2], f);
          }
        }
        std::fclose(f);
      }
    }
  }
  // Conta pixels nao pretos do que veio do host: distingue "readback ok, mas
  // o FBO estava vazio" de "readback ok com conteudo real".
  if (std::getenv("ZEEB_LOG_EGL") != nullptr) {
    size_t nonzero = 0, nonwhite = 0;
    for (size_t p = 0; p + 3 < rgba.size(); p += 4) {
      const bool black = rgba[p] == 0 && rgba[p + 1] == 0 && rgba[p + 2] == 0;
      const bool white = rgba[p] >= 250 && rgba[p + 1] >= 250 && rgba[p + 2] >= 250;
      if (!black) ++nonzero;
      if (!black && !white) ++nonwhite;
    }
    static uint64_t calls = 0;
    if ((calls++ % 30) == 0) {
      std::fprintf(stderr,
                   "[egl] readback rect=%d,%d %dx%d nao_preto=%zu nao_branco=%zu/%zu "
                   "draw_arrays=%llu clear=%llu tex=%llu\n",
                   rect_x, rect_y, rect_w, rect_h, nonzero, nonwhite, rgba.size() / 4,
                   (unsigned long long)DrawStats::Instance().gl_draw_arrays,
                   (unsigned long long)DrawStats::Instance().gl_clear,
                   (unsigned long long)DrawStats::Instance().gl_tex_image);
    }
  }
  for (int y = 0; y < rect_h; ++y) {
    for (int x = 0; x < rect_w; ++x) {
      const size_t src = (static_cast<size_t>(y) * static_cast<size_t>(rect_w) +
                          static_cast<size_t>(x)) * 4u;
      const uint16_t r5 = static_cast<uint16_t>(rgba[src + 0] >> 3);
      const uint16_t g6 = static_cast<uint16_t>(rgba[src + 1] >> 2);
      const uint16_t b5 = static_cast<uint16_t>(rgba[src + 2] >> 3);
      const uint16_t rgb565 = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
      const uint32_t dst = surface.color_buffer +
                           static_cast<uint32_t>((static_cast<size_t>(y) *
                                                  static_cast<size_t>(surface.width) +
                                                  static_cast<size_t>(x)) * 2u);
      memory.Write16(dst, rgb565);
    }
  }
  return true;
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
  // Guardado para o readback do pbuffer: o retangulo que o guest acabou de
  // pedir e exatamente a regiao do FBO onde ele desenhou o palco 3D.
  last_viewport_x_ = static_cast<int32_t>(core.GetRegister(kR0));
  last_viewport_y_ = static_cast<int32_t>(core.GetRegister(kR1));
  last_viewport_w_ = static_cast<int32_t>(core.GetRegister(kR2));
  last_viewport_h_ = static_cast<int32_t>(core.GetRegister(kR3));
  backend_.Viewport(static_cast<int>(core.GetRegister(kR0)),
                     static_cast<int>(core.GetRegister(kR1)),
                     static_cast<int>(core.GetRegister(kR2)),
                     static_cast<int>(core.GetRegister(kR3)));
}

// Logado (env-gated) porque o efeito de glCullFace so existe com
// GL_CULL_FACE (0x0B44) ligado, e o de GL_TEXTURE_2D e POR UNIDADE ativa --
// sem ver a sequencia real nao da para afirmar nada sobre descarte de face.
void GlHle::GlEnable(IArmCore& core) {
  GpuLog("Enable cap=0x%x", core.GetRegister(kR0));
  // Chave de bissecao ZEEB_GL_UNLIT=1: neutraliza a iluminacao de fixed-function.
  // MEDIDO: depois de ligar GL_LIGHTING/material de verdade, o fundo que era
  // BRANCO (79410 pixels) virou PRETO (81409 pixels) no bitmap do palco da
  // Z-Wheel. Isso isola a iluminacao como suspeita sem precisar recompilar.
  static const bool unlit = std::getenv("ZEEB_GL_UNLIT") != nullptr;
  constexpr uint32_t kGlLighting = 0x0B50u;
  if (unlit && core.GetRegister(kR0) == kGlLighting) return;
  backend_.Enable(core.GetRegister(kR0));
}
void GlHle::GlDisable(IArmCore& core) {
  GpuLog("Disable cap=0x%x", core.GetRegister(kR0));
  backend_.Disable(core.GetRegister(kR0));
}

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

void GlHle::GlLoadMatrixf(IArmCore& core) {
  uint32_t ptr = core.GetRegister(kR0);
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = FloatArg(core.GetMemory().Read32(ptr + i * 4u));
  backend_.LoadMatrix(m);
}

void GlHle::GlOrthof(IArmCore& core) {
  backend_.Ortho(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                 FloatArg(core.GetRegister(kR2)), FloatArg(core.GetRegister(kR3)),
                 FloatArg(HleRuntime::ReadStackArg(core, 0)),
                 FloatArg(HleRuntime::ReadStackArg(core, 1)));
}

void GlHle::GlTexEnvfv(IArmCore& core) {
  if (core.GetRegister(kR1) == 0x2200) {
    uint32_t ptr = core.GetRegister(kR2);
    if (ptr != 0) backend_.TexEnvMode(static_cast<GLenum>(FloatArg(core.GetMemory().Read32(ptr))));
  }
}

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

// Float matrix/state entry points. IGLES11 slots 3..30 are the float API
// (AEEGLES10.h/AEEGLES11.h order, mirrored in zeebx aee_slots.rs): without
// Translatef/Rotatef/Scalef/MultMatrixf every model transform was dropped, so
// titles that place sprites with the model matrix drew them all at the origin.
void GlHle::GlTranslatef(IArmCore& core) {
  backend_.Translate(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                     FloatArg(core.GetRegister(kR2)));
}

void GlHle::GlScalef(IArmCore& core) {
  backend_.Scale(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                 FloatArg(core.GetRegister(kR2)));
}

void GlHle::GlRotatef(IArmCore& core) {
  backend_.Rotate(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                  FloatArg(core.GetRegister(kR2)), FloatArg(core.GetRegister(kR3)));
}

void GlHle::GlMultMatrixf(IArmCore& core) {
  uint32_t ptr = core.GetRegister(kR0);
  std::array<float, 16> m{};
  for (int i = 0; i < 16; ++i) {
    m[static_cast<size_t>(i)] = FloatArg(core.GetMemory().Read32(ptr + static_cast<uint32_t>(i) * 4));
  }
  backend_.MultMatrix(m.data());
}

void GlHle::GlColor4f(IArmCore& core) {
  backend_.Color4(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                   FloatArg(core.GetRegister(kR2)), FloatArg(core.GetRegister(kR3)));
}

void GlHle::GlClearColorf(IArmCore& core) {
  backend_.ClearColor(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                      FloatArg(core.GetRegister(kR2)), FloatArg(core.GetRegister(kR3)));
}

void GlHle::GlFrustumf(IArmCore& core) {
  backend_.Frustum(FloatArg(core.GetRegister(kR0)), FloatArg(core.GetRegister(kR1)),
                   FloatArg(core.GetRegister(kR2)), FloatArg(core.GetRegister(kR3)),
                   FloatArg(HleRuntime::ReadStackArg(core, 0)),
                   FloatArg(HleRuntime::ReadStackArg(core, 1)));
}
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
  GpuLog("TexEnvx target=0x%x pname=0x%x param=0x%x", core.GetRegister(kR0),
         core.GetRegister(kR1), core.GetRegister(kR2));
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
  // GLES 1.x: size 2..4, stride nao negativo, tipo conhecido.
  const int size = static_cast<int32_t>(core.GetRegister(kR0));
  const GLenum type = core.GetRegister(kR1);
  const int stride = static_cast<int32_t>(core.GetRegister(kR2));
  vertex_array_ = (size >= 2 && size <= 4 && GlTypeSize(type) > 0 && stride >= 0)
                      ? ArrayState{vertex_array_.enabled, size, type, stride, core.GetRegister(kR3)}
                      : ArrayState{};
}

void GlHle::GlColorPointer(IArmCore& core) {
  // GLES exige quatro componentes de cor. Aceitar size=1 fazia o backend ler
  // quatro floats de um vetor com um, um OOB host controlado pelo guest.
  const int size = static_cast<int32_t>(core.GetRegister(kR0));
  const GLenum type = core.GetRegister(kR1);
  const int stride = static_cast<int32_t>(core.GetRegister(kR2));
  color_array_ = (size == 4 && GlTypeSize(type) > 0 && stride >= 0)
                     ? ArrayState{color_array_.enabled, size, type, stride, core.GetRegister(kR3)}
                     : ArrayState{};
}

void GlHle::GlTexCoordPointer(IArmCore& core) {
  const int size = static_cast<int32_t>(core.GetRegister(kR0));
  const GLenum type = core.GetRegister(kR1);
  const int stride = static_cast<int32_t>(core.GetRegister(kR2));
  // O ponteiro pertence a unidade escolhida pelo ultimo
  // glClientActiveTexture (medido: a Z-Wheel alterna entre GL_TEXTURE0 e
  // GL_TEXTURE1 a cada bloco de desenho).
  ArrayState& slot = texcoord_arrays_[client_active_unit_];
  slot = (size >= 2 && size <= 4 && GlTypeSize(type) > 0 && stride >= 0)
             ? ArrayState{slot.enabled, size, type, stride, core.GetRegister(kR3)}
             : ArrayState{};
}

void GlHle::GlNormalPointer(IArmCore& core) {
  const GLenum type = core.GetRegister(kR0);
  const int stride = static_cast<int32_t>(core.GetRegister(kR1));
  normal_array_ = (GlTypeSize(type) > 0 && stride >= 0)
                      ? ArrayState{normal_array_.enabled, 3, type, stride, core.GetRegister(kR2)}
                      : ArrayState{};
}

void GlHle::GlEnableClientState(IArmCore& core) {
  // void glEnableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = true; break;
    case kGlColorArray: color_array_.enabled = true; break;
    // GL_TEXTURE_COORD_ARRAY e por unidade de cliente, como o ponteiro.
    case kGlTextureCoordArray: texcoord_arrays_[client_active_unit_].enabled = true; break;
    case kGlNormalArray: normal_array_.enabled = true; break;
    default: break;
  }
}

void GlHle::GlDisableClientState(IArmCore& core) {
  // void glDisableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = false; break;
    case kGlColorArray: color_array_.enabled = false; break;
    case kGlTextureCoordArray: texcoord_arrays_[client_active_unit_].enabled = false; break;
    case kGlNormalArray: normal_array_.enabled = false; break;
    default: break;
  }
}

GlVertexArrays GlHle::ExtractArrays(Memory& memory,
                                      const std::vector<uint32_t>& indices) const {
  GlVertexArrays out;
  out.vertex_count = static_cast<int>(indices.size());

  auto extract = [&](const ArrayState& array, std::vector<float>& dest, bool normalize_ubyte,
                     int min_components, int max_components) -> bool {
    const int component_bytes = GlTypeSize(array.type);
    if (!array.enabled || array.pointer == 0 || array.size < min_components ||
        array.size > max_components || component_bytes <= 0 || array.stride < 0) return false;
    const uint64_t packed = static_cast<uint64_t>(array.size) * component_bytes;
    const uint64_t stride = array.stride != 0 ? static_cast<uint32_t>(array.stride) : packed;
    if (stride < packed) return false;
    const uint64_t values = static_cast<uint64_t>(indices.size()) * array.size;
    if (values > static_cast<uint64_t>(kMaxDrawVertices) * 4u) return false;
    dest.reserve(static_cast<size_t>(values));
    for (uint32_t index : indices) {
      const uint64_t base64 = static_cast<uint64_t>(array.pointer) +
                              static_cast<uint64_t>(index) * stride;
      if (base64 + packed > 0x100000000ull) {
        dest.clear();
        return false;
      }
      for (int c = 0; c < array.size; ++c) {
        float value = ReadGlComponent(memory, static_cast<uint32_t>(base64 + c * component_bytes),
                                      array.type);
        if (normalize_ubyte && array.type == kGlUnsignedByte) value /= 255.0f;
        dest.push_back(value);
      }
    }
    return true;
  };

  out.has_position = extract(vertex_array_, out.positions, false, 2, 4);
  if (out.has_position) out.position_size = vertex_array_.size;
  out.has_color = extract(color_array_, out.colors, true, 4, 4);
  out.has_texcoord = extract(texcoord_arrays_[0], out.texcoords, false, 2, 4);
  if (out.has_texcoord) out.texcoord_size = texcoord_arrays_[0].size;
  out.has_texcoord1 = extract(texcoord_arrays_[1], out.texcoords1, false, 2, 4);
  if (out.has_texcoord1) out.texcoord1_size = texcoord_arrays_[1].size;
  out.has_normal = extract(normal_array_, out.normals, false, 3, 3);
  return out;
}

void GlHle::GlDrawArrays(IArmCore& core) {
  // void glDrawArrays(GLenum mode, GLint first, GLsizei count)
  GLenum mode = core.GetRegister(kR0);
  auto first = static_cast<int32_t>(core.GetRegister(kR1));
  auto count = static_cast<int32_t>(core.GetRegister(kR2));

  if (first < 0 || count <= 0 || count > kMaxDrawVertices ||
      static_cast<int64_t>(first) + count > 0x100000000ll) return;
  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count));
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

  if (count <= 0 || count > kMaxDrawVertices || indices_ptr == 0 ||
      (type != kGlUnsignedByte && type != kGlUnsignedShort)) return;
  const uint64_t index_bytes = static_cast<uint64_t>(count) *
                               (type == kGlUnsignedShort ? 2u : 1u);
  if (static_cast<uint64_t>(indices_ptr) + index_bytes > 0x100000000ull) return;
  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < count; ++i) {
    uint32_t index = (type == kGlUnsignedShort)
                          ? memory.Read16(indices_ptr + static_cast<uint32_t>(i) * 2)
                          : memory.Read8(indices_ptr + static_cast<uint32_t>(i));
    indices.push_back(index);
  }
  ++DrawStats::Instance().gl_draw_arrays;
  GlVertexArrays arrays = ExtractArrays(memory, indices);
  backend_.DrawArrays(mode, arrays);
  GpuLog("DrawElements mode=0x%x count=%d type=0x%x", mode, count, type);
}

// --- Texture object management + upload -----------------------------------

void GlHle::GlGenTextures(IArmCore& core) {
  // void glGenTextures(GLsizei n, GLuint *textures)
  auto n = static_cast<int32_t>(core.GetRegister(kR0));
  uint32_t textures_ptr = core.GetRegister(kR1);
  if (n <= 0 || n > kMaxTextureObjectsPerCall || textures_ptr == 0 ||
      static_cast<uint64_t>(textures_ptr) + static_cast<uint64_t>(n) * 4u > 0x100000000ull) return;

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
  if (n <= 0 || n > kMaxTextureObjectsPerCall || textures_ptr == 0 ||
      static_cast<uint64_t>(textures_ptr) + static_cast<uint64_t>(n) * 4u > 0x100000000ull) return;

  std::vector<GLuint> textures(static_cast<size_t>(n));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < n; ++i) {
    textures[static_cast<size_t>(i)] = memory.Read32(textures_ptr + static_cast<uint32_t>(i) * 4);
  }
  backend_.DeleteTextures(n, textures.data());
}

void GlHle::GlBindTexture(IArmCore& core) {
  GpuLog("BindTexture target=0x%x name=%u", core.GetRegister(kR0), core.GetRegister(kR1));
  GpuLog("BindTexture target=0x%x name=%u", core.GetRegister(kR0), core.GetRegister(kR1));
  backend_.BindTexture(core.GetRegister(kR0), core.GetRegister(kR1));
}

// --- Estado fixed-function que o jogo pede e que era Stub -----------------
//
// EVIDENCIA: histograma por slot da vtable IGL rodando a Z-Wheel
// (tectoy.mod, clsid 17237912) por ~28 s. Todos os slots abaixo estavam como
// Stub (devolviam 0 e nao faziam nada) e mesmo assim o jogo os chamava:
// glActiveTexture 4240, glClientActiveTexture 4240, glMaterialxv 848,
// glHint 424, glShadeModel 319, glCullFace 213, glFinish 212,
// glStencilFunc 212, glStencilOp 212, glLightxv 106, glGetError 51,
// glPixelStorei 25. glFrontFace (slot 34) NAO aparece: o jogo nunca o chama.

namespace {

// Quantos componentes tem cada pname de glLightxv (GLES1.x). Ler 4 sempre
// seria ler memoria do guest que nao pertence ao parametro.
int GlLightParamCount(GLenum pname) {
  switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1203:  // GL_POSITION
      return 4;
    case 0x1204:  // GL_SPOT_DIRECTION
      return 3;
    default:
      // GL_SPOT_EXPONENT/GL_SPOT_CUTOFF e as tres atenuacoes sao escalares.
      return 1;
  }
}

int GlMaterialParamCount(GLenum pname) {
  switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1600:  // GL_EMISSION
    case 0x1602:  // GL_AMBIENT_AND_DIFFUSE
      return 4;
    default:
      return 1;  // GL_SHININESS
  }
}

int GlLightModelParamCount(GLenum pname) {
  return pname == 0x0B53 /* GL_LIGHT_MODEL_AMBIENT */ ? 4 : 1;
}

// Le `count` GLfixed consecutivos da memoria do guest e converte para float.
// `ptr` zero => nada a fazer (o chamador nao deve inventar valores).
bool ReadFixedVector(Memory& memory, uint32_t ptr, int count, float* out) {
  if (ptr == 0 || count <= 0) return false;
  for (int i = 0; i < count; ++i) {
    out[i] = FixedToFloat(static_cast<GLfixed>(memory.Read32(ptr + static_cast<uint32_t>(i) * 4)));
  }
  return true;
}

}  // namespace

// void glActiveTexture(GLenum texture) / void glClientActiveTexture(GLenum)
// 4240 chamadas de cada em ~28 s -- uma por bloco de desenho. Ignoradas,
// TUDO caia na unidade 0 do host, inclusive binds que o jogo queria em outra
// unidade. R0 e o primeiro argumento real (esta vtable nao passa `po`).
void GlHle::GlActiveTexture(IArmCore& core) {
  GpuLog("ActiveTexture unit=0x%x", core.GetRegister(kR0));
  // Chave de bissecao ZEEB_GL_NO_MULTITEX=1: colapsa tudo na unidade 0, que era
  // o comportamento antigo. MOTIVO: a Z-Wheel faz multitextura real (unidade 0
  // 3796x, unidade 1 2044x) e qualquer erro de amarre entre unidade e textura
  // troca a textura da unidade 0 -- apareceria como superficie preta.
  static const bool no_multitex = std::getenv("ZEEB_GL_NO_MULTITEX") != nullptr;
  if (no_multitex) return;
  backend_.ActiveTexture(core.GetRegister(kR0));
}

void GlHle::GlClientActiveTexture(IArmCore& core) {
  // Alem de repassar ao host, ESTA e a unidade a que os proximos
  // glTexCoordPointer/glEnableClientState(GL_TEXTURE_COORD_ARRAY) pertencem.
  // Unidades acima da ultima suportada sao registradas e mantidas na ultima
  // conhecida em vez de fingir que existe estado para elas.
  const GLenum unit = core.GetRegister(kR0);
  constexpr GLenum kGlTexture0 = 0x84C0;
  const int index = static_cast<int>(unit - kGlTexture0);
  if (index >= 0 && index < kMaxTextureUnits) {
    client_active_unit_ = index;
  } else {
    GpuLog("ClientActiveTexture unidade 0x%x fora das %d suportadas", unit, kMaxTextureUnits);
  }
  GpuLog("ClientActiveTexture unit=0x%x", unit);
  backend_.ClientActiveTexture(unit);
}

// void glCullFace(GLenum mode) -- 213 chamadas. O jogo escolhe qual face
// descartar (GL_BACK 0x0405, GL_FRONT 0x0404, GL_FRONT_AND_BACK 0x0408);
// engolir a escolha deixa o host no default e faz aparecer face de tras junto
// com a da frente.
void GlHle::GlCullFace(IArmCore& core) {
  GpuLog("CullFace mode=0x%x", core.GetRegister(kR0));
  // Chave de bissecao (ZEEB_GL_NO_CULLFACE=1): volta ao comportamento antigo
  // (chamada engolida em silencio) para comparar A/B na tela, do mesmo jeito
  // que ZEEB_NO_PBUFFER_FBO existe no backend do SDL. Medido na Z-Wheel: o
  // jogo alterna GL_BACK (0x0405, 147x) e GL_FRONT (0x0404, 146x) em ~35 s,
  // com GL_CULL_FACE sempre ligado (592 glEnable(0x0B44), zero glDisable) --
  // ou seja, sao dois passes por quadro e ignorar a escolha fazia os dois
  // descartarem a MESMA face.
  static const bool ignore = std::getenv("ZEEB_GL_NO_CULLFACE") != nullptr;
  if (ignore) return;
  backend_.CullFace(core.GetRegister(kR0));
}

// void glFrontFace(GLenum mode) -- zero chamadas medidas na Z-Wheel.
void GlHle::GlFrontFace(IArmCore& core) { backend_.FrontFace(core.GetRegister(kR0)); }

// void glShadeModel(GLenum mode) -- 319 chamadas (GL_FLAT/GL_SMOOTH).
void GlHle::GlShadeModel(IArmCore& core) { backend_.ShadeModel(core.GetRegister(kR0)); }

// void glHint(GLenum target, GLenum mode) -- 424 chamadas.
void GlHle::GlHint(IArmCore& core) {
  backend_.Hint(core.GetRegister(kR0), core.GetRegister(kR1));
}

// void glFinish(void) -- 212 chamadas. O jogo depende disso antes de ler o
// color buffer (eglGetColorBufferQUALCOMM) para compor o palco 3D no 2D.
void GlHle::GlFinish(IArmCore&) { backend_.Finish(); }

// GLenum glGetError(void) -- 51 chamadas. Devolver 0 fixo era uma mentira
// util: escondia erro real de upload/estado do driver. Agora sai o erro do
// host, que e o unico que pode ser verdadeiro aqui.
void GlHle::GlGetError(IArmCore& core) {
  GLenum err = backend_.GetError();
  if (err != 0) GpuLog("glGetError -> 0x%x", err);
  core.SetRegister(kR0, err);
}

// void glPixelStorei(GLenum pname, GLint param) -- 25 chamadas. O efeito que
// importa e do lado de CA: com GL_UNPACK_ALIGNMENT != 1 cada linha da imagem
// na memoria do guest e arredondada para cima, e GlTexImage2D tem de pular
// esse padding em vez de ler width*bpp contiguos.
void GlHle::GlPixelStorei(IArmCore& core) {
  constexpr GLenum kGlUnpackAlignment = 0x0CF5;
  GLenum pname = core.GetRegister(kR0);
  int32_t param = static_cast<int32_t>(core.GetRegister(kR1));
  // Chave de bissecao ZEEB_GL_NO_PIXELSTORE=1: volta a ignorar o alinhamento e
  // ler width*bpp contiguos, como antes. MOTIVO: a compactacao de linhas do
  // caminho de upload e um ponto unico de falha capaz de corromper textura
  // inteira (apareceria como regiao preta ou embaralhada).
  static const bool ignore_align = std::getenv("ZEEB_GL_NO_PIXELSTORE") != nullptr;
  if (pname == kGlUnpackAlignment && !ignore_align &&
      (param == 1 || param == 2 || param == 4 || param == 8)) {
    unpack_alignment_ = param;
  }
  GpuLog("PixelStorei pname=0x%x param=%d", pname, param);
  backend_.PixelStorei(pname, param);
}

// void glMaterialx(GLenum face, GLenum pname, GLfixed param)
// void glMaterialxv(GLenum face, GLenum pname, const GLfixed *params)
// 848 chamadas da forma vetorial. Junto com 424 glNormalPointer, e prova de
// que a Z-Wheel usa iluminacao fixed-function de verdade -- com material
// ignorado, o host usa o difuso default (cinza 0.8) para tudo.
void GlHle::GlMaterialx(IArmCore& core) {
  float value = FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2)));
  if (std::getenv("ZEEB_GL_UNLIT") != nullptr) return;
  backend_.Materialfv(core.GetRegister(kR0), core.GetRegister(kR1), &value, 1);
}

void GlHle::GlMaterialxv(IArmCore& core) {
  GLenum face = core.GetRegister(kR0);
  GLenum pname = core.GetRegister(kR1);
  uint32_t ptr = core.GetRegister(kR2);
  const int count = GlMaterialParamCount(pname);
  float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!ReadFixedVector(core.GetMemory(), ptr, count, values)) return;
  GpuLog("Materialxv face=0x%x pname=0x%x v=[%f %f %f %f] n=%d", face, pname, values[0],
         values[1], values[2], values[3], count);
  if (std::getenv("ZEEB_GL_UNLIT") != nullptr) return;
  backend_.Materialfv(face, pname, values, count);
}

// void glLightx(GLenum light, GLenum pname, GLfixed param)
// void glLightxv(GLenum light, GLenum pname, const GLfixed *params) -- 106
// chamadas da forma vetorial.
void GlHle::GlLightx(IArmCore& core) {
  float value = FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2)));
  if (std::getenv("ZEEB_GL_UNLIT") != nullptr) return;
  backend_.Lightfv(core.GetRegister(kR0), core.GetRegister(kR1), &value, 1);
}

void GlHle::GlLightxv(IArmCore& core) {
  GLenum light = core.GetRegister(kR0);
  GLenum pname = core.GetRegister(kR1);
  uint32_t ptr = core.GetRegister(kR2);
  const int count = GlLightParamCount(pname);
  float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!ReadFixedVector(core.GetMemory(), ptr, count, values)) return;
  GpuLog("Lightxv light=0x%x pname=0x%x v=[%f %f %f %f] n=%d", light, pname, values[0], values[1],
         values[2], values[3], count);
  if (std::getenv("ZEEB_GL_UNLIT") != nullptr) return;
  backend_.Lightfv(light, pname, values, count);
}

void GlHle::GlLightModelx(IArmCore& core) {
  float value = FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1)));
  backend_.LightModelfv(core.GetRegister(kR0), &value, 1);
}

void GlHle::GlLightModelxv(IArmCore& core) {
  GLenum pname = core.GetRegister(kR0);
  const int count = GlLightModelParamCount(pname);
  float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!ReadFixedVector(core.GetMemory(), core.GetRegister(kR1), count, values)) return;
  backend_.LightModelfv(pname, values, count);
}

// void glStencilFunc(GLenum func, GLint ref, GLuint mask) -- 212 chamadas.
// Sao 3 argumentos: cabem todos em r0..r2 pelo AAPCS, nenhum vai para a
// pilha (ReadStackArg so entra a partir do 5o argumento, como em
// glTexImage2D).
void GlHle::GlStencilFunc(IArmCore& core) {
  // Chave de bissecao ZEEB_GL_NO_STENCIL=1. MOTIVO: nenhum dos nossos FBOs
  // tem anexo de estencil, entao programar o teste so pode DESCARTAR pixel.
  // Sem buffer, o resultado do teste e indefinido no driver.
  static const bool no_stencil = std::getenv("ZEEB_GL_NO_STENCIL") != nullptr;
  if (no_stencil) return;
  backend_.StencilFunc(core.GetRegister(kR0), static_cast<GLint>(core.GetRegister(kR1)),
                       core.GetRegister(kR2));
}

// void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) -- 212 chamadas,
// tambem 3 argumentos em r0..r2.
void GlHle::GlStencilOp(IArmCore& core) {
  // Chave de bissecao ZEEB_GL_NO_STENCIL=1. MOTIVO: nenhum dos nossos FBOs
  // tem anexo de estencil, entao programar o teste so pode DESCARTAR pixel.
  // Sem buffer, o resultado do teste e indefinido no driver.
  static const bool no_stencil = std::getenv("ZEEB_GL_NO_STENCIL") != nullptr;
  if (no_stencil) return;
  backend_.StencilOp(core.GetRegister(kR0), core.GetRegister(kR1), core.GetRegister(kR2));
}

void GlHle::GlTexParameterx(IArmCore& core) {
  uint32_t target = core.GetRegister(kR0);
  uint32_t pname = core.GetRegister(kR1);
  uint32_t param = core.GetRegister(kR2);
  if (std::getenv("ZEEB_LOG_GPU")) {
    std::fprintf(stderr, "[tex_param_call] target=0x%x pname=0x%x param=0x%x\n", target, pname, param);
  }
  if (pname != 0) {
    backend_.TexParameter(target, pname, static_cast<GLint>(param));
  }
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
    if (!CopyGuestPixels(core.GetMemory(), pixels_ptr, image.width, image.height, image.format,
                          image.type, unpack_alignment_, pixel_bytes)) {
      return;
    }
    image.pixels = pixel_bytes.data();
  }
  ++DrawStats::Instance().gl_tex_image;
  // Diagnostico de ordem de canais: despeja a textura EXATAMENTE como ela sai
  // daqui para a GPU. Comparando com a arte original da' para dizer se R e B ja
  // chegam trocados do guest ou se a troca e nossa.
  if (const char* base = std::getenv("ZEEB_TEX_DUMP")) {
    static int dumped = 0;
    if (!pixel_bytes.empty() && dumped < 8 && image.width > 8 && image.height > 8) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s_%02d_%dx%d_f%x.ppm", base, dumped, image.width,
                    image.height, image.format);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", image.width, image.height);
        const int comps = (image.format == 0x1908 /*GL_RGBA*/) ? 4 : 3;
        for (size_t i = 0; i + comps <= pixel_bytes.size(); i += comps) {
          std::fputc(pixel_bytes[i + 0], f);
          std::fputc(pixel_bytes[i + 1], f);
          std::fputc(pixel_bytes[i + 2], f);
        }
        std::fclose(f);
        ++dumped;
      }
    }
  }
  GpuLog("TexImage2D %dx%d internal=0x%x format=0x%x type=0x%x pixels=%s", image.width,
         image.height, image.internal_format, image.format, image.type,
         pixels_ptr != 0 ? "yes" : "null");
  backend_.TexImage2D(target, image);
}

void GlHle::GlTexSubImage2D(IArmCore& core) {
  GLenum target = core.GetRegister(kR0);
  GlTextureSubImage image;
  image.level = static_cast<int>(core.GetRegister(kR1));
  image.xoffset = static_cast<int>(core.GetRegister(kR2));
  image.yoffset = static_cast<int>(core.GetRegister(kR3));
  image.width = static_cast<int>(HleRuntime::ReadStackArg(core, 0));
  image.height = static_cast<int>(HleRuntime::ReadStackArg(core, 1));
  image.format = HleRuntime::ReadStackArg(core, 2);
  image.type = HleRuntime::ReadStackArg(core, 3);
  uint32_t pixels_ptr = HleRuntime::ReadStackArg(core, 4);

  std::vector<uint8_t> pixel_bytes;
  if (pixels_ptr != 0 && image.width > 0 && image.height > 0) {
    if (!CopyGuestPixels(core.GetMemory(), pixels_ptr, image.width, image.height, image.format,
                          image.type, unpack_alignment_, pixel_bytes)) {
      return;
    }
    image.pixels = pixel_bytes.data();
  }
  GpuLog("TexSubImage2D %dx%d format=0x%x type=0x%x", image.width, image.height, image.format,
         image.type);
  backend_.TexSubImage2D(target, image);
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
  GpuLog("CompressedTexImage2D %dx%d internal=0x%x size=%u", width, height, internal_format,
         image_size);
  if (width <= 0 || height <= 0 || image_size > kMaxTextureUploadBytes || data_ptr == 0 ||
      static_cast<uint64_t>(data_ptr) + image_size > 0x100000000ull) return;
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
    if (image_size > 0xffffffffu - 8u) return;
    uint32_t total_size = 8 + image_size;
    std::vector<uint8_t> obm1_bytes;
    try { obm1_bytes.resize(total_size); } catch (const std::exception&) { return; }
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

  // Mesmo diagnostico de canais do caminho nao comprimido, agora DEPOIS do
  // decode ATITC -- e aqui que estao as artes grandes do palco.
  if (const char* base = std::getenv("ZEEB_TEX_DUMP")) {
    static int dumped_c = 0;
    if (dumped_c < 12 && width > 8 && height > 8) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s_atitc%02d_%dx%d.ppm", base, dumped_c, width, height);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", width, height);
        const auto& px = *decoded;
        for (size_t i = 0; i + 4 <= px.size(); i += 4) {
          std::fputc(px[i + 0], f);
          std::fputc(px[i + 1], f);
          std::fputc(px[i + 2], f);
        }
        std::fclose(f);
        ++dumped_c;
      }
    }
  }

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
      [this](IArmCore& c) { GlActiveTexture(c); },  // 3  glActiveTexture
      [this](IArmCore& c) { GlAlphaFuncx(c); },     // 4  glAlphaFuncx
      [this](IArmCore& c) { GlBindTexture(c); },  // 5  glBindTexture
      [this](IArmCore& c) { GlBlendFunc(c); },      // 6  glBlendFunc
      [this](IArmCore& c) { GlClear(c); },        // 7  glClear
      [this](IArmCore& c) { GlClearColorx(c); },  // 8  glClearColorx
      [this](IArmCore& c) { GlClearDepthx(c); },  // 9  glClearDepthx
      Stub,                                       // 10 glClearStencil
      [this](IArmCore& c) { GlClientActiveTexture(c); },  // 11 glClientActiveTexture
      [this](IArmCore& c) { GlColor4x(c); },      // 12 glColor4x
      Stub,                                       // 13 glColorMask
      [this](IArmCore& c) { GlColorPointer(c); }, // 14 glColorPointer
      [this](IArmCore& c) { GlCompressedTexImage2D(c); },  // 15 glCompressedTexImage2D
      Stub,                                       // 16 glCompressedTexSubImage2D
      Stub,                                       // 17 glCopyTexImage2D
      Stub,                                       // 18 glCopyTexSubImage2D
      [this](IArmCore& c) { GlCullFace(c); },     // 19 glCullFace
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
      [this](IArmCore& c) { GlFinish(c); },       // 30 glFinish
      Stub,                                       // 31 glFlush
      Stub,                                       // 32 glFogx
      Stub,                                       // 33 glFogxv
      [this](IArmCore& c) { GlFrontFace(c); },    // 34 glFrontFace
      [this](IArmCore& c) { GlFrustumx(c); },     // 35 glFrustumx
      [this](IArmCore& c) { GlGenTextures(c); },  // 36 glGenTextures
      [this](IArmCore& c) { GlGetError(c); },     // 37 glGetError
      [this](IArmCore& c) { GlGetIntegerv(c); },  // 38 glGetIntegerv
      [this](IArmCore& c) { GlGetString(c); },    // 39 glGetString
      [this](IArmCore& c) { GlHint(c); },         // 40 glHint
      [this](IArmCore& c) { GlLightModelx(c); },  // 41 glLightModelx
      [this](IArmCore& c) { GlLightModelxv(c); }, // 42 glLightModelxv
      [this](IArmCore& c) { GlLightx(c); },       // 43 glLightx
      [this](IArmCore& c) { GlLightxv(c); },      // 44 glLightxv
      Stub,                                       // 45 glLineWidthx
      [this](IArmCore& c) { GlLoadIdentity(c); }, // 46 glLoadIdentity
      [this](IArmCore& c) { GlLoadMatrixx(c); },  // 47 glLoadMatrixx
      Stub,                                       // 48 glLogicOp
      [this](IArmCore& c) { GlMaterialx(c); },    // 49 glMaterialx
      [this](IArmCore& c) { GlMaterialxv(c); },   // 50 glMaterialxv
      [this](IArmCore& c) { GlMatrixMode(c); },   // 51 glMatrixMode
      [this](IArmCore& c) { GlMultMatrixx(c); },  // 52 glMultMatrixx
      Stub,                                       // 53 glMultiTexCoord4x
      Stub,                                       // 54 glNormal3x
      [this](IArmCore& c) { GlNormalPointer(c); }, // 55 glNormalPointer
      [this](IArmCore& c) { GlOrthox(c); },       // 56 glOrthox
      [this](IArmCore& c) { GlPixelStorei(c); },  // 57 glPixelStorei
      Stub,                                       // 58 glPointSizex
      Stub,                                       // 59 glPolygonOffsetx
      [this](IArmCore& c) { GlPopMatrix(c); },    // 60 glPopMatrix
      [this](IArmCore& c) { GlPushMatrix(c); },   // 61 glPushMatrix
      Stub,                                       // 62 glReadPixels
      [this](IArmCore& c) { GlRotatex(c); },      // 63 glRotatex
      Stub,                                       // 64 glSampleCoveragex
      [this](IArmCore& c) { GlScalex(c); },       // 65 glScalex
      Stub,                                       // 66 glScissor
      [this](IArmCore& c) { GlShadeModel(c); },   // 67 glShadeModel
      [this](IArmCore& c) { GlStencilFunc(c); },  // 68 glStencilFunc
      Stub,                                       // 69 glStencilMask
      [this](IArmCore& c) { GlStencilOp(c); },    // 70 glStencilOp
      [this](IArmCore& c) { GlTexCoordPointer(c); }, // 71 glTexCoordPointer
      [this](IArmCore& c) { GlTexEnvx(c); },       // 72 glTexEnvx
      [this](IArmCore& c) { GlTexEnvxv(c); },      // 73 glTexEnvxv
      [this](IArmCore& c) { GlTexImage2D(c); },   // 74 glTexImage2D
      [this](IArmCore& c) { GlTexParameterx(c); }, // 75 glTexParameterx
      [this](IArmCore& c) { GlTexSubImage2D(c); }, // 76 glTexSubImage2D
      [this](IArmCore& c) { GlTranslatex(c); },   // 77 glTranslatex
      [this](IArmCore& c) { GlVertexPointer(c); }, // 78 glVertexPointer
      [this](IArmCore& c) { GlViewport(c); },     // 79 glViewport
  };
  // Mesmo histograma para a vtable IGL fixa (80 slots), pelo mesmo motivo.
  for (size_t slot = 0; slot < methods.size() && slot < g_gl_call_stats.gl.size(); ++slot) {
    auto* fnptr = methods[slot].target<void (*)(IArmCore&)>();
    g_gl_call_stats.gl_stub[slot] = (fnptr != nullptr && *fnptr == &Stub);
    auto inner = methods[slot];
    methods[slot] = [slot, inner](IArmCore& c) {
      ++g_gl_call_stats.gl[slot];
      inner(c);
    };
  }
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
      [this](IArmCore& c) { EglCreatePbufferSurface(c); },  // 14 eglCreatePbufferSurface
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
    // Assinatura tem seis argumentos incluindo pMe: dst=stack0, ret=stack1.
    // R4 e callee-saved e nunca e um argumento AAPCS; escrever nele corrompia
    // memoria guest arbitraria.
    uint32_t ret_ptr = HleRuntime::ReadStackArg(c, 1);
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
    // GetSurfaceScaleCaps(pMe,dpy,surf,caps,ret): quinto argumento.
    uint32_t ret_ptr = HleRuntime::ReadStackArg(c, 0);
    if (ret_ptr != 0) c.GetMemory().Write32(ret_ptr, kEglTrue);
    c.SetRegister(kR0, 0);
  };

  auto unsupported = [](IArmCore& c) {
    // Assinaturas variam por slot. Sem contrato confirmado, nao toque em
    // possiveis out-params e nao anuncie sucesso falso.
    c.SetRegister(kR0, 20);  // EUNSUPPORTED, AEEError.h
  };

  // EGL_SURFACE_MANIP has ~26 slots
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                   // 0 AddRef
      Stub,                   // 1 Release
      Stub,                   // 2 QueryInterface
      unsupported,          // 3 SurfaceScaleEnable
      set_surface_scale,      // 4 SetSurfaceScale
      unsupported,          // 5 GetSurfaceScale
      get_surface_scale_caps, // 6 GetSurfaceScaleCaps
      unsupported,          // 7 SurfaceRotateEnable
      unsupported,          // 8 SetSurfaceRotate
      unsupported,          // 9 GetSurfaceRotate
      unsupported,          // 10 GetSurfaceRotateCaps
      unsupported,          // 11 SurfaceTransparencyEnable
      unsupported,          // 12 SetSurfaceTransparency
      unsupported,          // 13 GetSurfaceTransparency
      unsupported,          // 14 SetSurfaceTransparencyMap
      unsupported,          // 15 GetSurfaceTransparencyMap
      unsupported,          // 16 GetSurfaceTransparencyCaps
      unsupported,          // 17 SurfaceColorKeyEnable
      unsupported,          // 18 SetSurfaceColorKey
      unsupported,          // 19 GetSurfaceColorKey
      unsupported,          // 20 CreateCompositeSurface
      unsupported,          // 21 SurfaceOverlayEnable
      unsupported,          // 22 SurfaceOverlayLayerEnable
      unsupported,          // 23 SurfaceOverlayBind
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
      c.SetRegister(kR0, 0);  // Default return code for BREW COM interface is AEE_SUCCESS (0)
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
  // Float API, 0-based indices from AEEGLES10/11's INHERIT_IGLES table.
  // Same COM ABI as fixed calls: `this` in R0 and AEE_SUCCESS in R0 on return.
  methods[16] = GlesMethod([this](IArmCore& c) { GlLoadMatrixf(c); });         // 16 LoadMatrixf
  // Float API slots from AEEGLES10.h/AEEGLES11.h order (see zeebx aee_slots.rs):
  // 3 AlphaFunc, 4 ClearColor, 6 Color4f, 10 Frustumf, 19 MultMatrixf,
  // 25 Rotatef, 26 Scalef, 30 Translatef.
  methods[4] = GlesMethod([this](IArmCore& c) { GlClearColorf(c); });          // 4 ClearColor
  methods[6] = GlesMethod([this](IArmCore& c) { GlColor4f(c); });              // 6 Color4f
  methods[10] = GlesMethod([this](IArmCore& c) { GlFrustumf(c); });            // 10 Frustumf
  methods[19] = GlesMethod([this](IArmCore& c) { GlMultMatrixf(c); });         // 19 MultMatrixf
  methods[25] = GlesMethod([this](IArmCore& c) { GlRotatef(c); });             // 25 Rotatef
  methods[26] = GlesMethod([this](IArmCore& c) { GlScalef(c); });              // 26 Scalef
  methods[30] = GlesMethod([this](IArmCore& c) { GlTranslatef(c); });          // 30 Translatef
  methods[22] = GlesMethod([this](IArmCore& c) { GlOrthof(c); });              // 22 Orthof
  methods[28] = GlesMethod([this](IArmCore& c) { GlTexEnvfv(c); });            // 28 TexEnvfv
  methods[2] = [](IArmCore& core) {
    // int QueryInterface(IGLES11* po, AEECLSID clsID, void** ppOut)
    uint32_t out_ptr = core.GetRegister(kR2);
    if (out_ptr != 0) {
      core.GetMemory().Write32(out_ptr, core.GetRegister(kR0));
    }
    core.SetRegister(kR0, 0);  // AEE_SUCCESS
  };

  // Core methods
  methods[31] = GlesMethod([this](IArmCore& c) { GlActiveTexture(c); });      // 31 ActiveTexture
  methods[32] = GlesMethod([this](IArmCore& c) { GlAlphaFuncx(c); });          // 32 AlphaFuncx
  methods[33] = GlesMethod([this](IArmCore& c) { GlBindTexture(c); });         // 33 BindTexture
  methods[34] = GlesMethod([this](IArmCore& c) { GlBlendFunc(c); });           // 34 BlendFunc
  methods[35] = GlesMethod([this](IArmCore& c) { GlClear(c); });               // 35 Clear
  methods[36] = GlesMethod([this](IArmCore& c) { GlClearColorx(c); });         // 36 ClearColorx
  methods[37] = GlesMethod([this](IArmCore& c) { GlClearDepthx(c); });         // 37 ClearDepthx
  methods[39] = GlesMethod([this](IArmCore& c) { GlClientActiveTexture(c); });// 39 ClientActiveTexture
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
  methods[65] = [this](IArmCore& core) {
    // int GetError(IGLES11* po, GLenum* pOut). Erro REAL do host, nao um zero
    // fixo: com GL de verdade em baixo, mentir aqui esconde erro de upload.
    uint32_t out_err = core.GetRegister(kR1);
    GLenum err = backend_.GetError();
    if (out_err != 0) {
      core.GetMemory().Write32(out_err, err);
    }
    core.SetRegister(kR0, 0);  // AEE_SUCCESS
  };
  methods[66] = GlesMethod([this](IArmCore& c) { GlGetIntegerv(c); });         // 66 GetIntegerv
  methods[67] = [this](IArmCore& core) {                                        // 67 GetString
    // int GetString(IGLES11* po, GLenum name, const char** ppOut)
    uint32_t name = core.GetRegister(kR1);
    uint32_t pp_out = core.GetRegister(kR2);
    constexpr GLenum kGlVendor = 0x1F00;
    constexpr GLenum kGlRenderer = 0x1F01;
    constexpr GLenum kGlVersion = 0x1F02;
    constexpr GLenum kGlExtensions = 0x1F03;
    const char* value = "";
    switch (name) {
      case kGlVendor: value = "Zeebulator"; break;
      case kGlRenderer: value = "Zeebulator Software Rasterizer"; break;
      case kGlVersion: value = "OpenGL ES-CM 1.1"; break;
      case kGlExtensions: {
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
    if (pp_out != 0) {
      core.GetMemory().Write32(pp_out, kQueryStringBufferAddr);
    }
    core.SetRegister(kR0, 0);  // AEE_SUCCESS
  };
  methods[74] = GlesMethod([this](IArmCore& c) { GlLoadIdentity(c); });        // 74 LoadIdentity
  methods[75] = GlesMethod([this](IArmCore& c) { GlLoadMatrixx(c); });         // 75 LoadMatrixx
  methods[79] = GlesMethod([this](IArmCore& c) { GlMatrixMode(c); });          // 79 MatrixMode
  methods[80] = GlesMethod([this](IArmCore& c) { GlMultMatrixx(c); });         // 80 MultMatrixx
  methods[83] = GlesMethod([this](IArmCore& c) { GlNormalPointer(c); });       // 83 NormalPointer
  methods[84] = GlesMethod([this](IArmCore& c) { GlOrthox(c); });              // 84 Orthox
  methods[85] = GlesMethod([this](IArmCore& c) { GlPixelStorei(c); });        // 85 PixelStorei
  methods[88] = GlesMethod([this](IArmCore& c) { GlPopMatrix(c); });           // 88 PopMatrix
  methods[89] = GlesMethod([this](IArmCore& c) { GlPushMatrix(c); });          // 89 PushMatrix
  methods[91] = GlesMethod([this](IArmCore& c) { GlRotatex(c); });             // 91 Rotatex
  methods[94] = GlesMethod([this](IArmCore& c) { GlScalex(c); });              // 94 Scalex
  methods[96] = GlesMethod([this](IArmCore& c) { GlShadeModel(c); });         // 96 ShadeModel
  methods[100] = GlesMethod([this](IArmCore& c) { GlTexCoordPointer(c); });    // 100 TexCoordPointer
  methods[101] = GlesMethod([this](IArmCore& c) { GlTexEnvx(c); });            // 101 TexEnvx
  methods[102] = GlesMethod([this](IArmCore& c) { GlTexEnvxv(c); });           // 102 TexEnvxv
  methods[103] = GlesMethod([this](IArmCore& c) { GlTexImage2D(c); });         // 103 TexImage2D
  methods[104] = GlesMethod([this](IArmCore& c) { GlTexParameterx(c); });      // 104 TexParameterx
  methods[105] = GlesMethod([this](IArmCore& c) { GlTexSubImage2D(c); });       // 105 TexSubImage2D
  methods[106] = GlesMethod([this](IArmCore& c) { GlTranslatex(c); });         // 106 Translatex
  methods[107] = GlesMethod([this](IArmCore& c) { GlVertexPointer(c); });      // 107 VertexPointer
  methods[108] = GlesMethod([this](IArmCore& c) { GlViewport(c); });           // 108 Viewport
  // 109 is ClipPlanef, not a second Viewport (zeebx aee_slots.rs lists
  // 108 Viewport / 109 ClipPlanef / 110 GetClipPlanef). Binding Viewport here
  // fed a plane equation pointer into glViewport as x/y/w/h.
  methods[109] = GlesMethod([](IArmCore& c) { c.SetRegister(kR0, 0); });        // 109 ClipPlanef

  // Instrumentacao: envolve cada slot num contador, marcando os que ainda sao
  // Stub silencioso. Nao muda comportamento; so torna a omissao visivel
  // (ZEEB_GL_TRACE=1 imprime o histograma no fim do processo).
  for (size_t slot = 0; slot < methods.size() && slot < g_gl_call_stats.gles.size(); ++slot) {
    auto* fnptr = methods[slot].target<void (*)(IArmCore&)>();
    g_gl_call_stats.gles_stub[slot] = (fnptr != nullptr && *fnptr == &Stub);
    auto inner = methods[slot];
    methods[slot] = [slot, inner](IArmCore& c) {
      ++g_gl_call_stats.gles[slot];
      inner(c);
    };
  }
  gles11_object_ = BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
  return gles11_object_;
}

}  // namespace zeebulator
