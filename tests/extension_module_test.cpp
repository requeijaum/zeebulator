// Carregamento de MODULO DE EXTENSAO BREW, ponta a ponta, dentro do
// interpretador: catalogo -> LoadMod -> AEEMod_Load -> IModule::CreateInstance.
//
// O modulo de extensao deste teste e ARM de verdade, montado a mao (o mesmo
// contrato que o .mod real cumpre), nao um mock em C++: o objetivo e provar que
// o caminho entra no convidado e volta com o ponteiro que o codigo do modulo
// escreveu.

#include "core/brew/extension_module.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

namespace {

namespace fs = std::filesystem;
using zeebulator::ArmInterpreter;
using zeebulator::BrewExtensionLoader;
using zeebulator::HleRuntime;

constexpr uint32_t kBase = zeebulator::kExtensionModuleBase;
constexpr uint32_t kAcceptedCls = 0x010292c3u;   // a classe do imicro3d, de verdade
constexpr uint32_t kRefusedCls = 0x01ffffffu;    // declarada no .mif, recusada pelo modulo

void Put32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (i * 8));
}

// .mod de extensao sintetico, posicao fixa (carregado em kBase).
//
//   0x00 AEEMod_Load(r0=shell, r1=ph, r2=ppMod): *ppMod = kBase+0x40; return 0
//   0x40 IModule       { vtable = kBase+0x50 }
//   0x50 vtable IModule: AddRef, Release, CreateInstance(kBase+0x80), FreeResources
//   0x80 CreateInstance(r0=pMod, r1=shell, r2=cls, r3=ppObj):
//          se cls == kAcceptedCls -> *ppObj = kBase+0xc0; return 0
//          senao                  -> *ppObj = 0;          return 3 (ECLASSNOTSUPPORT)
//   0xc0 objeto        { vtable = kBase+0xd0 }
std::vector<uint8_t> BuildSyntheticExtensionMod() {
  std::vector<uint8_t> m(0xE0, 0);
  // AEEMod_Load
  Put32(m, 0x00, 0xE59F3028);  // ldr r3, [pc, #0x28]  -> literal em 0x30
  Put32(m, 0x04, 0xE5823000);  // str r3, [r2]
  Put32(m, 0x08, 0xE3A00000);  // mov r0, #0
  Put32(m, 0x0C, 0xE12FFF1E);  // bx lr
  Put32(m, 0x30, kBase + 0x40);
  // IModule + vtable
  Put32(m, 0x40, kBase + 0x50);
  Put32(m, 0x58, kBase + 0x80);  // slot 2 = CreateInstance
  // CreateInstance
  Put32(m, 0x80, 0xE59FC028);  // ldr r12, [pc, #0x28] -> literal em 0xb0
  Put32(m, 0x84, 0xE152000C);  // cmp r2, r12
  Put32(m, 0x88, 0x1A000004);  // bne 0xa0
  Put32(m, 0x8C, 0xE59F0020);  // ldr r0, [pc, #0x20]  -> literal em 0xb4
  Put32(m, 0x90, 0xE5830000);  // str r0, [r3]
  Put32(m, 0x94, 0xE3A00000);  // mov r0, #0
  Put32(m, 0x98, 0xE12FFF1E);  // bx lr
  Put32(m, 0xA0, 0xE3A00000);  // mov r0, #0
  Put32(m, 0xA4, 0xE5830000);  // str r0, [r3]   (*ppObj = 0)
  Put32(m, 0xA8, 0xE3A00003);  // mov r0, #3     (ECLASSNOTSUPPORT)
  Put32(m, 0xAC, 0xE12FFF1E);  // bx lr
  Put32(m, 0xB0, kAcceptedCls);
  Put32(m, 0xB4, kBase + 0xC0);
  // objeto devolvido
  Put32(m, 0xC0, kBase + 0xD0);
  return m;
}

// .mif de extensao: sem registro de applet, com os registros de 8 bytes.
std::vector<uint8_t> BuildExtensionMif(const std::vector<uint32_t>& classes) {
  const uint32_t table_offset = 0x20;
  const uint32_t count = static_cast<uint32_t>(classes.size());
  const uint32_t data_offset = table_offset + (count + 1) * 4;
  std::vector<uint8_t> d(data_offset, 0);
  const uint8_t magic[6] = {0x11, 0x00, 0x01, 0x00, 0x01, 0x00};
  for (int i = 0; i < 6; ++i) d[i] = magic[i];
  Put32(d, 0x10, table_offset);
  Put32(d, 0x14, count);
  Put32(d, 0x18, data_offset);
  uint32_t cursor = data_offset;
  for (uint32_t i = 0; i < count; ++i) {
    Put32(d, table_offset + i * 4, cursor);
    d.resize(d.size() + 8, 0);
    Put32(d, cursor, classes[i]);
    cursor += 8;
  }
  Put32(d, table_offset + count * 4, cursor);
  return d;
}

void WriteFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// NAND sintetica: <root>/mif/4242.mif + <root>/mod/4242/fake_ext.mod
class FakeNand {
 public:
  FakeNand() {
    std::random_device rd;
    root_ = fs::temp_directory_path() /
            ("zeebulator_ext_test_" + std::to_string(rd()));
    WriteFile(root_ / "mif" / "4242.mif", BuildExtensionMif({kAcceptedCls, kRefusedCls}));
    WriteFile(root_ / "mod" / "4242" / "fake_ext.mod", BuildSyntheticExtensionMod());
  }
  ~FakeNand() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

}  // namespace

TEST(BrewExtensionLoaderTest, CatalogoAchaAsClassesDoMifSemApplet) {
  FakeNand nand;
  auto entries = zeebulator::ScanBrewExtensionCatalog(nand.root().string());
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].cls_id, kAcceptedCls);
  EXPECT_EQ(entries[1].cls_id, kRefusedCls);
  EXPECT_NE(entries[0].mod_path.find("fake_ext.mod"), std::string::npos);
}

TEST(BrewExtensionLoaderTest, CarregaOModuloEDevolveOObjetoQueOConvidadoEscreveu) {
  FakeNand nand;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  BrewExtensionLoader loader(cpu, hle);
  loader.SetShellPointer(0x80001000);
  loader.SetStaticBaseTable(0x80280000);
  ASSERT_EQ(loader.ScanNandRoot(nand.root().string()), 2u);
  EXPECT_TRUE(loader.Provides(kAcceptedCls));
  EXPECT_FALSE(loader.Provides(0x01001001u));

  // Uma pilha valida: o modulo e chamado como qualquer codigo do convidado.
  cpu.SetRegister(zeebulator::kSP, 0x00380000);
  const uint32_t obj = loader.CreateInstance(kAcceptedCls);
  ASSERT_NE(obj, 0u);
  EXPECT_EQ(obj, kBase + 0xC0) << "o ponteiro tem de vir do proprio modulo";
  EXPECT_EQ(cpu.GetMemory().Read32(obj), kBase + 0xD0) << "e a vtable dele tambem";

  // O static base ROPI tem de estar em base-4, senao o codigo compilado de
  // verdade nao acha a stdlib do BREW.
  EXPECT_EQ(cpu.GetMemory().Read32(kBase - 4), 0x80280000u);
}

// Falha honesta: a classe esta declarada no .mif, mas o modulo recusa.
TEST(BrewExtensionLoaderTest, ClasseRecusadaPeloModuloDevolveZero) {
  FakeNand nand;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  BrewExtensionLoader loader(cpu, hle);
  loader.SetShellPointer(0x80001000);
  loader.SetStaticBaseTable(0x80280000);
  ASSERT_EQ(loader.ScanNandRoot(nand.root().string()), 2u);
  cpu.SetRegister(zeebulator::kSP, 0x00380000);
  EXPECT_EQ(loader.CreateInstance(kRefusedCls), 0u);
}

TEST(BrewExtensionLoaderTest, ClasseSemFornecedorDevolveZeroSemCarregarNada) {
  FakeNand nand;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  BrewExtensionLoader loader(cpu, hle);
  loader.SetShellPointer(0x80001000);
  loader.SetStaticBaseTable(0x80280000);
  loader.ScanNandRoot(nand.root().string());
  EXPECT_EQ(loader.CreateInstance(0x0100deadu), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(kBase), 0u) << "nada foi carregado";
}

// O contexto do convidado NAO pode mudar: a carga acontece de dentro de um
// handler de HLE, com o jogo parado no meio de uma chamada. E a regra de
// reentrancia do projeto (CallArmFunctionPreservingContext).
TEST(BrewExtensionLoaderTest, PreservaORegistradorDoConvidado) {
  FakeNand nand;
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x10000);
  BrewExtensionLoader loader(cpu, hle);
  loader.SetShellPointer(0x80001000);
  loader.SetStaticBaseTable(0x80280000);
  loader.ScanNandRoot(nand.root().string());

  for (int r = 0; r < 13; ++r) cpu.SetRegister(r, 0xA0000000u + static_cast<uint32_t>(r));
  cpu.SetRegister(zeebulator::kSP, 0x00380000);
  cpu.SetRegister(zeebulator::kLR, 0x00123456);
  cpu.SetRegister(zeebulator::kPC, 0x00104000);
  const uint32_t saved_cpsr = cpu.GetCpsr();

  ASSERT_NE(loader.CreateInstance(kAcceptedCls), 0u);

  for (int r = 0; r < 13; ++r) {
    EXPECT_EQ(cpu.GetRegister(r), 0xA0000000u + static_cast<uint32_t>(r)) << "r" << r;
  }
  EXPECT_EQ(cpu.GetRegister(zeebulator::kSP), 0x00380000u);
  EXPECT_EQ(cpu.GetRegister(zeebulator::kLR), 0x00123456u);
  EXPECT_EQ(cpu.GetRegister(zeebulator::kPC), 0x00104000u);
  EXPECT_EQ(cpu.GetCpsr(), saved_cpsr);
}

// ARRANJO DO PACOTE BAIXADO: a extensao e uma pasta irma do jogo, com o .mif de
// mesmo nome ao lado dela -- medido no Kingdom Hearts:
//   <jogo>/Kingdon Hearts/kh.mod          + <jogo>/Kingdon Hearts.mif   (applet)
//   <jogo>/Kingdon Hearts_/swv21brew.mod  + <jogo>/Kingdon Hearts_.mif  (extensao)
TEST(BrewExtensionLoaderTest, AchaExtensaoNoArranjoDePacoteBaixado) {
  std::random_device rd;
  const fs::path root = fs::temp_directory_path() /
                        ("zeebulator_ext_pkg_" + std::to_string(rd()));
  WriteFile(root / "Jogo" / "jogo.mod", std::vector<uint8_t>(16, 0));
  WriteFile(root / "Jogo.mif", BuildExtensionMif({}));  // sem classe: nao e extensao
  WriteFile(root / "Jogo_" / "ext.mod", BuildSyntheticExtensionMod());
  WriteFile(root / "Jogo_.mif", BuildExtensionMif({kAcceptedCls}));

  auto entries = zeebulator::ScanBrewExtensionsForModule((root / "Jogo" / "jogo.mod").string());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].cls_id, kAcceptedCls);
  EXPECT_NE(entries[0].mod_path.find("ext.mod"), std::string::npos);

  std::error_code ec;
  fs::remove_all(root, ec);
}

// ARRANJO DA NAND: <raiz>/mod/<pasta>/x.mod com <raiz>/mif/<pasta>.mif.
TEST(BrewExtensionLoaderTest, AchaExtensaoNoArranjoDaNand) {
  FakeNand nand;
  WriteFile(nand.root() / "mod" / "9999" / "jogo.mod", std::vector<uint8_t>(16, 0));
  auto entries = zeebulator::ScanBrewExtensionsForModule(
      (nand.root() / "mod" / "9999" / "jogo.mod").string());
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].cls_id, kAcceptedCls);
}

// ARQUIVOS REAIS do Kingdom Hearts, se estiverem por perto: o .mif da extensao
// fornece 0x0102bbfc, exatamente a classe que o kh.mod pede em execucao, e o
// .mif do jogo declara o applet 0x01026191 e nao fornece nada.
TEST(BrewExtensionLoaderTest, ArquivosReaisDoKingdomHearts) {
  fs::path dir = "/home/rafaelfrequiao/Downloads/Kingdon Hearts/Kingdon Hearts";
  if (const char* env = std::getenv("ZEEB_KH_DIR")) dir = fs::path(env);
  if (!fs::exists(dir / "kh.mod")) GTEST_SKIP() << "pacote do KH ausente: " << dir;
  auto entries = zeebulator::ScanBrewExtensionsForModule((dir / "kh.mod").string());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].cls_id, 0x0102bbfcu);
  EXPECT_NE(entries[0].mod_path.find("swv21brew.mod"), std::string::npos);
}
