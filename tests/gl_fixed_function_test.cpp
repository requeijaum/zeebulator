#include "core/brew/gl_hle.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"
#include "core/gl_texture_log.h"

// Estado fixed-function que a Z-Wheel (tectoy.mod, clsid 17237912) REALMENTE
// chama e que ficava em Stub silencioso. As contagens citadas nos comentarios
// vem de um histograma por slot da vtable IGL medido em ~28 s de execucao
// real do jogo, nao de suposicao.
//
// Duas coisas sao verificadas aqui:
//  1. GlHle traduz os argumentos do guest (registradores ARM, GLfixed 16.16,
//     ponteiros para memoria emulada) para a forma nativa do host.
//  2. GlTextureRecordingBackend, o decorator que o game_probe SEMPRE usa,
//     encaminha cada um desses metodos. Esta parte existe porque um metodo
//     virtual novo com corpo default na base NAO obriga override: quando
//     ReadPixelsRgba nasceu assim, o decorator o engoliu em silencio e o
//     readback do palco 3D falhou em todo frame sem uma linha de erro.

using zeebulator::ArmInterpreter;
using zeebulator::GlBackend;
using zeebulator::GlHle;
using zeebulator::GlTextureRecordingBackend;
using zeebulator::HleRuntime;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kGlVtable = 0x80000000;
constexpr uint32_t kGlObject = 0x80001000;
constexpr uint32_t kScratch = 0x00090000;

constexpr zeebulator::GLfixed ToFixed(float v) {
  return static_cast<zeebulator::GLfixed>(v * 65536.0f);
}

// Backend que anota TUDO que recebe, inclusive o nome do metodo -- e o nome
// que permite afirmar "o decorator encaminhou este metodo" sem depender de
// nenhum valor de retorno.
class RecordingBackend : public GlBackend {
 public:
  bool CreateContext() override { calls.push_back("CreateContext"); return true; }
  void DestroyContext() override { calls.push_back("DestroyContext"); }
  void SwapBuffers() override { calls.push_back("SwapBuffers"); }
  bool BindOffscreenTarget(int, int) override { calls.push_back("BindOffscreenTarget"); return true; }
  void UnbindOffscreenTarget() override { calls.push_back("UnbindOffscreenTarget"); }
  bool ReadPixelsRgba(int, int, int, int, std::vector<uint8_t>&) override {
    calls.push_back("ReadPixelsRgba");
    return true;
  }

  void Clear(zeebulator::GLbitfield) override { calls.push_back("Clear"); }
  void ClearColor(float, float, float, float) override { calls.push_back("ClearColor"); }
  void Viewport(int, int, int, int) override { calls.push_back("Viewport"); }
  void Enable(zeebulator::GLenum) override { calls.push_back("Enable"); }
  void Disable(zeebulator::GLenum) override { calls.push_back("Disable"); }
  void MatrixMode(zeebulator::GLenum) override { calls.push_back("MatrixMode"); }
  void LoadIdentity() override { calls.push_back("LoadIdentity"); }
  void LoadMatrix(const float[16]) override { calls.push_back("LoadMatrix"); }
  void MultMatrix(const float[16]) override { calls.push_back("MultMatrix"); }
  void PushMatrix() override { calls.push_back("PushMatrix"); }
  void PopMatrix() override { calls.push_back("PopMatrix"); }
  void Ortho(float, float, float, float, float, float) override { calls.push_back("Ortho"); }
  void Frustum(float, float, float, float, float, float) override { calls.push_back("Frustum"); }
  void Translate(float, float, float) override { calls.push_back("Translate"); }
  void Rotate(float, float, float, float) override { calls.push_back("Rotate"); }
  void Scale(float, float, float) override { calls.push_back("Scale"); }
  void Color4(float, float, float, float) override { calls.push_back("Color4"); }
  void TexEnvMode(zeebulator::GLenum mode) override {
    calls.push_back("TexEnvMode");
    last_tex_env_mode = mode;
  }
  void AlphaFunc(zeebulator::GLenum, float) override { calls.push_back("AlphaFunc"); }
  void BlendFunc(zeebulator::GLenum, zeebulator::GLenum) override { calls.push_back("BlendFunc"); }
  void DepthFunc(zeebulator::GLenum) override { calls.push_back("DepthFunc"); }
  void ClearDepth(float) override { calls.push_back("ClearDepth"); }
  void DepthMask(bool) override { calls.push_back("DepthMask"); }
  void DrawArrays(zeebulator::GLenum, const zeebulator::GlVertexArrays& arrays) override {
    calls.push_back("DrawArrays");
    last_arrays = arrays;
  }

  void CullFace(zeebulator::GLenum mode) override {
    calls.push_back("CullFace");
    last_cull_face = mode;
  }
  void FrontFace(zeebulator::GLenum mode) override {
    calls.push_back("FrontFace");
    last_front_face = mode;
  }
  void ShadeModel(zeebulator::GLenum mode) override {
    calls.push_back("ShadeModel");
    last_shade_model = mode;
  }
  void ActiveTexture(zeebulator::GLenum texture) override {
    calls.push_back("ActiveTexture");
    last_active_texture = texture;
  }
  void ClientActiveTexture(zeebulator::GLenum texture) override {
    calls.push_back("ClientActiveTexture");
    last_client_active_texture = texture;
  }
  void PixelStorei(zeebulator::GLenum pname, zeebulator::GLint param) override {
    calls.push_back("PixelStorei");
    last_pixel_store_pname = pname;
    last_pixel_store_param = param;
  }
  void Materialfv(zeebulator::GLenum face, zeebulator::GLenum pname, const float* values,
                  int count) override {
    calls.push_back("Materialfv");
    last_material_face = face;
    last_material_pname = pname;
    last_material_values.assign(values, values + count);
  }
  void Lightfv(zeebulator::GLenum light, zeebulator::GLenum pname, const float* values,
               int count) override {
    calls.push_back("Lightfv");
    last_light = light;
    last_light_pname = pname;
    last_light_values.assign(values, values + count);
  }
  void LightModelfv(zeebulator::GLenum pname, const float* values, int count) override {
    calls.push_back("LightModelfv");
    last_light_model_pname = pname;
    last_light_model_values.assign(values, values + count);
  }
  void StencilFunc(zeebulator::GLenum func, zeebulator::GLint ref,
                    zeebulator::GLuint mask) override {
    calls.push_back("StencilFunc");
    last_stencil_func = func;
    last_stencil_ref = ref;
    last_stencil_mask = mask;
  }
  void StencilOp(zeebulator::GLenum sfail, zeebulator::GLenum dpfail,
                  zeebulator::GLenum dppass) override {
    calls.push_back("StencilOp");
    last_stencil_op = {sfail, dpfail, dppass};
  }
  void Hint(zeebulator::GLenum target, zeebulator::GLenum mode) override {
    calls.push_back("Hint");
    last_hint = {target, mode};
  }
  void Finish() override {
    calls.push_back("Finish");
    ++finish_count;
  }
  zeebulator::GLenum GetError() override {
    calls.push_back("GetError");
    return next_error;
  }

  void GenTextures(zeebulator::GLsizei n, zeebulator::GLuint* textures) override {
    calls.push_back("GenTextures");
    for (zeebulator::GLsizei i = 0; i < n; ++i) textures[i] = next_texture_id++;
  }
  void DeleteTextures(zeebulator::GLsizei, const zeebulator::GLuint*) override {
    calls.push_back("DeleteTextures");
  }
  void BindTexture(zeebulator::GLenum, zeebulator::GLuint) override {
    calls.push_back("BindTexture");
  }
  void TexParameter(zeebulator::GLenum, zeebulator::GLenum, zeebulator::GLint) override {
    calls.push_back("TexParameter");
  }
  void TexImage2D(zeebulator::GLenum, const zeebulator::GlTextureImage& image) override {
    calls.push_back("TexImage2D");
    last_image = image;
    if (image.pixels != nullptr) {
      size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                     static_cast<size_t>(zeebulator::GlPixelSize(image.format, image.type));
      last_image_pixels.assign(image.pixels, image.pixels + total);
    } else {
      last_image_pixels.clear();
    }
    last_image.pixels = nullptr;
  }
  void TexSubImage2D(zeebulator::GLenum, const zeebulator::GlTextureSubImage& image) override {
    calls.push_back("TexSubImage2D");
    if (image.pixels != nullptr) {
      size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                     static_cast<size_t>(zeebulator::GlPixelSize(image.format, image.type));
      last_image_pixels.assign(image.pixels, image.pixels + total);
    }
  }

  bool Saw(const std::string& name) const {
    for (const auto& call : calls) {
      if (call == name) return true;
    }
    return false;
  }

  std::vector<std::string> calls;
  zeebulator::GlVertexArrays last_arrays;
  zeebulator::GLuint next_texture_id = 1;
  zeebulator::GLenum next_error = 0;
  zeebulator::GLenum last_tex_env_mode = 0;
  zeebulator::GLenum last_cull_face = 0;
  zeebulator::GLenum last_front_face = 0;
  zeebulator::GLenum last_shade_model = 0;
  zeebulator::GLenum last_active_texture = 0;
  zeebulator::GLenum last_client_active_texture = 0;
  zeebulator::GLenum last_pixel_store_pname = 0;
  zeebulator::GLint last_pixel_store_param = 0;
  zeebulator::GLenum last_material_face = 0;
  zeebulator::GLenum last_material_pname = 0;
  std::vector<float> last_material_values;
  zeebulator::GLenum last_light = 0;
  zeebulator::GLenum last_light_pname = 0;
  std::vector<float> last_light_values;
  zeebulator::GLenum last_light_model_pname = 0;
  std::vector<float> last_light_model_values;
  zeebulator::GLenum last_stencil_func = 0;
  zeebulator::GLint last_stencil_ref = 0;
  zeebulator::GLuint last_stencil_mask = 0;
  std::array<zeebulator::GLenum, 3> last_stencil_op{};
  std::array<zeebulator::GLenum, 2> last_hint{};
  int finish_count = 0;
  zeebulator::GlTextureImage last_image;
  std::vector<uint8_t> last_image_pixels;
};

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  RecordingBackend backend;
  GlHle gl_hle{backend};
  uint32_t gl_obj;

  Fixture() { gl_obj = gl_hle.BuildGl(cpu.GetMemory(), hle, kGlVtable, kGlObject); }

  uint32_t GlSlot(uint32_t slot) { return cpu.GetMemory().Read32(kGlVtable + slot * 4); }
};

}  // namespace

// --- Marshaling IGL -> GlBackend -------------------------------------------

TEST(GlFixedFunction, CullFaceReachesBackend) {
  // 213 chamadas medidas. GL_BACK = 0x0405. Este e o suspeito direto do
  // sintoma "a roda tem 2 raios, entre frente e fundo": com a chamada
  // engolida, o host ficava no default em vez da escolha do jogo.
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(19), 0x0405);
  EXPECT_EQ(f.backend.last_cull_face, 0x0405u);
}

TEST(GlFixedFunction, FrontFaceSlotIsWiredEvenThoughZWheelNeverCallsIt) {
  // Medido: slot 34 tem ZERO chamadas na Z-Wheel -- o jogo usa o default
  // GL_CCW. O slot existe e funciona, para nao virar o proximo Stub mudo.
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(34), 0x0900);  // GL_CW
  EXPECT_EQ(f.backend.last_front_face, 0x0900u);
}

TEST(GlFixedFunction, ShadeModelReachesBackend) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(67), 0x1D00);  // GL_FLAT
  EXPECT_EQ(f.backend.last_shade_model, 0x1D00u);
}

TEST(GlFixedFunction, ActiveAndClientActiveTextureReachBackend) {
  // 4240 chamadas de cada em ~28 s. Ignoradas, todo bind/coordenada caia na
  // unidade 0 do host.
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(3), 0x84C1);   // GL_TEXTURE1
  EXPECT_EQ(f.backend.last_active_texture, 0x84C1u);
  f.hle.CallArmFunction(f.GlSlot(11), 0x84C0);  // GL_TEXTURE0
  EXPECT_EQ(f.backend.last_client_active_texture, 0x84C0u);
}

TEST(GlFixedFunction, HintForwardsBothArgs) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(40), 0x0C50, 0x1101);  // GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST
  EXPECT_EQ(f.backend.last_hint[0], 0x0C50u);
  EXPECT_EQ(f.backend.last_hint[1], 0x1101u);
}

TEST(GlFixedFunction, FinishReachesBackend) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(30));
  EXPECT_EQ(f.backend.finish_count, 1);
}

TEST(GlFixedFunction, GetErrorReturnsRealHostError) {
  // Antes: 0 fixo, sempre. Isso escondia erro real de upload de textura --
  // o jogo perguntava 51 vezes e sempre ouvia "esta tudo bem".
  Fixture f;
  f.backend.next_error = 0x0501;  // GL_INVALID_VALUE
  EXPECT_EQ(f.hle.CallArmFunction(f.GlSlot(37)), 0x0501u);
  f.backend.next_error = 0;
  EXPECT_EQ(f.hle.CallArmFunction(f.GlSlot(37)), 0u);
}

TEST(GlFixedFunction, StencilFuncAndOpForwardThreeRegisterArgs) {
  // 212 chamadas de cada. Sao 3 argumentos: pelo AAPCS ficam em r0..r2,
  // nenhum na pilha.
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(68), 0x0202 /*GL_EQUAL*/, 1, 0xFFu);
  EXPECT_EQ(f.backend.last_stencil_func, 0x0202u);
  EXPECT_EQ(f.backend.last_stencil_ref, 1);
  EXPECT_EQ(f.backend.last_stencil_mask, 0xFFu);

  f.hle.CallArmFunction(f.GlSlot(70), 0x1E00 /*GL_KEEP*/, 0x1E00, 0x1E01 /*GL_REPLACE*/);
  EXPECT_EQ(f.backend.last_stencil_op[0], 0x1E00u);
  EXPECT_EQ(f.backend.last_stencil_op[2], 0x1E01u);
}

TEST(GlFixedFunction, MaterialxvReadsFixedVectorFromGuestMemory) {
  // 848 chamadas medidas. GL_AMBIENT_AND_DIFFUSE (0x1602) tem 4 componentes
  // em 16.16; GL_SHININESS (0x1601) tem 1 -- ler 4 sempre invadiria memoria
  // do guest que nao pertence ao parametro.
  Fixture f;
  f.cpu.GetMemory().Write32(kScratch + 0, static_cast<uint32_t>(ToFixed(0.25f)));
  f.cpu.GetMemory().Write32(kScratch + 4, static_cast<uint32_t>(ToFixed(0.5f)));
  f.cpu.GetMemory().Write32(kScratch + 8, static_cast<uint32_t>(ToFixed(0.75f)));
  f.cpu.GetMemory().Write32(kScratch + 12, static_cast<uint32_t>(ToFixed(1.0f)));
  f.hle.CallArmFunction(f.GlSlot(50), 0x0408 /*GL_FRONT_AND_BACK*/, 0x1602, kScratch);
  EXPECT_EQ(f.backend.last_material_face, 0x0408u);
  EXPECT_EQ(f.backend.last_material_pname, 0x1602u);
  ASSERT_EQ(f.backend.last_material_values.size(), 4u);
  EXPECT_FLOAT_EQ(f.backend.last_material_values[0], 0.25f);
  EXPECT_FLOAT_EQ(f.backend.last_material_values[3], 1.0f);

  f.cpu.GetMemory().Write32(kScratch + 32, static_cast<uint32_t>(ToFixed(32.0f)));
  f.hle.CallArmFunction(f.GlSlot(50), 0x0408, 0x1601, kScratch + 32);
  ASSERT_EQ(f.backend.last_material_values.size(), 1u);
  EXPECT_FLOAT_EQ(f.backend.last_material_values[0], 32.0f);
}

TEST(GlFixedFunction, LightxvReadsPerPnameComponentCount) {
  // 106 chamadas medidas, e o jogo tambem envia normais (424
  // glNormalPointer) -- ou seja, usa iluminacao de verdade.
  Fixture f;
  f.cpu.GetMemory().Write32(kScratch + 0, static_cast<uint32_t>(ToFixed(1.0f)));
  f.cpu.GetMemory().Write32(kScratch + 4, static_cast<uint32_t>(ToFixed(2.0f)));
  f.cpu.GetMemory().Write32(kScratch + 8, static_cast<uint32_t>(ToFixed(3.0f)));
  f.cpu.GetMemory().Write32(kScratch + 12, static_cast<uint32_t>(ToFixed(0.0f)));
  f.hle.CallArmFunction(f.GlSlot(44), 0x4000 /*GL_LIGHT0*/, 0x1203 /*GL_POSITION*/, kScratch);
  EXPECT_EQ(f.backend.last_light, 0x4000u);
  ASSERT_EQ(f.backend.last_light_values.size(), 4u);
  EXPECT_FLOAT_EQ(f.backend.last_light_values[1], 2.0f);

  // GL_SPOT_DIRECTION tem 3 componentes.
  f.hle.CallArmFunction(f.GlSlot(44), 0x4000, 0x1204, kScratch);
  EXPECT_EQ(f.backend.last_light_values.size(), 3u);

  // GL_SPOT_CUTOFF e escalar.
  f.hle.CallArmFunction(f.GlSlot(44), 0x4000, 0x1206, kScratch);
  EXPECT_EQ(f.backend.last_light_values.size(), 1u);
}

TEST(GlFixedFunction, LightxvWithNullPointerDoesNotInventValues) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(44), 0x4000, 0x1203, 0);
  EXPECT_FALSE(f.backend.Saw("Lightfv"));
}

TEST(GlFixedFunction, PixelStoreiReachesBackend) {
  Fixture f;
  f.hle.CallArmFunction(f.GlSlot(57), 0x0CF5 /*GL_UNPACK_ALIGNMENT*/, 1);
  EXPECT_EQ(f.backend.last_pixel_store_pname, 0x0CF5u);
  EXPECT_EQ(f.backend.last_pixel_store_param, 1);
}

TEST(GlFixedFunction, TexImage2DHonorsGuestUnpackAlignment) {
  // Com GL_UNPACK_ALIGNMENT = 4 (o default da especificacao) uma imagem RGB
  // de 3 pixels de largura ocupa 12 bytes por linha na memoria do guest
  // (9 bytes uteis + 3 de padding). Ler 9 bytes contiguos por linha
  // deslocaria a segunda linha em 3 bytes -- cor "escorregando" de canal.
  Fixture f;
  constexpr uint32_t kPixels = kScratch + 0x100;
  auto& mem = f.cpu.GetMemory();
  for (uint32_t row = 0; row < 2; ++row) {
    for (uint32_t i = 0; i < 9; ++i) {
      mem.Write8(kPixels + row * 12 + i, static_cast<uint8_t>(row * 100 + i));
    }
    for (uint32_t pad = 9; pad < 12; ++pad) {
      mem.Write8(kPixels + row * 12 + pad, 0xEE);  // padding, nunca deve aparecer
    }
  }
  f.cpu.SetRegister(zeebulator::kSP, kScratch);
  mem.Write32(kScratch + 0, 2);           // height
  mem.Write32(kScratch + 4, 0);           // border
  mem.Write32(kScratch + 8, 0x1907);      // format = GL_RGB
  mem.Write32(kScratch + 12, 0x1401);     // type = GL_UNSIGNED_BYTE
  mem.Write32(kScratch + 16, kPixels);    // pixels
  f.hle.CallArmFunction(f.GlSlot(74), 0x0DE1 /*GL_TEXTURE_2D*/, 0, 0x1907, 3);

  ASSERT_EQ(f.backend.last_image_pixels.size(), 18u);  // 2 linhas compactadas
  EXPECT_EQ(f.backend.last_image_pixels[0], 0);
  EXPECT_EQ(f.backend.last_image_pixels[8], 8);
  EXPECT_EQ(f.backend.last_image_pixels[9], 100);   // inicio da linha 1
  EXPECT_EQ(f.backend.last_image_pixels[17], 108);
  for (uint8_t byte : f.backend.last_image_pixels) {
    EXPECT_NE(byte, 0xEE) << "byte de padding vazou para o upload";
  }
}

TEST(GlFixedFunction, TexImage2DWithAlignmentOneReadsContiguousRows) {
  Fixture f;
  constexpr uint32_t kPixels = kScratch + 0x200;
  auto& mem = f.cpu.GetMemory();
  for (uint32_t i = 0; i < 18; ++i) mem.Write8(kPixels + i, static_cast<uint8_t>(i));
  f.hle.CallArmFunction(f.GlSlot(57), 0x0CF5, 1);  // glPixelStorei(UNPACK_ALIGNMENT, 1)
  f.cpu.SetRegister(zeebulator::kSP, kScratch);
  mem.Write32(kScratch + 0, 2);
  mem.Write32(kScratch + 4, 0);
  mem.Write32(kScratch + 8, 0x1907);
  mem.Write32(kScratch + 12, 0x1401);
  mem.Write32(kScratch + 16, kPixels);
  f.hle.CallArmFunction(f.GlSlot(74), 0x0DE1, 0, 0x1907, 3);
  ASSERT_EQ(f.backend.last_image_pixels.size(), 18u);
  EXPECT_EQ(f.backend.last_image_pixels[9], 9);
}

// --- O decorator tem de encaminhar TUDO ------------------------------------

TEST(GlBackendForwarding, RecordingDecoratorForwardsEveryFixedFunctionMethod) {
  // ARMADILHA COMPROVADA: metodo virtual novo com corpo default na classe
  // base nao obriga override, entao o decorator pode engolir o metodo sem
  // nenhum erro de compilacao (foi o que aconteceu com ReadPixelsRgba e,
  // depois, com TexEnvMode -- 4135 glTexEnvx por execucao morrendo aqui).
  RecordingBackend real;
  GlTextureRecordingBackend decorator(real);

  decorator.TexEnvMode(0x1E01);
  decorator.CullFace(0x0405);
  decorator.FrontFace(0x0900);
  decorator.ShadeModel(0x1D00);
  decorator.ActiveTexture(0x84C1);
  decorator.ClientActiveTexture(0x84C1);
  decorator.PixelStorei(0x0CF5, 1);
  const float material[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  decorator.Materialfv(0x0408, 0x1602, material, 4);
  decorator.Lightfv(0x4000, 0x1203, material, 4);
  decorator.LightModelfv(0x0B53, material, 4);
  decorator.StencilFunc(0x0202, 1, 0xFF);
  decorator.StencilOp(0x1E00, 0x1E00, 0x1E01);
  decorator.Hint(0x0C50, 0x1101);
  decorator.Finish();
  real.next_error = 0x0500;
  zeebulator::GLenum err = decorator.GetError();

  const std::vector<std::string> expected = {
      "TexEnvMode", "CullFace",    "FrontFace",  "ShadeModel", "ActiveTexture",
      "ClientActiveTexture",       "PixelStorei", "Materialfv", "Lightfv",
      "LightModelfv",              "StencilFunc", "StencilOp",  "Hint",
      "Finish",                    "GetError"};
  EXPECT_EQ(real.calls, expected);
  EXPECT_EQ(err, 0x0500u) << "GetError do decorator tem de devolver o erro REAL do host";
  EXPECT_EQ(real.last_cull_face, 0x0405u);
  EXPECT_EQ(real.last_tex_env_mode, 0x1E01u);
  ASSERT_EQ(real.last_material_values.size(), 4u);
  EXPECT_FLOAT_EQ(real.last_material_values[2], 0.3f);
}

TEST(GlBackendForwarding, RecordingDecoratorStillForwardsThePreExistingSurface) {
  // Regressao: os metodos que ja existiam continuam encaminhados (e o
  // ReadPixelsRgba, cuja falha silenciosa originou este teste).
  RecordingBackend real;
  GlTextureRecordingBackend decorator(real);
  std::vector<uint8_t> pixels;
  EXPECT_TRUE(decorator.CreateContext());
  decorator.SwapBuffers();
  EXPECT_TRUE(decorator.BindOffscreenTarget(64, 64));
  decorator.UnbindOffscreenTarget();
  EXPECT_TRUE(decorator.ReadPixelsRgba(0, 0, 4, 4, pixels));
  decorator.Clear(0x4000);
  decorator.AlphaFunc(0x0205, 0.0f);
  decorator.BlendFunc(0x0302, 0x0303);
  decorator.DepthFunc(0x0203);
  decorator.DepthMask(false);
  zeebulator::GlVertexArrays arrays;
  decorator.DrawArrays(0x0004, arrays);
  decorator.DestroyContext();
  for (const char* name : {"CreateContext", "SwapBuffers", "BindOffscreenTarget",
                            "UnbindOffscreenTarget", "ReadPixelsRgba", "Clear", "AlphaFunc",
                            "BlendFunc", "DepthFunc", "DepthMask", "DrawArrays",
                            "DestroyContext"}) {
    EXPECT_TRUE(real.Saw(name)) << name << " nao foi encaminhado pelo decorator";
  }
}

// --- Multitextura: coordenadas por unidade ---------------------------------

TEST(GlFixedFunction, TexCoordPointerBelongsToTheClientActiveUnit) {
  // MEDIDO na Z-Wheel (35 s, ZEEB_LOG_GPU=1): glClientActiveTexture chega
  // 3796 vezes com GL_TEXTURE0 e 2044 vezes com GL_TEXTURE1. Com um unico
  // array global de coordenadas, o ponteiro da unidade 1 sobrescrevia o da
  // unidade 0 -- as duas etapas de textura passavam a amostrar com a
  // coordenada errada, que e um caminho direto para textura sair preta.
  Fixture f;
  auto& mem = f.cpu.GetMemory();
  auto WriteFloat = [&mem](uint32_t addr, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    mem.Write32(addr, bits);
  };
  constexpr uint32_t kPos = kScratch + 0x300;
  constexpr uint32_t kUv0 = kScratch + 0x400;
  constexpr uint32_t kUv1 = kScratch + 0x500;
  for (int i = 0; i < 3; ++i) {
    WriteFloat(kPos + i * 8 + 0, static_cast<float>(i));
    WriteFloat(kPos + i * 8 + 4, 0.0f);
    WriteFloat(kUv0 + i * 8 + 0, 0.125f * i);
    WriteFloat(kUv0 + i * 8 + 4, 0.0f);
    WriteFloat(kUv1 + i * 8 + 0, 0.5f + 0.125f * i);
    WriteFloat(kUv1 + i * 8 + 4, 1.0f);
  }
  constexpr uint32_t kGlFloatType = 0x1406;
  constexpr uint32_t kGlTexture0 = 0x84C0;
  constexpr uint32_t kTexCoordArray = 0x8078;

  // Unidade 0: coordenadas kUv0.
  f.hle.CallArmFunction(f.GlSlot(11), kGlTexture0);
  f.hle.CallArmFunction(f.GlSlot(71), 2, kGlFloatType, 0, kUv0);
  f.hle.CallArmFunction(f.GlSlot(29), kTexCoordArray);
  // Unidade 1: coordenadas kUv1 -- NAO pode apagar as da unidade 0.
  f.hle.CallArmFunction(f.GlSlot(11), kGlTexture0 + 1);
  f.hle.CallArmFunction(f.GlSlot(71), 2, kGlFloatType, 0, kUv1);
  f.hle.CallArmFunction(f.GlSlot(29), kTexCoordArray);
  // Posicoes.
  f.hle.CallArmFunction(f.GlSlot(78), 2, kGlFloatType, 0, kPos);
  f.hle.CallArmFunction(f.GlSlot(29), 0x8074 /*GL_VERTEX_ARRAY*/);

  f.hle.CallArmFunction(f.GlSlot(26), 0x0004 /*GL_TRIANGLES*/, 0, 3);

  const auto& arrays = f.backend.last_arrays;
  ASSERT_TRUE(arrays.has_texcoord);
  ASSERT_TRUE(arrays.has_texcoord1);
  ASSERT_EQ(arrays.texcoords.size(), 6u);
  ASSERT_EQ(arrays.texcoords1.size(), 6u);
  EXPECT_FLOAT_EQ(arrays.texcoords[2], 0.125f);
  EXPECT_FLOAT_EQ(arrays.texcoords1[2], 0.625f);
  EXPECT_FLOAT_EQ(arrays.texcoords1[1], 1.0f);
}

TEST(GlFixedFunction, DisableClientStateOnlyAffectsItsOwnUnit) {
  Fixture f;
  auto& mem = f.cpu.GetMemory();
  auto WriteFloat = [&mem](uint32_t addr, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    mem.Write32(addr, bits);
  };
  constexpr uint32_t kPos = kScratch + 0x600;
  constexpr uint32_t kUv = kScratch + 0x700;
  for (int i = 0; i < 3; ++i) {
    WriteFloat(kPos + i * 8 + 0, static_cast<float>(i));
    WriteFloat(kPos + i * 8 + 4, 0.0f);
    WriteFloat(kUv + i * 8 + 0, 0.25f);
    WriteFloat(kUv + i * 8 + 4, 0.75f);
  }
  constexpr uint32_t kGlFloatType = 0x1406;
  constexpr uint32_t kGlTexture0 = 0x84C0;
  constexpr uint32_t kTexCoordArray = 0x8078;
  f.hle.CallArmFunction(f.GlSlot(11), kGlTexture0);
  f.hle.CallArmFunction(f.GlSlot(71), 2, kGlFloatType, 0, kUv);
  f.hle.CallArmFunction(f.GlSlot(29), kTexCoordArray);
  f.hle.CallArmFunction(f.GlSlot(11), kGlTexture0 + 1);
  f.hle.CallArmFunction(f.GlSlot(25), kTexCoordArray);  // desliga SO a unidade 1
  f.hle.CallArmFunction(f.GlSlot(78), 2, kGlFloatType, 0, kPos);
  f.hle.CallArmFunction(f.GlSlot(29), 0x8074);
  f.hle.CallArmFunction(f.GlSlot(26), 0x0004, 0, 3);

  EXPECT_TRUE(f.backend.last_arrays.has_texcoord);
  EXPECT_FALSE(f.backend.last_arrays.has_texcoord1);
}
