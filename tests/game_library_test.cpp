#include "frontends/gui/game_library.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/loader/mif.h"

namespace fs = std::filesystem;
using namespace zeebulator::gui;

namespace {

// Cria uma NAND sintetica. Deliberadamente monta SO o que a descoberta le --
// o teste nao deve depender de .mod real, senao vira teste de corpus.
struct FakeNand {
  fs::path root;
  explicit FakeNand(const std::string& tag) {
    root = fs::temp_directory_path() / ("zeeb_gl_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "mod");
    fs::create_directories(root / "mif");
  }
  ~FakeNand() { std::error_code ec; fs::remove_all(root, ec); }

  void AddMod(const std::string& folder, const std::string& mod_name,
              const std::string& body = "M") {
    fs::create_directories(root / "mod" / folder);
    std::ofstream out(root / "mod" / folder / (mod_name + ".mod"), std::ios::binary);
    out << body;
  }
  void AddFile(const std::string& folder, const std::string& name,
               const std::string& body = "x") {
    std::ofstream out(root / "mod" / folder / name, std::ios::binary);
    out << body;
  }
  // MIF minimo e real na forma: as strings sao UTF-16LE com prefixo 0xFFFE.
  // O extrator do projeto procura exatamente esse padrao, e montar o MIF assim
  // exercita o caminho real de leitura em vez de um atalho.
  void AddMif(const std::string& folder, const std::vector<std::string>& utf8_strings,
              const std::vector<uint32_t>& class_ids = {}) {
    std::vector<uint8_t> b(0x40, 0);
    auto put16 = [&](size_t off, uint16_t v) {
      b[off] = static_cast<uint8_t>(v & 0xff);
      b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    };
    auto put32 = [&](size_t off, uint32_t v) {
      b[off] = static_cast<uint8_t>(v & 0xff);
      b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
      b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
      b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
    };
    put16(0x06, static_cast<uint16_t>(class_ids.size()));
    for (const std::string& s : utf8_strings) {
      b.push_back(0xFF);
      b.push_back(0xFE);
      for (char c : s) {
        b.push_back(static_cast<uint8_t>(c));
        b.push_back(0);
      }
      b.push_back(0);
      b.push_back(0);
    }
    if (!class_ids.empty()) {
      // Tabela de offsets em 0x10 e registros de 8 bytes {ClassID, flags}.
      const uint32_t table_off = 0x10;
      const uint32_t records_off = static_cast<uint32_t>(b.size()) + 0x40;
      for (size_t i = 0; i < class_ids.size(); ++i) {
        const uint32_t rec = records_off + static_cast<uint32_t>(i) * 8;
        put32(table_off + static_cast<size_t>(i) * 4, rec);
      }
      put32(0x14, static_cast<uint32_t>(class_ids.size()));
      while (b.size() < records_off) b.push_back(0);
      for (uint32_t c : class_ids) {
        const size_t at = b.size();
        b.resize(at + 8, 0);
        put32(at, c);
      }
    }
    std::ofstream out(root / "mif" / (folder + ".mif"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }
};

ScanOptions OptionsFor(const FakeNand& nand) {
  ScanOptions o;
  o.nand_root = nand.root.string();
  return o;
}

}  // namespace

TEST(GameLibrary, ListsOnlyFoldersThatContainAMod) {
  FakeNand nand("only_mod");
  nand.AddMod("100", "alpha");
  nand.AddMod("200", "beta");
  // Pasta sem .mod: cache de asset ou sobra. Nao pode virar linha na lista.
  fs::create_directories(nand.root / "mod" / "300");

  const ScanResult r = ScanNand(OptionsFor(nand));
  EXPECT_TRUE(r.root_exists);
  EXPECT_EQ(r.folders_seen, 3u);
  EXPECT_EQ(r.folders_with_mod, 2u);
  ASSERT_EQ(r.entries.size(), 2u);
}

TEST(GameLibrary, MissingNandRootIsReportedNotSilentlyEmpty) {
  ScanOptions o;
  o.nand_root = (fs::temp_directory_path() / "zeeb_nao_existe_12345").string();
  const ScanResult r = ScanNand(o);
  EXPECT_FALSE(r.root_exists);
  EXPECT_FALSE(r.error.empty());
  EXPECT_TRUE(r.entries.empty());
}

TEST(GameLibrary, PickupsGgzAndBarAssetsWhenPresent) {
  FakeNand nand("assets");
  nand.AddMod("100", "alpha");
  nand.AddFile("100", "data.ggz");
  nand.AddFile("100", "sound.ggz");
  nand.AddFile("100", "alpha.bar");

  const ScanResult r = ScanNand(OptionsFor(nand));
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_FALSE(r.entries[0].data_ggz.empty());
  EXPECT_FALSE(r.entries[0].sound_ggz.empty());
  EXPECT_FALSE(r.entries[0].bar.empty());
}

TEST(GameLibrary, NameFallsBackToFolderAndSaysSo) {
  FakeNand nand("no_name");
  nand.AddMod("274214", "cnk2");
  const ScanResult r = ScanNand(OptionsFor(nand));
  ASSERT_EQ(r.entries.size(), 1u);
  // Sem .mif o nome exibido e a pasta, e a PROCEDENCIA tem de dizer isso.
  // Um nome inventado parece confiavel e nao e.
  EXPECT_EQ(r.entries[0].name, "274214");
  EXPECT_EQ(r.entries[0].name_source, NameSource::kFolder);
}

TEST(GameLibrary, NameComesFromMifAndSkipsVersionAndPlatformStrings) {
  FakeNand nand("mif_name");
  nand.AddMod("100", "alpha");
  // Ordem real observada nos MIFs desta NAND: versao primeiro, "Zeebo" depois.
  nand.AddMif("100", {"1.0.696", "Zeebo", "Ridge Racer"});
  const ScanResult r = ScanNand(OptionsFor(nand));
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].name, "Ridge Racer");
  EXPECT_EQ(r.entries[0].name_source, NameSource::kMif);
}

TEST(GameLibrary, ManifestWinsOverMif) {
  FakeNand nand("manifest");
  nand.AddMod("274214", "cnk2");
  nand.AddMif("274214", {"Crash Nitro Kart 2"}, {0x01081984u});
  ScanOptions o = OptionsFor(nand);
  o.manifest_clsids["274214"] = 0x01081985u;  // valor explicito do usuario
  const ScanResult r = ScanNand(o);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].clsid, 0x01081985u);
  EXPECT_EQ(r.entries[0].clsid_source, ClsidSource::kManifest);
}

TEST(GameLibrary, ClsidUnknownIsMarkedNotLaunchableWithAReason) {
  FakeNand nand("unknown");
  nand.AddMod("270001", "sem_nada");  // .mod sem literal de ClsId reconhecivel
  const ScanResult r = ScanNand(OptionsFor(nand));
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].clsid, 0u);
  EXPECT_FALSE(r.entries[0].launchable);
  EXPECT_EQ(r.entries[0].clsid_source, ClsidSource::kUnknown);
  // O motivo precisa ser legivel: "nao iniciavel" mudo foi exatamente o que
  // escondeu 11 titulos dados como mortos por ClsId errado nesta sessao.
  EXPECT_FALSE(r.entries[0].status_reason.empty());
}

TEST(GameLibrary, OrderIsStableAcrossScans) {
  FakeNand nand("order");
  nand.AddMod("300", "c_mod");
  nand.AddMod("100", "a_mod");
  nand.AddMod("200", "b_mod");
  const ScanResult first = ScanNand(OptionsFor(nand));
  const ScanResult second = ScanNand(OptionsFor(nand));
  ASSERT_EQ(first.entries.size(), 3u);
  ASSERT_EQ(second.entries.size(), 3u);
  for (size_t i = 0; i < first.entries.size(); ++i) {
    EXPECT_EQ(first.entries[i].folder, second.entries[i].folder);
  }
  // Ordem alfabetica por nome exibido (aqui, a pasta): a_mod, b_mod, c_mod.
  EXPECT_EQ(first.entries[0].name, "100");
  EXPECT_EQ(first.entries[1].name, "200");
  EXPECT_EQ(first.entries[2].name, "300");
}

TEST(GameLibrary, FilterMatchesNameAndFolderCaseInsensitively) {
  GameEntry e;
  e.name = "Crash Nitro Kart 2";
  e.folder = "274214";
  EXPECT_TRUE(MatchesFilter(e, ""));
  EXPECT_TRUE(MatchesFilter(e, "nitro"));
  EXPECT_TRUE(MatchesFilter(e, "NITRO"));
  EXPECT_TRUE(MatchesFilter(e, "274214"));   // buscar pela pasta e util
  EXPECT_FALSE(MatchesFilter(e, "zenonia"));
}

TEST(GameLibrary, FindsInRangeClsidLiteralComparedRightAfterLoading) {
  // Blob ARM montado a mao: `ldr r1, [pc, #4]` seguido de `cmp r0, r1`, com o
  // literal logo depois. E o padrao medido no despacho do CreateInstance real.
  // Endereco ARM do literal: (pc + 8 + imm); em 0x00100000 com imm=4 da
  // 0x0010000C. Errar essa conta foi o que fez este teste falhar antes.
  std::vector<uint8_t> mod(0x40, 0);
  auto put32 = [&](size_t off, uint32_t v) {
    mod[off] = static_cast<uint8_t>(v & 0xff);
    mod[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    mod[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
    mod[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
  };
  put32(0x00, 0xE59F1004u);  // ldr r1, [pc, #4] -> literal em 0x0010000C
  put32(0x04, 0xE1500001u);  // cmp r0, r1
  put32(0x08, 0xE1A00000u);  // nop
  const uint32_t cls = 0x01081970u;  // faixa BREW real (a3d)
  put32(0x0C, cls);
  const std::vector<uint32_t> found = FindClsidCandidatesInMod(mod);
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(found.front(), cls);
}

TEST(GameLibrary, RejectsFloatingPointConstantsThatLookLikeClsids) {
  // Medido no kh.mod (pasta 11839): a varredura estrita devolvia, alem do ClsId,
  // os doubles 0x3FE921FB (pi/4) e 0x3FD33333 (0.3). Sem guarda, o primeiro
  // candidato escolhido era pi/4. Aqui o blob tem SO uma constante dessas: a
  // lista tem de sair vazia, nao com um falso positivo.
  std::vector<uint8_t> mod(0x40, 0);
  auto put32 = [&](size_t off, uint32_t v) {
    mod[off] = static_cast<uint8_t>(v & 0xff);
    mod[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    mod[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
    mod[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
  };
  put32(0x00, 0xE59F1004u);
  put32(0x04, 0xE1500001u);
  put32(0x0C, 0x3FE921FBu);  // pi/4 em IEEE-754 double
  EXPECT_TRUE(FindClsidCandidatesInMod(mod).empty());
}

TEST(GameLibrary, InRangeCandidatesComeBeforeOutOfRangeOnes) {
  // Ordem medida como necessaria: no 11839 havia candidato na faixa BREW
  // (0x01026191) e constantes fora dela; sem a ordem, o escolhido era pi/4.
  std::vector<uint8_t> mod(0x80, 0);
  auto put32 = [&](size_t off, uint32_t v) {
    mod[off] = static_cast<uint8_t>(v & 0xff);
    mod[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    mod[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
    mod[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
  };
  // Primeira ocorrencia: valor fora da faixa (0x2E6723BD, que nao parece float)
  put32(0x00, 0xE59F1004u);
  put32(0x04, 0xE1500001u);
  put32(0x0C, 0x2E6723BDu);
  // Segunda ocorrencia: valor dentro da faixa BREW
  put32(0x20, 0xE59F1004u);
  put32(0x24, 0xE1500001u);
  put32(0x2C, 0x01026191u);
  const std::vector<uint32_t> found = FindClsidCandidatesInMod(mod);
  ASSERT_GE(found.size(), 2u);
  EXPECT_EQ(found.front(), 0x01026191u);
}

TEST(GameLibrary, ManifestParsesHandEditedJson) {
  const fs::path p = fs::temp_directory_path() /
                     ("zeeb_manifest_" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream out(p);
    out << "{\n  \"games\": {\n"
           "    \"274214\": \"0x1081984\",\n"
           "    \"277455\": 3207913505\n"
           "  }\n}\n";
  }
  const std::map<std::string, uint32_t> m = LoadManifest(p.string());
  std::error_code ec; fs::remove(p, ec);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at("274214"), 0x01081984u);
  EXPECT_EQ(m.at("277455"), 3207913505u);
}

TEST(GameLibrary, MissingManifestIsEmptyAndIsNotAnError) {
  EXPECT_TRUE(LoadManifest("/nonexistent/games.json").empty());
}

// Validacao contra os binarios reais, que e o que importa: o metodo existe
// porque a varredura do .mif falhou nestes titulos. Os valores esperados foram
// lidos no despacho do CreateInstance de cada um. Pula quando a NAND nao esta
// montada, como os outros testes de corpus deste projeto.
namespace {
std::vector<uint8_t> ReadRealModIfPresent(const std::string& folder, const std::string& name) {
  const fs::path root = fs::path("/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand/mod") /
                        folder / (name + ".mod");
  if (!fs::is_regular_file(root)) return {};
  std::ifstream in(root, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}
}  // namespace

TEST(GameLibraryCorpus, FindsTheRealClsidInTheTitlesTheMifScanMissed) {
  struct Case { const char* folder; const char* mod; uint32_t clsid; };
  // So os dois que a varredura estrita ALCANCA. cnk2 e resolvido pelo .mif, e
  // zenonia pelo manifesto -- declarar isso e melhor que afrouxar a heuristica
  // ate ela "achar" os quatro e virar geradora de falso positivo.
  const Case cases[] = {
      {"274259", "a3d", 0x01081970u},
      {"278200", "heavyweaponbrew", 0x010978A2u},
  };
  int checked = 0;
  for (const Case& c : cases) {
    const std::vector<uint8_t> mod = ReadRealModIfPresent(c.folder, c.mod);
    if (mod.empty()) continue;
    ++checked;
    const std::vector<uint32_t> found = FindClsidCandidatesInMod(mod);
    EXPECT_NE(std::find(found.begin(), found.end(), c.clsid), found.end())
        << "nao achou " << std::hex << c.clsid << " em " << c.mod;
    // A lista precisa ser CURTA o bastante para o lancador tentar em ordem.
    EXPECT_LT(found.size(), 64u) << "lista de candidatos grande demais em " << c.mod;
  }
  if (checked == 0) GTEST_SKIP() << "NAND nao montada: nenhum .mod real disponivel";
}

TEST(GameLibraryCorpus, Cnk2ComesFromTheMifNotFromTheModScan) {
  const fs::path mif_path =
      fs::path("/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand/mif/274214.mif");
  if (!fs::is_regular_file(mif_path)) GTEST_SKIP() << "NAND nao montada";
  std::ifstream in(mif_path, std::ios::binary);
  const std::vector<uint8_t> mif((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  ASSERT_FALSE(mif.empty());
  const std::vector<uint32_t> ids =
      zeebulator::ExtractMifClassIds(mif.data(), mif.size());
  EXPECT_NE(std::find(ids.begin(), ids.end(), 0x01081984u), ids.end());
}
