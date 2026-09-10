// Testes do parser FUFS (".vfs"). Como todo container do projeto, o
// formato foi levantado sobre dumps reais que NAO podem ser commitados
// (CONTRIBUTING.md, politica de sala limpa), entao os arquivos aqui sao
// sinteticos e montados byte a byte com o layout medido -- inclusive as
// duas copias da lista de nomes (texto puro + rodape zlib).
//
// Os vetores de hash abaixo, esses sim, sao medidos: sao caminhos reais
// de jogos reais com o valor que esta gravado na tabela do .vfs
// correspondente na NAND de referencia.
//
// Controle negativo obrigatorio (notes/MORE_INFO.md 7): lixo aleatorio,
// cabecalho corrompido e arquivo truncado precisam ser recusados de forma
// limpa -- devolvendo std::nullopt, sem excecao e sem falso positivo.

#include "core/loader/fufs.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::FufsArchive;
using zeebulator::FufsEntry;

namespace {

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out(compressBound(static_cast<uLong>(in.size())));
  uLongf out_size = static_cast<uLongf>(out.size());
  EXPECT_EQ(compress(out.data(), &out_size, in.data(), static_cast<uLong>(in.size())), Z_OK);
  out.resize(out_size);
  return out;
}

struct SyntheticFile {
  std::string name;
  std::vector<uint8_t> payload;
};

enum class NameCopy { kNone, kPlainText, kDeflatedTrailerOnly };

// Monta um FUFS bem formado: cabecalho de 12 bytes, tabela de registros de
// 12 bytes ordenada por hash crescente, payloads contiguos logo apos a
// tabela e (opcional) a lista de nomes.
std::vector<uint8_t> BuildFufs(std::vector<SyntheticFile> files, NameCopy names) {
  std::sort(files.begin(), files.end(), [](const SyntheticFile& a, const SyntheticFile& b) {
    return FufsArchive::HashName(a.name) < FufsArchive::HashName(b.name);
  });

  const uint32_t count = static_cast<uint32_t>(files.size());
  const uint32_t table_end = 12 + count * 12;

  std::vector<uint8_t> name_blob;
  for (const auto& f : files) {
    name_blob.insert(name_blob.end(), f.name.begin(), f.name.end());
    name_blob.push_back(0);
  }

  uint32_t data_size = 0;
  for (const auto& f : files) data_size += static_cast<uint32_t>(f.payload.size());

  const uint32_t plain_size =
      (names == NameCopy::kPlainText) ? static_cast<uint32_t>(name_blob.size()) : 0;
  const uint32_t names_end = table_end + data_size + plain_size;

  std::vector<uint8_t> out;
  out.insert(out.end(), {'F', 'U', 'F', 'S'});
  AppendU32LE(out, 0x80000000u | names_end);
  AppendU32LE(out, count);

  uint32_t offset = table_end;
  for (const auto& f : files) {
    AppendU32LE(out, offset);
    AppendU32LE(out, FufsArchive::HashName(f.name));
    AppendU32LE(out, static_cast<uint32_t>(f.payload.size()));
    offset += static_cast<uint32_t>(f.payload.size());
  }
  for (const auto& f : files) out.insert(out.end(), f.payload.begin(), f.payload.end());

  if (names == NameCopy::kPlainText) {
    out.insert(out.end(), name_blob.begin(), name_blob.end());
  }
  if (names != NameCopy::kNone) {
    AppendU32LE(out, static_cast<uint32_t>(name_blob.size()));
    std::vector<uint8_t> z = Deflate(name_blob);
    out.insert(out.end(), z.begin(), z.end());
  }
  return out;
}

std::vector<uint8_t> Bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<SyntheticFile> SampleFiles() {
  return {
      {"data/carts/chars/crash.pof", Bytes("payload-do-crash")},
      {"data/gfx/game/bomb_icon.png", Bytes("\x89PNG\r\n\x1a\n fake")},
      {"data/audio/engine_sound1_idle.psn", Bytes("PLZP....")},
      {"font.fnt", Bytes("f")},
  };
}

}  // namespace

// --- a funcao de hash -------------------------------------------------

// Vetores medidos: caminho real -> valor real gravado na tabela do .vfs.
TEST(FufsHash, ReproducesRealTableValues) {
  // mod/274214/data.vfs (Crash Nitro Kart 2); o caminho e uma string
  // literal do proprio cnk2.mod.
  EXPECT_EQ(FufsArchive::HashName("data/carts/chars/crash.pof"), 0x552c08b1u);
  // mod/280394/splash.vfs, primeiro nome da lista embutida do arquivo.
  EXPECT_EQ(FufsArchive::HashName("splash/menu/load_pad.pvr"), 0x1ebe20b8u);
}

TEST(FufsHash, IsCaseInsensitive) {
  EXPECT_EQ(FufsArchive::HashName("DATA/CARTS/CHARS/CRASH.POF"),
            FufsArchive::HashName("data/carts/chars/crash.pof"));
  EXPECT_EQ(FufsArchive::HashName(""), 0u);
}

TEST(FufsHash, IsPositionalBase67) {
  // Trocar um caractere muda o hash em delta * 67^(caracteres a direita).
  // Foi essa relacao, medida em pares reais, que revelou a base 67.
  const uint32_t a = FufsArchive::HashName("shaders/diffuse.fp");
  const uint32_t b = FufsArchive::HashName("shaders/diffuse.vp");
  EXPECT_EQ(static_cast<uint32_t>(b - a), static_cast<uint32_t>(('V' - 'F') * 67u));
}

// --- caminho feliz ----------------------------------------------------

TEST(FufsArchiveTest, ParsesArchiveWithPlainTextNames) {
  auto archive = FufsArchive::Parse(BuildFufs(SampleFiles(), NameCopy::kPlainText));
  ASSERT_TRUE(archive.has_value());
  EXPECT_TRUE(archive->HasNames());
  ASSERT_EQ(archive->Entries().size(), 4u);

  const FufsEntry* crash = archive->Find("data/carts/chars/crash.pof");
  ASSERT_NE(crash, nullptr);
  EXPECT_EQ(crash->name, "data/carts/chars/crash.pof");
  EXPECT_EQ(archive->Extract(*crash), Bytes("payload-do-crash"));

  // Busca insensivel a caixa, como o hash real.
  EXPECT_EQ(archive->Find("DATA/CARTS/CHARS/CRASH.POF"), crash);
  EXPECT_EQ(archive->Find("data/carts/chars/cortex.pof"), nullptr);
}

TEST(FufsArchiveTest, ParsesArchiveWithOnlyTheDeflatedNameTrailer) {
  auto archive =
      FufsArchive::Parse(BuildFufs(SampleFiles(), NameCopy::kDeflatedTrailerOnly));
  ASSERT_TRUE(archive.has_value());
  EXPECT_TRUE(archive->HasNames());
  const FufsEntry* fnt = archive->Find("font.fnt");
  ASSERT_NE(fnt, nullptr);
  EXPECT_EQ(fnt->name, "font.fnt");
}

// Dois dos seis .vfs reais (274214 e 277229) nao trazem lista de nomes:
// o parser precisa entregar a tabela mesmo assim, e a busca por nome
// continua funcionando porque a chave e o hash.
TEST(FufsArchiveTest, ParsesArchiveWithoutAnyNameList) {
  auto archive = FufsArchive::Parse(BuildFufs(SampleFiles(), NameCopy::kNone));
  ASSERT_TRUE(archive.has_value());
  EXPECT_FALSE(archive->HasNames());
  ASSERT_EQ(archive->Entries().size(), 4u);
  for (const auto& e : archive->Entries()) EXPECT_TRUE(e.name.empty());

  const FufsEntry* icon = archive->Find("data/gfx/game/bomb_icon.png");
  ASSERT_NE(icon, nullptr);
  EXPECT_EQ(archive->Extract(*icon), Bytes("\x89PNG\r\n\x1a\n fake"));
}

TEST(FufsArchiveTest, EntriesAreContiguousAndInsideTheFile) {
  auto data = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  auto archive = FufsArchive::Parse(data);
  ASSERT_TRUE(archive.has_value());
  uint64_t expected = 12 + archive->Entries().size() * 12;
  for (const auto& e : archive->Entries()) {
    EXPECT_EQ(e.offset, expected);
    expected += e.size;
    EXPECT_LE(static_cast<uint64_t>(e.offset) + e.size, data.size());
  }
}

// --- controle negativo ------------------------------------------------

TEST(FufsNegative, RejectsEmptyAndTooSmall) {
  EXPECT_FALSE(FufsArchive::Parse({}).has_value());
  EXPECT_FALSE(FufsArchive::Parse({'F', 'U', 'F', 'S'}).has_value());
  EXPECT_FALSE(FufsArchive::Parse({'F', 'U', 'F', 'S', 0, 0, 0, 0, 1, 0, 0}).has_value());
}

TEST(FufsNegative, RejectsWrongMagic) {
  auto data = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  data[0] = 'X';
  EXPECT_FALSE(FufsArchive::Parse(data).has_value());
}

TEST(FufsNegative, RejectsZeroAndAbsurdEntryCounts) {
  auto data = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  auto with_count = [&](uint32_t c) {
    auto copy = data;
    std::memcpy(copy.data() + 8, &c, 4);
    return copy;
  };
  EXPECT_FALSE(FufsArchive::Parse(with_count(0)).has_value());
  EXPECT_FALSE(FufsArchive::Parse(with_count(0xffffffffu)).has_value());
  EXPECT_FALSE(FufsArchive::Parse(with_count(100000u)).has_value());
}

TEST(FufsNegative, RejectsTruncatedFile) {
  auto full = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  uint32_t names_end;
  std::memcpy(&names_end, full.data() + 4, 4);
  names_end &= 0x7fffffffu;
  // Qualquer corte dentro da tabela, dos dados ou da lista de nomes tem
  // de ser recusado.
  const std::vector<size_t> cuts = {20, 40, names_end / 2, names_end - 1};
  for (size_t cut : cuts) {
    std::vector<uint8_t> truncated(full.begin(), full.begin() + cut);
    EXPECT_FALSE(FufsArchive::Parse(truncated).has_value()) << "corte em " << cut;
  }
  // Ressalva medida: cortar SO o rodape zlib (que e uma copia redundante
  // da lista de nomes) ainda deixa um arquivo integro -- os 4 .vfs reais
  // com nomes ja chegam assim, sem os 4 bytes de adler32 do stream.
  std::vector<uint8_t> without_trailer(full.begin(), full.begin() + names_end);
  auto archive = FufsArchive::Parse(without_trailer);
  ASSERT_TRUE(archive.has_value());
  EXPECT_TRUE(archive->HasNames());
}

TEST(FufsNegative, RejectsUnsortedHashTable) {
  auto data = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  // Troca os hashes das duas primeiras entradas: a tabela deixa de ser
  // crescente e a busca binaria nao valeria mais.
  std::swap_ranges(data.begin() + 16, data.begin() + 20, data.begin() + 28);
  EXPECT_FALSE(FufsArchive::Parse(data).has_value());
}

TEST(FufsNegative, RejectsOverlappingOrOutOfBoundsPayloads) {
  auto data = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  auto poke = [&](size_t at, uint32_t v) {
    auto copy = data;
    std::memcpy(copy.data() + at, &v, 4);
    return copy;
  };
  // Primeiro offset fora do fim da tabela (deixaria um buraco).
  EXPECT_FALSE(FufsArchive::Parse(poke(12, 12 + 4 * 12 + 8)).has_value());
  // Primeiro offset antes do fim da tabela (sobreporia a propria tabela).
  EXPECT_FALSE(FufsArchive::Parse(poke(12, 16)).has_value());
  // Tamanho enorme na primeira entrada: estoura o arquivo.
  EXPECT_FALSE(FufsArchive::Parse(poke(20, 0x7fffffffu)).has_value());
}

TEST(FufsNegative, RejectsRandomBytesWithAndWithoutTheMagic) {
  std::mt19937 rng(20260909);
  for (int round = 0; round < 200; ++round) {
    std::vector<uint8_t> junk(4096);
    for (auto& b : junk) b = static_cast<uint8_t>(rng() & 0xff);
    EXPECT_FALSE(FufsArchive::Parse(junk).has_value()) << "lixo puro, rodada " << round;
    std::memcpy(junk.data(), "FUFS", 4);
    // Lixo com o magic certo: e exatamente o caso que derrubava o probe.
    EXPECT_FALSE(FufsArchive::Parse(junk).has_value()) << "lixo com magic, rodada " << round;
  }
}

// Fuzz de mutacao de um byte: nunca pode explodir, e o que sobrar aceito
// tem de continuar internamente consistente (nada de falso positivo com
// offsets para fora do arquivo).
TEST(FufsNegative, SingleByteMutationsNeverCrashOrLie) {
  const auto original = BuildFufs(SampleFiles(), NameCopy::kPlainText);
  std::mt19937 rng(4242);
  for (int round = 0; round < 500; ++round) {
    auto data = original;
    data[rng() % data.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
    const size_t size = data.size();
    auto archive = FufsArchive::Parse(data);
    if (!archive.has_value()) continue;
    for (const auto& e : archive->Entries()) {
      EXPECT_LE(static_cast<uint64_t>(e.offset) + e.size, size);
      EXPECT_EQ(archive->Extract(e).size(), e.size);
      if (archive->HasNames()) {
        EXPECT_EQ(FufsArchive::HashName(e.name), e.name_hash);
      }
    }
  }
}

// Nome que nao bate com o hash da propria entrada: os nomes inteiros sao
// descartados em vez de mentir. O container continua utilizavel por hash.
TEST(FufsNegative, DropsNameListWhenAnyNameDoesNotHashToItsEntry) {
  auto files = SampleFiles();
  auto data = BuildFufs(files, NameCopy::kPlainText);
  // Estraga um caractere da copia em texto puro E da copia zlib nao da
  // para estragar de forma simples, entao remove-se o rodape zlib: sobra
  // so a lista corrompida.
  auto archive_ok = FufsArchive::Parse(data);
  ASSERT_TRUE(archive_ok.has_value());
  ASSERT_TRUE(archive_ok->HasNames());

  // Localiza a lista em texto puro e troca uma letra.
  const std::string needle = "font.fnt";
  auto it = std::search(data.begin() + 12, data.end(), needle.begin(), needle.end());
  ASSERT_NE(it, data.end());
  // A primeira ocorrencia pode ser o payload; pega a ultima.
  auto last = data.end();
  for (auto scan = data.begin(); scan != data.end();) {
    auto found = std::search(scan, data.end(), needle.begin(), needle.end());
    if (found == data.end()) break;
    last = found;
    scan = found + 1;
  }
  ASSERT_NE(last, data.end());
  *last = 'g';
  // Remove o rodape zlib para que a lista corrompida seja a unica fonte.
  uint32_t names_end;
  std::memcpy(&names_end, data.data() + 4, 4);
  names_end &= 0x7fffffffu;
  data.resize(names_end);

  auto archive = FufsArchive::Parse(data);
  ASSERT_TRUE(archive.has_value());
  EXPECT_FALSE(archive->HasNames());
  EXPECT_TRUE(archive->Entries()[0].name.empty());
}

TEST(FufsNegative, WrongNameCountIsNotAccepted) {
  auto files = SampleFiles();
  auto data = BuildFufs(files, NameCopy::kPlainText);
  uint32_t names_end;
  std::memcpy(&names_end, data.data() + 4, 4);
  names_end &= 0x7fffffffu;
  // Corta o ultimo NUL da lista em texto puro e o rodape zlib junto.
  data.resize(names_end - 1);
  uint32_t patched = 0x80000000u | static_cast<uint32_t>(data.size());
  std::memcpy(data.data() + 4, &patched, 4);
  auto archive = FufsArchive::Parse(data);
  ASSERT_TRUE(archive.has_value());
  EXPECT_FALSE(archive->HasNames());
}

// --- corpus real, opcional -------------------------------------------

// Roda so quando ZEEB_FUFS_CORPUS aponta para uma pasta com .vfs reais
// (a NAND de referencia nunca entra no repositorio). Serve para medir o
// parser de verdade, com os mesmos numeros citados em core/loader/fufs.h.
TEST(FufsCorpus, ParsesEveryRealVfsWhenTheCorpusIsAvailable) {
  const char* root = std::getenv("ZEEB_FUFS_CORPUS");
  if (root == nullptr) GTEST_SKIP() << "ZEEB_FUFS_CORPUS nao definido";

  namespace fs = std::filesystem;
  size_t archives = 0, entries = 0, named = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file() || e.path().extension() != ".vfs") continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    auto archive = FufsArchive::Parse(std::move(bytes));
    ASSERT_TRUE(archive.has_value()) << e.path().string();
    ++archives;
    entries += archive->Entries().size();
    if (archive->HasNames()) ++named;
    for (const auto& entry : archive->Entries()) {
      if (!entry.name.empty()) {
        EXPECT_EQ(FufsArchive::HashName(entry.name), entry.name_hash);
        EXPECT_EQ(archive->Find(entry.name), &entry);
      }
    }
  }
  std::printf("FUFS: %zu arquivos, %zu entradas, %zu com lista de nomes\n", archives,
              entries, named);
  EXPECT_GT(archives, 0u);
}
