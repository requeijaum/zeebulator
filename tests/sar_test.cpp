// Testes do parser SAR ("SWVARC", containers do chessbots / Superscape).
// O formato foi levantado sobre a NAND de referencia, que NAO pode ser
// commitada (CONTRIBUTING.md, politica de sala limpa), entao os arquivos
// aqui sao sinteticos e montados byte a byte com o layout medido.
//
// Controle negativo obrigatorio (notes/MORE_INFO.md 7): lixo aleatorio,
// magico errado, arquivo truncado, offsets fora do arquivo e sobra de bytes
// no fim precisam ser recusados devolvendo std::nullopt -- sem excecao
// vazando para quem chama e sem falso positivo.

#include "core/loader/sar.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::SarArchive;
using zeebulator::SarEntry;

namespace {

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

struct SyntheticFile {
  std::string name;
  std::vector<uint8_t> payload;
};

// Monta um SAR bem formado: 12 bytes de assinatura, cabecalho de 0x25 bytes,
// um registro por entrada, um byte 0x00 fechando o indice e os payloads
// contiguos ate o fim do arquivo.
std::vector<uint8_t> BuildSar(const std::vector<SyntheticFile>& files) {
  std::vector<uint8_t> index;
  uint64_t payload_bytes = 0;
  for (const auto& f : files) {
    AppendU32LE(index, static_cast<uint32_t>(21 + f.name.size()));  // tamanho do registro
    AppendU32LE(index, 0x4ad58b60u);                                // timestamp
    AppendU32LE(index, static_cast<uint32_t>(f.payload.size()));
    AppendU32LE(index, 0xdeadbeefu);  // hash do nome
    index.insert(index.end(), f.name.begin(), f.name.end());
    index.push_back(0x00);
    AppendU32LE(index, 0xfeedfaceu);  // hash do conteudo
    payload_bytes += f.payload.size();
  }
  const uint32_t data_offset = static_cast<uint32_t>(0x25 + index.size() + 1);

  std::vector<uint8_t> out = {0xAB, 'S', 'W', 'V', 'A', 'R', 'C', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  out.push_back(0x00);                                                    // 0x0C versao
  AppendU32LE(out, data_offset);                                          // 0x0D
  AppendU32LE(out, static_cast<uint32_t>(data_offset + payload_bytes));   // 0x11 total
  AppendU32LE(out, 0x4ad58bf6u);                                          // 0x15
  AppendU32LE(out, 0x4ad58bf6u);                                          // 0x19
  AppendU32LE(out, 0x78c81390u);                                          // 0x1D
  AppendU32LE(out, static_cast<uint32_t>(files.size()));                  // 0x21
  out.insert(out.end(), index.begin(), index.end());
  out.push_back(0x00);  // terminador do indice
  for (const auto& f : files) out.insert(out.end(), f.payload.begin(), f.payload.end());
  return out;
}

// Aproxima o corpus real: um PNG, um M3G e um .asc, como em background14.sar.
std::vector<SyntheticFile> SampleFiles() {
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.resize(64, 0x11);
  std::vector<uint8_t> m3g = {0xAB, 'J', 'S', 'R', '1', '8', '4', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  m3g.resize(40, 0x22);
  std::vector<uint8_t> asc(17, 0x33);
  return {{"background140.png", png}, {"board5.m3g", m3g}, {"board5.asc", asc}};
}

void PatchU32LE(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

// --- caminho feliz ---------------------------------------------------

TEST(SarArchive, ParsesIndexAndPayloads) {
  const auto files = SampleFiles();
  auto archive = SarArchive::Parse(BuildSar(files));
  ASSERT_TRUE(archive.has_value());
  ASSERT_EQ(archive->Entries().size(), files.size());

  // Payloads contiguos a partir do offset declarado em 0x0D.
  uint32_t expected_offset = archive->Entries()[0].payload_offset;
  for (size_t i = 0; i < files.size(); ++i) {
    const SarEntry& e = archive->Entries()[i];
    EXPECT_EQ(e.name, files[i].name);
    EXPECT_EQ(e.payload_size, files[i].payload.size());
    EXPECT_EQ(e.payload_offset, expected_offset);
    EXPECT_EQ(e.name_hash, 0xdeadbeefu);
    EXPECT_EQ(e.content_hash, 0xfeedfaceu);
    auto blob = archive->Extract(e);
    ASSERT_TRUE(blob.has_value());
    EXPECT_EQ(*blob, files[i].payload);
    expected_offset += e.payload_size;
  }
  EXPECT_EQ(archive->Timestamp(), 0x4ad58bf6u);
  EXPECT_EQ(archive->HeaderHash(), 0x78c81390u);
}

TEST(SarArchive, FindsEntryByName) {
  auto archive = SarArchive::Parse(BuildSar(SampleFiles()));
  ASSERT_TRUE(archive.has_value());
  const SarEntry* e = archive->Find("board5.m3g");
  ASSERT_NE(e, nullptr);
  auto blob = archive->Extract(*e);
  ASSERT_TRUE(blob.has_value());
  ASSERT_GE(blob->size(), 12u);
  EXPECT_EQ((*blob)[0], 0xAB);  // assinatura M3G/JSR184
  EXPECT_EQ((*blob)[1], 'J');
  EXPECT_EQ(archive->Find("nao_existe.png"), nullptr);
}

// Um SAR de entrada unica e o caso mais comum do corpus (ans2.sar, en_main.sar).
TEST(SarArchive, ParsesSingleEntryArchive) {
  auto archive = SarArchive::Parse(BuildSar({{"ans2.asc", std::vector<uint8_t>(654, 0x5A)}}));
  ASSERT_TRUE(archive.has_value());
  ASSERT_EQ(archive->Entries().size(), 1u);
  EXPECT_EQ(archive->Entries()[0].name, std::string("ans2.asc"));
  EXPECT_EQ(archive->Entries()[0].payload_size, 654u);
}

// --- controle negativo -----------------------------------------------

// Bytes aleatorios nao podem virar um container valido.
TEST(SarNegative, RejectsRandomBytes) {
  std::vector<uint8_t> junk;
  uint32_t state = 12345;
  for (int i = 0; i < 8192; ++i) {
    state = state * 1103515245u + 12345u;
    junk.push_back(static_cast<uint8_t>(state >> 16));
  }
  ASSERT_FALSE(SarArchive::Parse(junk).has_value());
}

// Lixo aleatorio DEPOIS de uma assinatura valida: e o teste que pega um
// parser que so olha o magico.
TEST(SarNegative, RejectsValidMagicWithRandomTail) {
  std::vector<uint8_t> junk = {0xAB, 'S', 'W', 'V', 'A', 'R', 'C', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  uint32_t state = 999;
  for (int i = 0; i < 4096; ++i) {
    state = state * 1103515245u + 12345u;
    junk.push_back(static_cast<uint8_t>(state >> 16));
  }
  ASSERT_FALSE(SarArchive::Parse(junk).has_value());
}

TEST(SarNegative, RejectsWrongMagic) {
  auto data = BuildSar(SampleFiles());
  data[1] = 'X';  // "SWVARC" -> "XWVARC"
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsCorruptedLineEndingGuard) {
  // O 0D 0A 1A 0A existe para denunciar transferencia em modo texto: se o
  // 0D some, o arquivo esta corrompido e nao pode ser aceito.
  auto data = BuildSar(SampleFiles());
  data.erase(data.begin() + 8);
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsUnknownVersionByte) {
  auto data = BuildSar(SampleFiles());
  data[0x0c] = 0x01;  // 0x00 nos 53 arquivos medidos
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsEmptyInput) {
  ASSERT_FALSE(SarArchive::Parse(std::vector<uint8_t>{}).has_value());
  ASSERT_FALSE(SarArchive::Parse(std::vector<uint8_t>(12, 0)).has_value());
}

// Cabecalho completo mas sem nenhum byte de indice.
TEST(SarNegative, RejectsHeaderOnly) {
  auto data = BuildSar(SampleFiles());
  data.resize(0x25);
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsTruncatedFile) {
  auto full = BuildSar(SampleFiles());
  // Corta em varios pontos: no indice, na fronteira e no meio do payload.
  for (size_t cut : {size_t(20), size_t(40), size_t(60), size_t(90), size_t(150)}) {
    if (cut >= full.size()) continue;
    std::vector<uint8_t> data(full.begin(), full.begin() + cut);
    ASSERT_FALSE(SarArchive::Parse(data).has_value()) << "corte em " << cut;
  }
  // E o corte de um unico byte no fim: o tamanho declarado em 0x11 deixa de bater.
  auto one_short = full;
  one_short.pop_back();
  ASSERT_FALSE(SarArchive::Parse(one_short).has_value());
}

TEST(SarNegative, RejectsTrailingGarbage) {
  auto data = BuildSar(SampleFiles());
  data.push_back(0x99);
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsDataOffsetOutsideFile) {
  auto data = BuildSar(SampleFiles());
  PatchU32LE(data, 0x0d, static_cast<uint32_t>(data.size() + 4096));
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
  PatchU32LE(data, 0x0d, 0xffffffffu);
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
  PatchU32LE(data, 0x0d, 0);  // antes do fim do cabecalho
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsWrongDeclaredTotalSize) {
  auto data = BuildSar(SampleFiles());
  PatchU32LE(data, 0x11, static_cast<uint32_t>(data.size() - 1));
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
  PatchU32LE(data, 0x11, 0xffffffffu);
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

TEST(SarNegative, RejectsWrongEntryCount) {
  auto good = BuildSar(SampleFiles());
  for (uint32_t count : {0u, 1u, 2u, 4u, 100u, 0xffffffffu}) {
    auto data = good;
    PatchU32LE(data, 0x21, count);
    ASSERT_FALSE(SarArchive::Parse(data).has_value()) << "count=" << count;
  }
}

// Um tamanho de payload absurdo faria a area de dados sair do arquivo.
TEST(SarNegative, RejectsPayloadSizeOutsideFile) {
  auto data = BuildSar(SampleFiles());
  PatchU32LE(data, 0x25 + 8, 0xfffffff0u);  // tamanho da 1a entrada
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

// Payloads que nao cobrem o resto do arquivo (buraco entre entradas).
TEST(SarNegative, RejectsPayloadsThatDoNotFillTheFile) {
  auto data = BuildSar(SampleFiles());
  PatchU32LE(data, 0x25 + 8, 1);  // 1a entrada encolhe, sobra byte no fim
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

// Tamanho de registro que nao bate com o nome gravado: o NUL deixa de cair
// no lugar exato e a cadeia de registros para fora do terminador.
TEST(SarNegative, RejectsInconsistentRecordSize) {
  auto good = BuildSar(SampleFiles());
  for (int delta : {-2, -1, 1, 2, 1000}) {
    auto data = good;
    const uint32_t rec = static_cast<uint32_t>(21 + std::string("background140.png").size());
    PatchU32LE(data, 0x25, static_cast<uint32_t>(rec + delta));
    ASSERT_FALSE(SarArchive::Parse(data).has_value()) << "delta=" << delta;
  }
  auto zeroed = good;
  PatchU32LE(zeroed, 0x25, 0);
  ASSERT_FALSE(SarArchive::Parse(zeroed).has_value());
}

// Nome com bytes binarios nao e um nome de asset.
TEST(SarNegative, RejectsBinaryName) {
  auto data = BuildSar(SampleFiles());
  data[0x25 + 16] = 0x01;
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

// O byte 0x00 entre o fim do indice e a area de payloads e obrigatorio:
// medido em todos os 53 arquivos do corpus.
TEST(SarNegative, RejectsMissingIndexTerminator) {
  auto data = BuildSar(SampleFiles());
  const uint32_t data_offset = static_cast<uint32_t>(data[0x0d]) |
                               (static_cast<uint32_t>(data[0x0e]) << 8) |
                               (static_cast<uint32_t>(data[0x0f]) << 16) |
                               (static_cast<uint32_t>(data[0x10]) << 24);
  data[data_offset - 1] = 0x42;
  ASSERT_FALSE(SarArchive::Parse(data).has_value());
}

// Extract com uma entrada forjada (offset fora do arquivo) devolve nullopt
// em vez de ler fora dos limites.
TEST(SarNegative, ExtractRejectsForgedEntry) {
  auto archive = SarArchive::Parse(BuildSar(SampleFiles()));
  ASSERT_TRUE(archive.has_value());
  SarEntry forged = archive->Entries()[0];
  forged.payload_offset = 0xfffffff0u;
  forged.payload_size = 64;
  EXPECT_FALSE(archive->Extract(forged).has_value());
}

// --- corpus real, opcional -------------------------------------------

// Roda so quando ZEEB_SAR_CORPUS aponta para uma pasta com .sar reais (a NAND
// de referencia nunca entra no repositorio). Confere os mesmos numeros citados
// em core/loader/sar.h: todo arquivo e consumido por inteiro e os payloads
// carregam assinaturas conhecidas.
TEST(SarCorpus, ParsesEveryRealSarWhenTheCorpusIsAvailable) {
  const char* root = std::getenv("ZEEB_SAR_CORPUS");
  if (root == nullptr) GTEST_SKIP() << "ZEEB_SAR_CORPUS nao definido";

  namespace fs = std::filesystem;
  size_t archives = 0, entries = 0, known_signature = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    const std::string path = e.path().string();
    if (path.size() < 4 || path.find(".sar") == std::string::npos) continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    const size_t file_size = bytes.size();
    auto archive = SarArchive::Parse(std::move(bytes));
    ASSERT_TRUE(archive.has_value()) << path;
    ++archives;
    uint64_t covered = archive->Entries().empty() ? 0 : archive->Entries()[0].payload_offset;
    for (const auto& entry : archive->Entries()) {
      ++entries;
      EXPECT_EQ(entry.payload_offset, covered) << path << " / " << entry.name;
      covered += entry.payload_size;
      auto blob = archive->Extract(entry);
      ASSERT_TRUE(blob.has_value());
      ASSERT_EQ(blob->size(), entry.payload_size);
      EXPECT_EQ(archive->Find(entry.name)->payload_offset, entry.payload_offset);
      if (blob->size() >= 12) {
        const uint8_t* p = blob->data();
        if (std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 ||
            std::memcmp(p, "\xabJSR184\xbb\r\n\x1a\n", 12) == 0 ||
            std::memcmp(p, "cmid", 4) == 0 || std::memcmp(p, "MThd", 4) == 0 ||
            std::memcmp(p, "RIFF", 4) == 0) {
          ++known_signature;
        }
      }
    }
    EXPECT_EQ(covered, file_size) << path;  // arquivo consumido por completo
  }
  std::printf("SAR: %zu arquivos, %zu entradas, %zu com assinatura conhecida\n", archives,
              entries, known_signature);
  EXPECT_GT(archives, 0u);
}
