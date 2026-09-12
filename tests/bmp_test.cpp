#include "core/loader/bmp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using zeebulator::DecodeBmp;

namespace {

// Escrita com limite: alguns testes montam cabecalhos DIB menores de
// proposito (para checar cabecalho truncado/desconhecido), e nesses
// casos os campos que nao cabem simplesmente nao sao gravados.
void PutU16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
  if (offset + 2 > out.size()) return;
  out[offset + 0] = static_cast<uint8_t>(value & 0xff);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void PutU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  if (offset + 4 > out.size()) return;
  out[offset + 0] = static_cast<uint8_t>(value & 0xff);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

// Descricao de um BMP montado pelo teste. Os campos existem para que
// cada teste consiga descrever UM defeito de cada vez (bpp errado,
// compressao RLE, paleta curta, pixel data faltando, ...) sem precisar
// reescrever o cabecalho na mao.
struct BmpSpec {
  int32_t width = 2;
  int32_t height = 2;
  uint16_t bpp = 24;
  uint32_t compression = 0;  // BI_RGB
  uint32_t clr_used = 0;
  uint32_t dib_size = 40;  // BITMAPINFOHEADER
  std::vector<uint32_t> masks;    // mascaras de BI_BITFIELDS (RGB, as vezes A)
  std::vector<uint8_t> palette;   // bytes crus, ja no tamanho de entrada correto
  std::vector<uint8_t> pixels;    // bytes crus, com o padding de linha ja incluido
};

// Monta um arquivo BMP completo em memoria. `bfSize` (offset 2) e
// `biSizeImage` (offset 34) sao gravados de proposito como o valor
// "obvio" e como 0: o decodificador tem que ignorar os dois, como
// acontece com arquivo real (o BMP real de 214x34 traz biSizeImage 2
// bytes maior que o calculado e o arquivo tem 2 bytes sobrando no fim).
std::vector<uint8_t> MakeBmp(const BmpSpec& spec) {
  const size_t masks_bytes = spec.masks.size() * 4;
  // Nos cabecalhos V4/V5 (>= 52 bytes) as mascaras sao campos DENTRO do
  // cabecalho (offsets 40/44/48/52); no BITMAPINFOHEADER de 40 bytes
  // elas vem imediatamente APOS o cabecalho.
  const bool masks_inside = spec.dib_size >= 52;
  std::vector<uint8_t> file(14 + spec.dib_size, 0);
  if (!masks_inside && masks_bytes > 0) file.resize(14 + spec.dib_size + masks_bytes, 0);
  const size_t masks_at = 14 + (masks_inside ? 40 : spec.dib_size);
  const size_t offset = 14 + spec.dib_size + (masks_inside ? 0 : masks_bytes) + spec.palette.size();

  file[0] = 'B';
  file[1] = 'M';
  PutU32(file, 2, static_cast<uint32_t>(offset + spec.pixels.size()));
  PutU32(file, 10, static_cast<uint32_t>(offset));
  PutU32(file, 14, spec.dib_size);
  PutU32(file, 18, static_cast<uint32_t>(spec.width));
  PutU32(file, 22, static_cast<uint32_t>(spec.height));
  PutU16(file, 26, 1);  // biPlanes
  PutU16(file, 28, spec.bpp);
  PutU32(file, 30, spec.compression);
  PutU32(file, 34, 0);  // biSizeImage = 0 ("calcule pela largura/altura")
  PutU32(file, 46, spec.clr_used);
  for (size_t i = 0; i < spec.masks.size(); ++i) {
    PutU32(file, masks_at + i * 4, spec.masks[i]);
  }

  std::vector<uint8_t> out = file;
  out.insert(out.end(), spec.palette.begin(), spec.palette.end());
  out.insert(out.end(), spec.pixels.begin(), spec.pixels.end());
  return out;
}

// Monta um BITMAPCOREHEADER (12 bytes) + os bytes passados.
std::vector<uint8_t> MakeCoreBmp(uint16_t width, uint16_t height, uint16_t bpp,
                                 const std::vector<uint8_t>& palette,
                                 const std::vector<uint8_t>& pixels) {
  const size_t offset = 14 + 12 + palette.size();
  std::vector<uint8_t> file(14 + 12, 0);
  file[0] = 'B';
  file[1] = 'M';
  PutU32(file, 2, static_cast<uint32_t>(offset + pixels.size()));
  PutU32(file, 10, static_cast<uint32_t>(offset));
  PutU32(file, 14, 12);
  PutU16(file, 18, width);
  PutU16(file, 20, height);
  PutU16(file, 22, 1);  // biPlanes
  PutU16(file, 24, bpp);
  file.insert(file.end(), palette.begin(), palette.end());
  file.insert(file.end(), pixels.begin(), pixels.end());
  return file;
}

// Concatena linhas de pixel ja alinhadas em 4 bytes, preenchendo o
// padding com o valor passado (0 e o que os gravadores reais usam; um
// valor diferente de zero prova que o padding nao entrou na imagem).
std::vector<uint8_t> PaddedRows(int width, int bytes_per_pixel,
                                const std::vector<std::vector<uint8_t>>& rows,
                                uint8_t padding = 0) {
  const size_t stride = ((static_cast<size_t>(width) * bytes_per_pixel + 3) / 4) * 4;
  std::vector<uint8_t> out;
  for (const auto& row : rows) {
    out.insert(out.end(), row.begin(), row.end());
    out.resize(out.size() + (stride - row.size()), padding);
  }
  return out;
}

uint8_t Rgba(const std::vector<uint8_t>& rgba, int width, int x, int y, int channel) {
  return rgba[(static_cast<size_t>(y) * width + x) * 4 + channel];
}

// Le um arquivo real do corpus, se ele estiver montado nesta maquina.
std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

// Recurso real 5007 de tectoy_pt.brf (tipo 6, MIME "image/bmp"), ja
// extraido para /tmp. Sem o arquivo nesta maquina o teste e SKIP
// EXPLICITO -- nunca um pass silencioso.
std::filesystem::path RealZwResourcePath() {
  if (const char* env = std::getenv("ZEEB_BMP_ZW")) return std::filesystem::path(env);
  if (std::filesystem::exists("/tmp/zw_res5007.bmp")) return std::filesystem::path("/tmp/zw_res5007.bmp");
  return std::filesystem::path(FIXTURES_DIR) / "zw_res5007.bmp";
}

// stage_background.bmp real do console (mod/274755 do corpus debug_nand).
std::filesystem::path RealStageBackgroundPath() {
  if (const char* env = std::getenv("ZEEB_BMP_STAGE")) return std::filesystem::path(env);
  return std::filesystem::path(
      "/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand/mod/274755/stage_background.bmp");
}

}  // namespace

// ---------------------------------------------------------------------
// Erros: cada teste cobre uma familia e exige nullopt. Requisito do
// projeto: nenhum caminho devolve imagem vazia, preta ou parcial.
// ---------------------------------------------------------------------

TEST(Bmp, RejectsNullPointer) {
  int w = 0, h = 0;
  EXPECT_FALSE(DecodeBmp(nullptr, 0, w, h).has_value());
  EXPECT_FALSE(DecodeBmp(nullptr, 65536, w, h).has_value());
  EXPECT_FALSE(DecodeBmp(nullptr, 0).has_value());
}

TEST(Bmp, RejectsWrongSignature) {
  // Sem "BM" no inicio: nem file header valido, mesmo tendo tamanho.
  std::vector<uint8_t> not_bmp(64, 0);
  not_bmp[0] = 'Z';
  not_bmp[1] = 'M';
  int w = -1, h = -1;
  EXPECT_FALSE(DecodeBmp(not_bmp.data(), not_bmp.size(), w, h).has_value());
  // Em erro as dimensoes de saida ficam intocadas (nada de largura
  // parcialmente escrita).
  EXPECT_EQ(w, -1);
  EXPECT_EQ(h, -1);

  // Um BMP de verdade com a assinatura zerada continua sendo rejeitado.
  BmpSpec spec;
  spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}});
  auto data = MakeBmp(spec);
  data[0] = 0;
  EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
}

TEST(Bmp, RejectsTruncatedHeader) {
  int w = 0, h = 0;
  // Menor que o BITMAPFILEHEADER (14 bytes).
  const std::vector<uint8_t> tiny = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(DecodeBmp(tiny.data(), tiny.size(), w, h).has_value());

  // File header inteiro, mas nenhum byte do cabecalho DIB.
  std::vector<uint8_t> no_dib(14, 0);
  no_dib[0] = 'B';
  no_dib[1] = 'M';
  PutU32(no_dib, 10, 54);
  EXPECT_FALSE(DecodeBmp(no_dib.data(), no_dib.size(), w, h).has_value());

  // DIB declara 40 bytes mas so 16 existem no buffer.
  BmpSpec spec;
  spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {0, 0, 0, 0, 0, 0}});
  auto data = MakeBmp(spec);
  EXPECT_FALSE(DecodeBmp(data.data(), 14 + 16, w, h).has_value());

  // BITMAPCOREHEADER (12) tambem truncado.
  auto core = MakeCoreBmp(2, 1, 24, {}, PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}}));
  EXPECT_FALSE(DecodeBmp(core.data(), 14 + 8, w, h).has_value());

  // Tamanho de DIB que nao existe no formato (16/20/24).
  BmpSpec bogus = spec;
  bogus.dib_size = 16;
  auto bogus_data = MakeBmp(bogus);
  EXPECT_FALSE(DecodeBmp(bogus_data.data(), bogus_data.size(), w, h).has_value());
}

TEST(Bmp, RejectsUnsupportedBpp) {
  const uint16_t unsupported[] = {0, 1, 2, 4, 15, 12, 48};
  for (const uint16_t bpp : unsupported) {
    BmpSpec spec;
    spec.bpp = bpp;
    spec.pixels = std::vector<uint8_t>(64, 0x11);
    auto data = MakeBmp(spec);
    SCOPED_TRACE(testing::Message() << "bpp=" << bpp);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
}

TEST(Bmp, RejectsUnsupportedCompression) {
  // RLE8 (BI_RLE8 = 1): stream RLE valido presente, para que a recusa
  // seja pela COMPRESSAO e nao por falta de bytes.
  {
    BmpSpec spec;
    spec.bpp = 8;
    spec.compression = 1;
    spec.clr_used = 2;
    spec.palette = std::vector<uint8_t>(2 * 4, 0xff);
    // 1 run de 2 pixels do indice 0 + end-of-bitmap (0,0).
    spec.pixels = {2, 0, 0, 0};
    auto data = MakeBmp(spec);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
  // RLE4 (BI_RLE4 = 2), mesma ideia.
  {
    BmpSpec spec;
    spec.bpp = 8;
    spec.compression = 2;
    spec.clr_used = 2;
    spec.palette = std::vector<uint8_t>(2 * 4, 0xff);
    spec.pixels = {2, 0x01, 0, 0};
    auto data = MakeBmp(spec);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
  // BI_JPEG (4), BI_PNG (5) e valor desconhecido.
  for (const uint32_t compression : {4u, 5u, 7u, 0xffffffffu}) {
    BmpSpec spec;
    spec.compression = compression;
    spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {0, 0, 0, 0, 0, 0}});
    auto data = MakeBmp(spec);
    SCOPED_TRACE(testing::Message() << "compression=" << compression);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
  // BI_BITFIELDS (3) em 24 bpp nao existe: as mascaras nao se aplicam.
  {
    BmpSpec spec;
    spec.compression = 3;
    spec.masks = {0x00ff0000u, 0x0000ff00u, 0x000000ffu};
    spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {0, 0, 0, 0, 0, 0}});
    auto data = MakeBmp(spec);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
  // Mascaras tortas: canal esparso (dois blocos de bits), sobrepostas,
  // zeradas ou que nao cabem em 32 bits -- todas recusadas em vez de
  // virarem canal zero silencioso.
  const std::vector<std::vector<uint32_t>> bad_masks = {
      {0x0000f00fu, 0x00000ff0u, 0x0000000fu},  // esparsa
      {0xf800f800u, 0x07e00000u, 0x001f0000u},  // sobreposta (R e G)
      {0, 0x07e0u, 0x001fu},                    // R zerada
      {0xff000000u, 0xff000000u, 0xff000000u},  // todas sobrepostas
  };
  for (const auto& masks : bad_masks) {
    BmpSpec spec;
    spec.bpp = 16;
    spec.compression = 3;
    spec.masks = masks;
    spec.pixels = std::vector<uint8_t>(8, 0);
    auto data = MakeBmp(spec);
    int w = 0, h = 0;
    EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());
  }
  // BI_BITFIELDS sem os 12 bytes de mascara no arquivo.
  {
    BmpSpec spec;
    spec.bpp = 16;
    spec.compression = 3;
    spec.pixels = std::vector<uint8_t>(8, 0);
    auto data = MakeBmp(spec);  // sem mascaras: offset aponta direto nos pixels
    EXPECT_FALSE(DecodeBmp(data.data(), data.size()).has_value());
  }
}

TEST(Bmp, RejectsInsufficientPixelData) {
  // 24 bpp 3x2: cada linha precisa de 12 bytes (9 + 3 de padding). So
  // uma linha esta no arquivo.
  BmpSpec spec;
  spec.width = 3;
  spec.height = 2;
  spec.pixels = PaddedRows(3, 3, {{0, 0, 255, 0, 0, 255, 0, 0, 255}});
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());

  // Faltando 1 byte de padding da ultima linha tambem e insuficiente:
  // o padding faz parte da linha gravada.
  auto short_by_one = data;
  short_by_one.resize(short_by_one.size() - 1);
  EXPECT_FALSE(DecodeBmp(short_by_one.data(), short_by_one.size(), w, h).has_value());

  // bfOffBits apontando para fora do arquivo (ou exatamente no fim,
  // sem nenhum pixel).
  BmpSpec full;
  full.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {0, 0, 0, 0, 0, 0}});
  auto ok = MakeBmp(full);
  std::vector<uint8_t> past_end = ok;
  PutU32(past_end, 10, static_cast<uint32_t>(past_end.size() + 16));
  EXPECT_FALSE(DecodeBmp(past_end.data(), past_end.size(), w, h).has_value());
  std::vector<uint8_t> at_end = ok;
  PutU32(at_end, 10, static_cast<uint32_t>(at_end.size()));
  EXPECT_FALSE(DecodeBmp(at_end.data(), at_end.size(), w, h).has_value());

  // bfOffBits menor que o file header e arquivo incoerente.
  std::vector<uint8_t> bad_offset = ok;
  PutU32(bad_offset, 10, 4);
  EXPECT_FALSE(DecodeBmp(bad_offset.data(), bad_offset.size(), w, h).has_value());
}

TEST(Bmp, RejectsTruncatedPalette) {
  // 8 bpp declara 256 entradas de 4 bytes = 1024 bytes, mas o arquivo
  // so tem 4 entradas antes do pixel data.
  BmpSpec spec;
  spec.bpp = 8;
  spec.pixels = std::vector<uint8_t>(8, 0);
  spec.palette = std::vector<uint8_t>(4 * 4, 0x40);
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());

  // biClrUsed maior que o bpp comporta (300 entradas em 8 bpp) e
  // cabecalho incoerente.
  BmpSpec too_many;
  too_many.bpp = 8;
  too_many.clr_used = 300;
  too_many.palette = std::vector<uint8_t>(300 * 4, 0x40);
  too_many.pixels = std::vector<uint8_t>(8, 0);
  auto too_many_data = MakeBmp(too_many);
  EXPECT_FALSE(DecodeBmp(too_many_data.data(), too_many_data.size(), w, h).has_value());

  // Paleta completa presente, mas sem pixel data nenhum.
  BmpSpec no_pixels;
  no_pixels.bpp = 8;
  no_pixels.clr_used = 4;
  no_pixels.palette = std::vector<uint8_t>(4 * 4, 0x40);
  auto no_pixels_data = MakeBmp(no_pixels);
  EXPECT_FALSE(DecodeBmp(no_pixels_data.data(), no_pixels_data.size(), w, h).has_value());

  // O BITMAPCOREHEADER usa 3 bytes por entrada (BGR). Com o cabecalho
  // "core" declarando 4 entradas so ha 10 dos 12 bytes.
  auto core = MakeCoreBmp(2, 1, 8, std::vector<uint8_t>(10, 0x10),
                          std::vector<uint8_t>(4, 0));
  EXPECT_FALSE(DecodeBmp(core.data(), core.size(), w, h).has_value());
}

TEST(Bmp, RejectsPaletteIndexOutsidePalette) {
  // Paleta com 2 entradas; o pixel usa o indice 5. Indice fora da
  // paleta e erro explicito (no mundo real isso normalmente vira
  // "imagem preta" silenciosa).
  BmpSpec spec;
  spec.width = 2;
  spec.height = 1;
  spec.bpp = 8;
  spec.clr_used = 2;
  spec.palette = {0, 0, 255, 0, /* entrada 1: verde */ 0, 255, 0, 0};
  spec.pixels = PaddedRows(2, 1, {{0, 5}});
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  EXPECT_FALSE(DecodeBmp(data.data(), data.size(), w, h).has_value());

  // Mesmo arquivo com indices validos decodifica (prova que o defeito
  // acima e mesmo o indice, nao o resto do arquivo).
  BmpSpec ok = spec;
  ok.pixels = PaddedRows(2, 1, {{0, 1}});
  auto ok_data = MakeBmp(ok);
  auto decoded = DecodeBmp(ok_data.data(), ok_data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(w, 2);
  ASSERT_EQ(h, 1);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 0), 255);  // entrada 0 = azul
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 2), 0);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 1), 255);  // entrada 1 = verde
}

// ---------------------------------------------------------------------
// Casos validos. Os cabecalhos destes testes sao construidos aqui
// porque nos recursos reais medidos so existe BITMAPINFOHEADER de 40
// bytes, 24 bpp, BI_RGB e bottom-up (ver bmp.h): 8/16/32 bpp e top-down
// sao exigencia de cobertura, nao medicao do corpus.
// ---------------------------------------------------------------------

TEST(Bmp, Decodes24BppBottomUpInfoHeader) {
  // 3x2, bottom-up: a PRIMEIRA linha gravada e a de BAIXO.
  BmpSpec spec;
  spec.width = 3;
  spec.height = 2;
  spec.bpp = 24;
  // Os trios de bytes sao BGR, como no formato. Linha gravada primeiro
  // (a de BAIXO num arquivo bottom-up): BGR 00 00 ff = vermelho,
  // 00 ff 00 = verde, ff 00 00 = azul.
  spec.pixels = PaddedRows(3, 3,
                           {{0, 0, 255, 0, 255, 0, 255, 0, 0},
                            {255, 255, 0, 255, 0, 255, 128, 128, 128}});
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 3);
  EXPECT_EQ(h, 2);
  ASSERT_EQ(decoded->size(), 3u * 2u * 4u);
  // Linha 0 da saida = topo = a SEGUNDA linha do arquivo.
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 0), 0);    // BGR ff ff 00 = ciano
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 1), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 2), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 3), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 1, 0, 0), 255);  // BGR ff 00 ff = magenta
  EXPECT_EQ(Rgba(*decoded, 3, 1, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 1, 0, 2), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 2, 0, 0), 128);  // cinza
  EXPECT_EQ(Rgba(*decoded, 3, 2, 0, 1), 128);
  EXPECT_EQ(Rgba(*decoded, 3, 2, 0, 2), 128);
  // Linha 1 = base = a PRIMEIRA linha do arquivo.
  EXPECT_EQ(Rgba(*decoded, 3, 0, 1, 0), 255);  // BGR 00 00 ff = vermelho
  EXPECT_EQ(Rgba(*decoded, 3, 0, 1, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 0, 1, 2), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 1, 1, 0), 0);    // BGR 00 ff 00 = verde
  EXPECT_EQ(Rgba(*decoded, 3, 1, 1, 1), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 1, 1, 2), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 2, 1, 0), 0);    // BGR ff 00 00 = azul
  EXPECT_EQ(Rgba(*decoded, 3, 2, 1, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 2, 1, 2), 255);
  // O padding de linha (3 bytes por linha aqui) nao pode ter virado
  // pixel: nenhuma amostra acima pode ser 0xaa.
}

TEST(Bmp, Decodes24BppTopDownNegativeHeight) {
  // 2x2 com biHeight = -2: a primeira linha gravada ja e o topo.
  BmpSpec spec;
  spec.width = 2;
  spec.height = -2;
  spec.bpp = 24;
  spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 255, 0}, {255, 0, 0, 255, 255, 255}});
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(h, 2);  // altura devolvida e sempre positiva
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 0), 255);  // topo: vermelho
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 1), 255);  // topo: verde
  EXPECT_EQ(Rgba(*decoded, 2, 0, 1, 2), 255);  // base: azul
  EXPECT_EQ(Rgba(*decoded, 2, 1, 1, 0), 255);  // base: branco
}

TEST(Bmp, Decodes8BppWithPalette) {
  // 3x1 em 8 bpp: stride = 4, entao ha 1 byte de padding (0xAA, para
  // provar que ele nao vira pixel).
  BmpSpec spec;
  spec.width = 3;
  spec.height = 1;
  spec.bpp = 8;
  spec.clr_used = 3;
  // Entradas de 4 bytes BGRX: vermelho, verde, azul.
  spec.palette = {0, 0, 255, 0, /**/ 0, 255, 0, 0, /**/ 255, 0, 0, 0};
  spec.pixels = PaddedRows(3, 1, {{0, 1, 2}}, 0xaa);
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 3);
  EXPECT_EQ(h, 1);
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 0, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 3, 1, 0, 1), 255);
  EXPECT_EQ(Rgba(*decoded, 3, 2, 0, 2), 255);
  for (int x = 0; x < 3; ++x) EXPECT_EQ(Rgba(*decoded, 3, x, 0, 3), 255);
}

TEST(Bmp, DecodesBitmapCoreHeader) {
  // BITMAPCOREHEADER (12 bytes), 8 bpp: paleta de 3 bytes por entrada.
  // O cabecalho "core" NAO tem campo biClrUsed: o formato fixa a paleta
  // em 1 << bpp = 256 entradas (768 bytes), entao as 254 entradas que o
  // teste nao preenche precisam existir de verdade.
  std::vector<uint8_t> core_palette(256 * 3, 0);
  core_palette[0] = 0;   // entrada 0: B = 0
  core_palette[1] = 0;   //             G = 0
  core_palette[2] = 255;  //            R = 255 -> vermelho
  core_palette[3] = 0;   // entrada 1: B = 0
  core_palette[4] = 255;  //            G = 255
  core_palette[5] = 0;   //              R = 0  -> verde
  auto core8 = MakeCoreBmp(2, 1, 8, core_palette, std::vector<uint8_t>{0, 1, 0, 0});
  int w = 0, h = 0;
  auto decoded8 = DecodeBmp(core8.data(), core8.size(), w, h);
  ASSERT_TRUE(decoded8.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(h, 1);
  EXPECT_EQ(Rgba(*decoded8, 2, 0, 0, 0), 255);  // vermelho
  EXPECT_EQ(Rgba(*decoded8, 2, 1, 0, 1), 255);  // verde

  // CORE 24 bpp: sem campo de compressao e sempre bottom-up.
  auto core24 = MakeCoreBmp(2, 2, 24, {},
                            PaddedRows(2, 3, {{0, 0, 255, 0, 255, 0}, {255, 0, 0, 255, 255, 255}}));
  auto decoded24 = DecodeBmp(core24.data(), core24.size(), w, h);
  ASSERT_TRUE(decoded24.has_value());
  EXPECT_EQ(h, 2);
  EXPECT_EQ(Rgba(*decoded24, 2, 0, 0, 2), 255);  // topo = segunda linha do arquivo = azul
  EXPECT_EQ(Rgba(*decoded24, 2, 0, 1, 0), 255);  // base = primeira linha = vermelho
}

TEST(Bmp, Decodes16BppRgb555) {
  // BI_RGB em 16 bpp nao grava mascara: a convencao do formato e 555.
  BmpSpec spec;
  spec.width = 4;
  spec.height = 1;
  spec.bpp = 16;
  // 555: R = bits 14..10, G = 9..5, B = 4..0.
  const uint16_t red = 0x7c00;
  const uint16_t green = 0x03e0;
  const uint16_t blue = 0x001f;
  const uint16_t white = 0x7fff;
  spec.pixels = {static_cast<uint8_t>(red & 0xff), static_cast<uint8_t>(red >> 8),
                 static_cast<uint8_t>(green & 0xff), static_cast<uint8_t>(green >> 8),
                 static_cast<uint8_t>(blue & 0xff), static_cast<uint8_t>(blue >> 8),
                 static_cast<uint8_t>(white & 0xff), static_cast<uint8_t>(white >> 8)};
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 4);
  // 5 bits expandidos para 8: 31 -> 255 (nao 248).
  EXPECT_EQ(Rgba(*decoded, 4, 0, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 4, 0, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 4, 1, 0, 1), 255);
  EXPECT_EQ(Rgba(*decoded, 4, 2, 0, 2), 255);
  EXPECT_EQ(Rgba(*decoded, 4, 3, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 4, 3, 0, 1), 255);
  EXPECT_EQ(Rgba(*decoded, 4, 3, 0, 3), 255);
}

TEST(Bmp, Decodes16Bpp565Bitfields) {
  // 565 declarado por BI_BITFIELDS: as mascaras ficam nos 12 bytes
  // imediatamente apos o BITMAPINFOHEADER de 40 bytes.
  BmpSpec spec;
  spec.width = 2;
  spec.height = 1;
  spec.bpp = 16;
  spec.compression = 3;  // BI_BITFIELDS
  spec.masks = {0xf800u, 0x07e0u, 0x001fu};
  const uint16_t red = 0xf800;
  const uint16_t green = 0x07e0;
  spec.pixels = {static_cast<uint8_t>(red & 0xff), static_cast<uint8_t>(red >> 8),
                 static_cast<uint8_t>(green & 0xff), static_cast<uint8_t>(green >> 8)};
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 1), 255);  // 63 -> 255
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 3), 255);  // 16 bpp sem alfa = opaco
}

TEST(Bmp, Decodes32BppRgbAsOpaque) {
  // 32 bpp BI_RGB: o 4o byte nao tem alfa definido (ferramentas gravam
  // 0) e a GDI ignora; o alfa da saida tem que ser 255, senao a imagem
  // inteira sairia transparente.
  BmpSpec spec;
  spec.width = 2;
  spec.height = 1;
  spec.bpp = 32;
  // BGRX: 00 00 ff = vermelho, ff 00 00 = azul. O 4o byte e 0 nos dois
  // (o que as ferramentas gravam), e mesmo assim o alfa tem que sair 255.
  spec.pixels = {0, 0, 255, 0, 255, 0, 0, 0};
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 1), 0);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 2), 0);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 3), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 2), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 3), 255);
}

TEST(Bmp, Decodes32BppBitfieldsWithAlphaMask) {
  // Cabecalho V4 (108 bytes) com BI_BITFIELDS e QUARTA mascara de alfa.
  // So um cabecalho >= 56 bytes tem essa mascara dentro dele; um
  // BITMAPINFOHEADER de 40 bytes nao tem, e ali o 4o byte continua
  // sendo lixo (teste anterior).
  BmpSpec spec;
  spec.width = 2;
  spec.height = 1;
  spec.bpp = 32;
  spec.dib_size = 108;
  spec.compression = 3;
  spec.masks = {0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u};
  // Pixel 0: azul, alfa 255. Pixel 1: vermelho, alfa 128 (0x80).
  spec.pixels = {255, 0, 0, 255, 0, 0, 255, 128};
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 2), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 0, 0, 3), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 0), 255);
  EXPECT_EQ(Rgba(*decoded, 2, 1, 0, 3), 128);
}

TEST(Bmp, IgnoresBytesAfterTheLastRow) {
  // O recurso real de 214x34 tem 2 bytes sobrando depois das linhas (e
  // biSizeImage 2 bytes maior que o calculado): lixo no fim nao pode
  // virar erro nem pixel.
  BmpSpec spec;
  spec.width = 2;
  spec.height = 2;
  spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {255, 0, 0, 255, 255, 255}});
  spec.pixels.push_back(0xde);
  spec.pixels.push_back(0xad);
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto decoded = DecodeBmp(data.data(), data.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 2);
  EXPECT_EQ(h, 2);
  EXPECT_EQ(decoded->size(), 16u);
}

TEST(Bmp, TwoArgumentOverloadMatchesTheDimensionOne) {
  BmpSpec spec;
  spec.width = 2;
  spec.height = 2;
  spec.pixels = PaddedRows(2, 3, {{0, 0, 255, 0, 0, 255}, {255, 0, 0, 255, 255, 255}});
  auto data = MakeBmp(spec);
  int w = 0, h = 0;
  auto with_dims = DecodeBmp(data.data(), data.size(), w, h);
  auto without_dims = DecodeBmp(data.data(), data.size());
  ASSERT_TRUE(with_dims.has_value());
  ASSERT_TRUE(without_dims.has_value());
  EXPECT_EQ(*with_dims, *without_dims);
  EXPECT_EQ(w, 2);
  EXPECT_EQ(h, 2);
}

// ---------------------------------------------------------------------
// ARQUIVO REAL 1: recurso 5007 (tipo 6, MIME "image/bmp") de
// `tectoy_pt.brf`, extraido para /tmp. Medido nos bytes: 21952 bytes,
// assinatura "BM", BITMAPINFOHEADER de 40 bytes, 214x34, 24 bpp,
// BI_RGB, bfOffBits 54, biSizeImage 21898 (2 bytes MAIOR que
// 644*34 = 21896). Os pixels conferidos abaixo vieram de um decodificador
// independente (Pillow) sobre o MESMO arquivo, nao deste codigo.
// ---------------------------------------------------------------------
TEST(Bmp, DecodesRealZwResourceBmpFromTectoyBrf) {
  const std::filesystem::path path = RealZwResourcePath();
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "recurso BMP real (5007 de tectoy_pt.brf) nao esta nesta maquina: "
                 << path.string() << " (defina ZEEB_BMP_ZW para apontar para ele)";
  }
  const std::vector<uint8_t> bytes = ReadFile(path);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes.size(), 21952u);
  EXPECT_EQ(bytes[0], 'B');
  EXPECT_EQ(bytes[1], 'M');

  int w = 0, h = 0;
  auto decoded = DecodeBmp(bytes.data(), bytes.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 214);
  EXPECT_EQ(h, 34);
  ASSERT_EQ(decoded->size(), 214u * 34u * 4u);

  // Confere que a orientacao e mesmo bottom-up: o pixel escuro da
  // primeira linha logica esta na linha 6 do topo e a linha 27 (mesma
  // coluna, imagem espelhada) e branca.
  auto px = [&](int x, int y, int c) { return Rgba(*decoded, 214, x, y, c); };
  EXPECT_EQ(px(108, 6, 0), 51);
  EXPECT_EQ(px(108, 6, 1), 51);
  EXPECT_EQ(px(108, 6, 2), 51);
  EXPECT_EQ(px(108, 27, 0), 255);
  EXPECT_EQ(px(108, 27, 1), 255);
  EXPECT_EQ(px(108, 27, 2), 255);
  // Amostras de canto e de borda (todas brancas neste recurso).
  EXPECT_EQ(px(0, 0, 0), 255);
  EXPECT_EQ(px(213, 0, 2), 255);
  EXPECT_EQ(px(0, 33, 1), 255);
  EXPECT_EQ(px(213, 33, 0), 255);
  EXPECT_EQ(px(62, 1, 0), 245);
  EXPECT_EQ(px(59, 2, 2), 193);
  EXPECT_EQ(px(150, 20, 1), 228);

  // Contagem global: 5983 pixels brancos exatos e 174 pixels (51,51,51)
  // no arquivo real, alem de todo pixel opaco (fonte de 24 bpp).
  int white = 0;
  int darkest = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      if (decoded->at(i) == 255 && decoded->at(i + 1) == 255 && decoded->at(i + 2) == 255) ++white;
      if (decoded->at(i) == 51 && decoded->at(i + 1) == 51 && decoded->at(i + 2) == 51) ++darkest;
      ASSERT_EQ(decoded->at(i + 3), 255) << "x=" << x << " y=" << y;
    }
  }
  EXPECT_EQ(white, 5983);
  EXPECT_EQ(darkest, 174);
}

// ---------------------------------------------------------------------
// ARQUIVO REAL 2: stage_background.bmp do proprio console
// (mod/274755 do corpus debug_nand). Medido nos bytes: 633656 bytes,
// "BM", BITMAPINFOHEADER 40, 640x330, 24 bpp, BI_RGB, bfOffBits 54,
// biSizeImage 633602. E um fundo 100% azul: todo pixel tem o canal azul
// maior que o vermelho e que o verde, e os dois tons dominantes medidos
// (44,136,199) e (143,193,227) aparecem 13320 e 6827 vezes.
// ---------------------------------------------------------------------
TEST(Bmp, DecodesRealStageBackgroundFromConsoleMod) {
  const std::filesystem::path path = RealStageBackgroundPath();
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "stage_background.bmp real do console nao esta nesta maquina: " << path.string()
                 << " (defina ZEEB_BMP_STAGE para apontar para ele)";
  }
  const std::vector<uint8_t> bytes = ReadFile(path);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes.size(), 633656u);
  EXPECT_EQ(bytes[0], 'B');
  EXPECT_EQ(bytes[1], 'M');

  int w = 0, h = 0;
  auto decoded = DecodeBmp(bytes.data(), bytes.size(), w, h);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(w, 640);
  EXPECT_EQ(h, 330);
  ASSERT_EQ(decoded->size(), 640u * 330u * 4u);

  auto px = [&](int x, int y, int c) { return Rgba(*decoded, 640, x, y, c); };
  EXPECT_EQ(px(0, 0, 0), 13);    // canto superior esquerdo
  EXPECT_EQ(px(0, 0, 1), 54);
  EXPECT_EQ(px(0, 0, 2), 123);
  EXPECT_EQ(px(639, 0, 1), 58);  // canto superior direito
  EXPECT_EQ(px(0, 329, 2), 210);  // canto inferior esquerdo
  EXPECT_EQ(px(639, 329, 0), 50);
  EXPECT_EQ(px(320, 165, 0), 44);  // centro, tom dominante
  EXPECT_EQ(px(320, 165, 1), 135);
  EXPECT_EQ(px(320, 165, 2), 199);
  EXPECT_EQ(px(500, 300, 2), 212);

  // 100% azul: nenhum pixel fora disso, e os dois tons dominantes com a
  // contagem exata do arquivo real.
  size_t blue_dominant = 0;
  size_t dominant_a = 0;
  size_t dominant_b = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      const int r = decoded->at(i);
      const int g = decoded->at(i + 1);
      const int b = decoded->at(i + 2);
      if (b > r && b > g) ++blue_dominant;
      if (r == 44 && g == 136 && b == 199) ++dominant_a;
      if (r == 143 && g == 193 && b == 227) ++dominant_b;
      ASSERT_EQ(decoded->at(i + 3), 255) << "x=" << x << " y=" << y;
    }
  }
  EXPECT_EQ(blue_dominant, 640u * 330u);
  EXPECT_EQ(dominant_a, 13320u);
  EXPECT_EQ(dominant_b, 6827u);
}

// ---------------------------------------------------------------------
// Corpus real inteiro: todos os .bmp do corpus debug_nand (raiz
// configuravel por ZEEB_BMP_CORPUS). Medido com este decodificador e
// comparado pixel a pixel com um decodificador independente (Pillow):
// sao 65 arquivos -- 42 de 24 bpp, 10 de 16 bpp e 13 de 32 bpp, TODOS
// com BITMAPINFOHEADER de 40 bytes e BI_RGB (0), 10 deles com altura
// NEGATIVA (top-down real, nao so sintetico). Nos 16 bpp o resultado
// bate com 555 (diferenca maxima de 1 unidade, so arredondamento de 5
// para 8 bits; uma leitura 565 diferiria em ate 132 unidades). Nos 13
// arquivos de 32 bpp o 4o byte e 0 em TODOS os pixels, e o alfa forcado
// a 255 e exatamente o que evita devolver 13 imagens transparentes.
// Sem a raiz do corpus nesta maquina o teste e SKIP explicito.
// ---------------------------------------------------------------------
TEST(Bmp, DecodesEveryRealBmpOfTheCorpusWhenAvailable) {
  const char* env = std::getenv("ZEEB_BMP_CORPUS");
  const std::filesystem::path root =
      env != nullptr ? std::filesystem::path(env)
                     : std::filesystem::path(
                           "/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand");
  if (!std::filesystem::is_directory(root)) {
    GTEST_SKIP() << "corpus de BMP real nao esta nesta maquina: " << root.string()
                 << " (defina ZEEB_BMP_CORPUS para apontar para ele)";
  }

  int files = 0;
  int bpp16 = 0;
  int bpp32 = 0;
  int top_down = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".bmp") continue;
    const std::vector<uint8_t> bytes = ReadFile(entry.path());
    ASSERT_FALSE(bytes.empty()) << entry.path();
    int w = 0, h = 0;
    auto decoded = DecodeBmp(bytes.data(), bytes.size(), w, h);
    ASSERT_TRUE(decoded.has_value()) << "nao decodificou: " << entry.path();
    ASSERT_GT(w, 0) << entry.path();
    ASSERT_GT(h, 0) << entry.path();
    ASSERT_EQ(decoded->size(), static_cast<size_t>(w) * h * 4u) << entry.path();

    // Cabecalho DIB de 40 bytes: bpp no offset 28 do arquivo, altura no
    // offset 22 (negativa = top-down real).
    const int bpp = bytes[28] | (bytes[29] << 8);
    const int32_t raw_height = static_cast<int32_t>(static_cast<uint32_t>(bytes[22]) |
                                                   (static_cast<uint32_t>(bytes[23]) << 8) |
                                                   (static_cast<uint32_t>(bytes[24]) << 16) |
                                                   (static_cast<uint32_t>(bytes[25]) << 24));
    if (bpp == 16) ++bpp16;
    if (bpp == 32) ++bpp32;
    if (raw_height < 0) ++top_down;

    // Nenhum BMP real deste corpus tem alfa util (nem os de 32 bpp, cujo
    // 4o byte e 0): todo pixel tem que sair opaco, e a imagem nao pode
    // ser toda preta/transparente.
    size_t non_opaque = 0;
    size_t non_zero_pixels = 0;
    for (size_t i = 0; i + 3 < decoded->size(); i += 4) {
      if ((*decoded)[i + 3] != 255) ++non_opaque;
      if ((*decoded)[i] != 0 || (*decoded)[i + 1] != 0 || (*decoded)[i + 2] != 0) ++non_zero_pixels;
    }
    EXPECT_EQ(non_opaque, 0u) << entry.path();
    EXPECT_GT(non_zero_pixels, 0u) << entry.path();
    ++files;
  }
  EXPECT_GT(files, 0);
  std::cout << "[corpus bmp] " << files << " arquivos, 16bpp=" << bpp16 << ", 32bpp=" << bpp32
            << ", top-down=" << top_down << "\n";
}
