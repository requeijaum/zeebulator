// Formato do registro de EXTENSAO do .mif -- medido, nao suposto.
//
// O que estes testes provam, e que nenhum outro teste do projeto provava: a
// diferenca entre "este .mif FORNECE a classe" e "este .mif PEDE a classe". Os
// dois casos gravam o MESMO registro de 8 bytes {ClassID, 0}; o que muda e a
// presenca do registro de applet. Ver core/loader/mif.h para a medicao inteira.

#include "core/loader/mif.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

// Raiz do corpus real (debug_nand). Mesma convencao dos outros testes que usam
// arquivo real: variavel de ambiente com um padrao, e SKIP se nao existir.
fs::path CorpusRoot() {
  if (const char* env = std::getenv("ZEEB_NAND_CORPUS")) return fs::path(env);
  return fs::path("/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand");
}

std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

void Put32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (i * 8));
}

// Monta um .mif com a geometria medida: cabecalho, indice vazio e uma tabela de
// N+1 offsets delimitando as secoes dadas.
std::vector<uint8_t> BuildMifWithSections(const std::vector<std::vector<uint8_t>>& sections) {
  const uint32_t table_offset = 0x20;
  const uint32_t count = static_cast<uint32_t>(sections.size());
  const uint32_t data_offset = table_offset + (count + 1) * 4;
  std::vector<uint8_t> d(data_offset, 0);
  const uint8_t magic[6] = {0x11, 0x00, 0x01, 0x00, 0x01, 0x00};
  for (int i = 0; i < 6; ++i) d[i] = magic[i];
  Put32(d, 0x08, table_offset);  // offset do indice (vazio)
  Put32(d, 0x0c, 0);             // tamanho do indice
  Put32(d, 0x10, table_offset);
  Put32(d, 0x14, count);
  Put32(d, 0x18, data_offset);
  uint32_t cursor = data_offset;
  for (uint32_t i = 0; i < count; ++i) {
    Put32(d, table_offset + i * 4, cursor);
    d.insert(d.end(), sections[i].begin(), sections[i].end());
    cursor += static_cast<uint32_t>(sections[i].size());
  }
  Put32(d, table_offset + count * 4, cursor);  // o offset extra fecha a ultima
  Put32(d, 0x1c, cursor - data_offset);
  return d;
}

std::vector<uint8_t> ClassRecord(uint32_t cls, uint32_t flags = 0) {
  std::vector<uint8_t> r(8, 0);
  Put32(r, 0, cls);
  Put32(r, 4, flags);
  return r;
}

std::vector<uint8_t> AppletRecord(uint32_t cls) {
  // 20 bytes: ClassID, zero, um numero, zero, um campo que varia -- a forma
  // medida em 62 dos 63 .mif do corpus.
  std::vector<uint8_t> r(20, 0);
  Put32(r, 0, cls);
  Put32(r, 8, 0x14);
  Put32(r, 16, 0x00100000);
  return r;
}

}  // namespace

// Sem applet: os registros de 8 bytes sao classes FORNECIDAS.
TEST(MifExtensionTest, MifSemAppletFornececlasse) {
  auto d = BuildMifWithSections({ClassRecord(0x010292c3u)});
  EXPECT_TRUE(zeebulator::ExtractMifAppletClassIds(d.data(), d.size()).empty());
  auto provided = zeebulator::ExtractMifExtensionClassIds(d.data(), d.size());
  ASSERT_EQ(provided.size(), 1u);
  EXPECT_EQ(provided[0], 0x010292c3u);
}

// COM applet: o MESMO registro de 8 bytes e dependencia, nao fornecimento.
TEST(MifExtensionTest, MifComAppletNaoFornececlasse) {
  auto d = BuildMifWithSections({AppletRecord(0x01081970u), ClassRecord(0x010292c3u)});
  auto applets = zeebulator::ExtractMifAppletClassIds(d.data(), d.size());
  ASSERT_EQ(applets.size(), 1u);
  EXPECT_EQ(applets[0], 0x01081970u);
  EXPECT_TRUE(zeebulator::ExtractMifExtensionClassIds(d.data(), d.size()).empty());
}

// Uma string UTF-16 de 8 bytes ("1.0" com BOM) tambem e uma secao de 8 bytes.
// Ela nao pode virar ClassID: flags != 0 e o valor fica fora da faixa.
TEST(MifExtensionTest, StringDeOitoBytesNaoViraClasse) {
  std::vector<uint8_t> str8 = {0xff, 0xfe, 0x31, 0x00, 0x2e, 0x00, 0x30, 0x00};
  auto d = BuildMifWithSections({str8});
  EXPECT_TRUE(zeebulator::ExtractMifExtensionClassIds(d.data(), d.size()).empty());
}

// CONTROLE NEGATIVO: sem o magico, nada sai.
TEST(MifExtensionTest, RecusaMagicoErrado) {
  auto d = BuildMifWithSections({ClassRecord(0x010292c3u)});
  d[0] = 0x12;
  EXPECT_TRUE(zeebulator::ExtractMifExtensionClassIds(d.data(), d.size()).empty());
  EXPECT_TRUE(zeebulator::ExtractMifSections(d.data(), d.size()).empty());
}

// ARQUIVO REAL: 12875.mif (o .mif do imicro3d) fornece 0x010292c3.
TEST(MifExtensionTest, ArquivoReal12875FornecImicro3d) {
  const fs::path p = CorpusRoot() / "mif" / "12875.mif";
  if (!fs::exists(p)) GTEST_SKIP() << "corpus ausente: " << p;
  const auto d = ReadAll(p);
  ASSERT_FALSE(d.empty());
  EXPECT_TRUE(zeebulator::ExtractMifAppletClassIds(d.data(), d.size()).empty())
      << "12875.mif e o unico .mif do corpus sem applet";
  auto provided = zeebulator::ExtractMifExtensionClassIds(d.data(), d.size());
  ASSERT_EQ(provided.size(), 1u);
  EXPECT_EQ(provided[0], 0x010292c3u);
}

// ARQUIVO REAL, O CASO QUE TEM DE SER DIFERENTE: 274259.mif (Action Hero 3D)
// contem o MESMO registro {0x010292c3, 0}, mas e um jogo -- ele PEDE a classe.
TEST(MifExtensionTest, ArquivoReal274259NaoFornecNadaApesarDoRegistroIgual) {
  const fs::path p = CorpusRoot() / "mif" / "274259.mif";
  if (!fs::exists(p)) GTEST_SKIP() << "corpus ausente: " << p;
  const auto d = ReadAll(p);
  ASSERT_FALSE(d.empty());
  auto applets = zeebulator::ExtractMifAppletClassIds(d.data(), d.size());
  ASSERT_EQ(applets.size(), 1u);
  EXPECT_EQ(applets[0], 0x01081970u) << "ClassID do a3d, ja conhecido por outra fonte";
  EXPECT_TRUE(zeebulator::ExtractMifExtensionClassIds(d.data(), d.size()).empty());

  // E o registro de 8 bytes esta mesmo la: o teste acima so vale se os dois
  // arquivos forem byte-a-byte iguais nesse ponto.
  bool tem_registro = false;
  for (const auto& s : zeebulator::ExtractMifSections(d.data(), d.size())) {
    if (s.size != 8) continue;
    uint32_t cls = 0;
    for (int i = 3; i >= 0; --i) cls = (cls << 8) | d[s.offset + i];
    if (cls == 0x010292c3u) tem_registro = true;
  }
  EXPECT_TRUE(tem_registro) << "a3d.mif tem o registro de 0x010292c3 (a dependencia)";
}

// CORPUS INTEIRO: os dois papeis sao mutuamente exclusivos, e a grande maioria
// dos .mif e de applet.
//
// Sem contagem fixa de proposito: o corpus e uma pasta viva (durante este
// trabalho o usuario copiou o pacote do Kingdom Hearts para dentro dele, e o
// numero de .mif mudou de 63 para 65 no meio da medicao). O que o formato
// garante -- e o que este teste cobra -- e a exclusao mutua entre "declara
// applet" e "fornece classe", mais os dois casos conhecidos por outra fonte.
TEST(MifExtensionTest, CorpusInteiroSeparaAppletDeExtensao) {
  const fs::path dir = CorpusRoot() / "mif";
  if (!fs::exists(dir)) GTEST_SKIP() << "corpus ausente: " << dir;
  int com_applet = 0;
  std::vector<std::string> extensoes;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".mif") continue;
    const auto d = ReadAll(entry.path());
    if (d.size() < 6 || d[0] != 0x11 || d[1] != 0x00) continue;  // 11839.mif e cifrado
    const auto applets = zeebulator::ExtractMifAppletClassIds(d.data(), d.size());
    const auto provided = zeebulator::ExtractMifExtensionClassIds(d.data(), d.size());
    EXPECT_TRUE(applets.empty() || provided.empty())
        << entry.path() << " nao pode ser applet e extensao ao mesmo tempo";
    if (!applets.empty()) {
      ++com_applet;
    } else if (!provided.empty()) {
      extensoes.push_back(entry.path().stem().string());
    }
  }
  EXPECT_GT(com_applet, 50) << "o corpus e quase todo de jogos";
  ASSERT_FALSE(extensoes.empty());
  EXPECT_NE(std::find(extensoes.begin(), extensoes.end(), "12875"), extensoes.end())
      << "12875 (imicro3d) tem de aparecer como extensao";
  EXPECT_LT(extensoes.size(), 5u) << "extensao e excecao no corpus, nao regra";
}
