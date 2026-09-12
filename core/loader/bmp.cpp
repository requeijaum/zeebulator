#include "core/loader/bmp.h"

namespace zeebulator {
namespace {

// Valores do campo biCompression (BITMAPINFOHEADER).
constexpr uint32_t kBiRgb = 0;
constexpr uint32_t kBiRle8 = 1;
constexpr uint32_t kBiRle4 = 2;
constexpr uint32_t kBiBitfields = 3;

// Cabecalho DIB do BMP "core" (OS/2 1.x) e o minimo do Windows.
constexpr uint32_t kCoreHeaderSize = 12;
constexpr uint32_t kInfoHeaderMinSize = 40;

// Todos os inteiros do BMP sao little-endian. Leitura sem checagem de
// limite: quem chama ja garantiu que os bytes existem.
uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Uma mascara de canal decomposta em deslocamento e numero de bits.
struct ChannelMask {
  bool ok = false;  // mascara valida (bloco contiguo de bits)
  int shift = 0;
  int bits = 0;
};

// O BMP nao tem canal esparso: uma mascara so vale se for um bloco
// CONTIGUO de bits que caiba nos 32 bits do pixel. Mascara torta e
// tratada como erro pelo chamador, nunca como canal zero silencioso.
ChannelMask AnalyzeMask(uint32_t mask) {
  ChannelMask out;
  if (mask == 0) return out;
  int shift = 0;
  while (((mask >> shift) & 1u) == 0) ++shift;
  const uint32_t run = mask >> shift;
  // Contiguo <=> run & (run + 1) == 0 (o primeiro zero depois do bloco).
  if ((run & (run + 1u)) != 0) return out;
  int bits = 0;
  while (((run >> bits) & 1u) != 0) ++bits;
  if (shift + bits > 32) return out;
  out.ok = true;
  out.shift = shift;
  out.bits = bits;
  return out;
}

// Extrai um canal do pixel e o expande para 8 bits. 5 bits viram 8 por
// expansao arredondada (raw*255 + max/2)/max: leva 31 a 255, enquanto o
// shift simples (raw << 3) levaria a 248 e escureceria todo BMP 16 bpp.
uint8_t ExtractChannel(uint32_t pixel, const ChannelMask& mask) {
  if (!mask.ok || mask.bits == 0) return 0;
  const uint32_t raw = mask.bits >= 32
                           ? (pixel >> mask.shift)
                           : ((pixel >> mask.shift) & ((1u << mask.bits) - 1u));
  if (mask.bits > 8) return static_cast<uint8_t>(raw >> (mask.bits - 8));
  if (mask.bits == 8) return static_cast<uint8_t>(raw);
  const uint32_t max = (1u << mask.bits) - 1u;
  return static_cast<uint8_t>((raw * 255u + max / 2u) / max);
}

std::optional<std::vector<uint8_t>> DecodeBmpImpl(const uint8_t* data, size_t size,
                                                 int& out_width, int& out_height) {
  // Ponteiro nulo e buffer menor que o BITMAPFILEHEADER: erro explicito.
  if (data == nullptr || size < 14) return std::nullopt;
  if (data[0] != 'B' || data[1] != 'M') return std::nullopt;

  // `bfSize` (offset 2) e ignorado de proposito: e o campo que os
  // gravadores de BMP mais erram, enquanto `bfOffBits` (offset 10) e
  // quem de fato diz onde o pixel data comeca. Um `bfOffBits` que nao
  // caia dentro do arquivo e erro, nao chute.
  const uint32_t pixel_offset = ReadU32(data + 10);
  if (pixel_offset < 14 || pixel_offset > size) return std::nullopt;

  // Tamanho do cabecalho DIB nos 4 bytes seguintes ao file header.
  if (size < 18) return std::nullopt;
  const uint8_t* dib = data + 14;
  const size_t dib_available = size - 14;
  const uint32_t dib_size = ReadU32(dib);

  const bool is_core_header = dib_size == kCoreHeaderSize;
  if (is_core_header) {
    if (dib_available < kCoreHeaderSize) return std::nullopt;  // truncado
  } else if (dib_size >= kInfoHeaderMinSize) {
    // Cabecalho maior (V4/V5) tambem e aceito, mas os bytes declarados
    // precisam existir: `dib_size` maior que o buffer = truncado.
    if (dib_size > dib_available) return std::nullopt;
  } else {
    return std::nullopt;  // 16/20/24/... nao sao cabecalhos DIB reais
  }

  int64_t width = 0;
  int64_t height = 0;
  bool top_down = false;
  uint16_t bpp = 0;
  uint32_t compression = kBiRgb;
  uint32_t clr_used = 0;

  if (is_core_header) {
    // BITMAPCOREHEADER: largura/altura sao u16 e NAO existe campo de
    // compressao -- o formato e sempre BI_RGB e sempre bottom-up.
    width = ReadU16(dib + 4);
    height = ReadU16(dib + 6);
    bpp = ReadU16(dib + 10);
  } else {
    const int32_t raw_width = static_cast<int32_t>(ReadU32(dib + 4));
    const int32_t raw_height = static_cast<int32_t>(ReadU32(dib + 8));
    width = raw_width;
    // Altura NEGATIVA = linhas em ordem top-down (o sinal e o unico
    // lugar do formato que diz isso). `-INT32_MIN` nao cabe em int32,
    // por isso a conversao para int64 antes do modulo.
    top_down = raw_height < 0;
    height = top_down ? -static_cast<int64_t>(raw_height) : raw_height;
    bpp = ReadU16(dib + 14);
    compression = ReadU32(dib + 16);
    clr_used = ReadU32(dib + 32);
  }

  const uint16_t planes = ReadU16(is_core_header ? dib + 8 : dib + 12);
  if (planes != 1) return std::nullopt;
  if (width <= 0 || height <= 0) return std::nullopt;
  if (width > 0x7fffffff || height > 0x7fffffff) return std::nullopt;

  // bpp fora destes quatro nao e suportado: 1 bpp e 4 bpp sem RLE
  // devolvem erro explicito em vez de uma imagem preta.
  if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return std::nullopt;

  if (!is_core_header) {
    // RLE4/RLE8 nao implementados: erro explicito, nunca pixel parcial.
    if (compression == kBiRle8 || compression == kBiRle4) return std::nullopt;
    if (compression == kBiBitfields && bpp != 16 && bpp != 32) return std::nullopt;
    if (compression != kBiRgb && compression != kBiBitfields) {
      return std::nullopt;  // BI_JPEG/BI_PNG/desconhecida
    }
  }

  // ---- Paleta (so 8 bpp) ----
  // Armadilha do formato: a entrada de paleta tem 4 bytes (BGRX) no
  // BITMAPINFOHEADER e 3 bytes (BGR) no BITMAPCOREHEADER. Ler 4 bytes
  // do cabecalho "core" desalinha a paleta inteira.
  const size_t palette_entry_bytes = is_core_header ? 3u : 4u;
  const size_t palette_start = 14u + dib_size;
  size_t palette_entries = 0;
  if (bpp == 8) {
    // O BITMAPCOREHEADER nao tem biClrUsed: o formato fixa a paleta em
    // 1 << bpp entradas. No BITMAPINFOHEADER, biClrUsed = 0 significa
    // "use 1 << bpp".
    palette_entries = clr_used != 0 ? clr_used : size_t{1} << bpp;
    // biClrUsed maior que o bpp comporta e arquivo incoerente.
    if (palette_entries > (size_t{1} << bpp)) return std::nullopt;
    // A paleta tem que caber ANTES do pixel data (o `bfOffBits` real e
    // quem diz onde ele comeca); se `bfOffBits` apontar para dentro do
    // cabecalho a paleta so precisa caber no arquivo.
    const size_t palette_limit = pixel_offset >= palette_start ? pixel_offset : size;
    if (palette_start + palette_entries * palette_entry_bytes > palette_limit) {
      return std::nullopt;  // paleta truncada
    }
  }

  // ---- Mascaras de canal (16 e 32 bpp) ----
  ChannelMask r_mask;
  ChannelMask g_mask;
  ChannelMask b_mask;
  ChannelMask a_mask;
  if (bpp == 16 || bpp == 32) {
    if (!is_core_header && compression == kBiBitfields) {
      // As tres mascaras RGB ficam imediatamente apos o BITMAPINFOHEADER
      // de 40 bytes (offset 54 do arquivo). Nos cabecalhos maiores
      // (V4/V5, >= 52 bytes) elas ja estao dentro do proprio cabecalho,
      // nos MESMOS offsets absolutos -- por isso `dib_size >= 52` usa 40.
      const size_t masks_offset = dib_size >= 52 ? 40u : dib_size;
      if (masks_offset + 12u > dib_available) return std::nullopt;
      const uint32_t raw_r = ReadU32(dib + masks_offset);
      const uint32_t raw_g = ReadU32(dib + masks_offset + 4);
      const uint32_t raw_b = ReadU32(dib + masks_offset + 8);
      if ((raw_r & raw_g) != 0 || (raw_r & raw_b) != 0 || (raw_g & raw_b) != 0) {
        return std::nullopt;  // canais sobrepostos = mascara incoerente
      }
      r_mask = AnalyzeMask(raw_r);
      g_mask = AnalyzeMask(raw_g);
      b_mask = AnalyzeMask(raw_b);
      if (!r_mask.ok || !g_mask.ok || !b_mask.ok) return std::nullopt;
    } else if (bpp == 16) {
      // BI_RGB de 16 bpp nao traz mascara nenhuma gravada; a convencao
      // do formato e 555 (0x7C00/0x03E0/0x001F). 565 so aparece com
      // BI_BITFIELDS, por isso nao se "adivinha" 565 aqui.
      r_mask = AnalyzeMask(0x7c00u);
      g_mask = AnalyzeMask(0x03e0u);
      b_mask = AnalyzeMask(0x001fu);
    } else {
      // 32 bpp BI_RGB: BGRX. O 4o byte nao tem alfa definido (a GDI
      // ignora o que as ferramentas gravam ali, normalmente 0), entao o
      // canal nao e lido como alfa: o alfa sai 255 (opaco).
      r_mask = AnalyzeMask(0x00ff0000u);
      g_mask = AnalyzeMask(0x0000ff00u);
      b_mask = AnalyzeMask(0x000000ffu);
    }
    // Cabecalho V4/V5 (>= 56 bytes) tem uma QUARTA mascara de alfa
    // dentro do cabecalho, no offset 52. Em BITMAPINFOHEADER de 40
    // bytes ela nao existe: os 12 bytes seguintes sao as mascaras RGB e
    // depois deles vem paleta/pixels -- ler alfa dali significaria ler
    // pixel como alfa, entao so se le quando o cabecalho e grande.
    if (dib_size >= 56 && compression == kBiBitfields) {
      const ChannelMask candidate = AnalyzeMask(ReadU32(dib + 52));
      // Mascara 0 ou torta = "sem canal de alfa" (imagem opaca).
      if (candidate.ok) a_mask = candidate;
    }
  }

  // ---- Geometria do pixel data ----
  // Cada LINHA e alinhada em 4 bytes: o padding do fim da linha nao e
  // pixel. O tamanho vem de largura/altura/bpp/alinhamento, NUNCA do
  // campo `biSizeImage` (no recurso real de 214x34 ele vem 2 bytes
  // maior que o calculado, e um monte de BMP real o grava como 0).
  const uint64_t row_bytes = static_cast<uint64_t>(width) * (bpp / 8u);
  const uint64_t stride = (row_bytes + 3u) / 4u * 4u;
  const uint64_t needed = stride * static_cast<uint64_t>(height);
  if (needed > static_cast<uint64_t>(size - pixel_offset)) return std::nullopt;

  const uint8_t* pixels = data + pixel_offset;
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

  for (int64_t y = 0; y < height; ++y) {
    // Saida sempre TOPO PRIMEIRO: linha 0 da saida = linha de cima da
    // imagem. Bottom-up (biHeight positivo, o padrao) inverte.
    const int64_t src_row = top_down ? y : (height - 1 - y);
    const uint8_t* row = pixels + static_cast<size_t>(src_row) * static_cast<size_t>(stride);
    for (int64_t x = 0; x < width; ++x) {
      const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)) * 4u;
      if (bpp == 8) {
        const uint8_t index = row[x];
        // Indice fora da paleta e erro explicito. Nunca um pixel preto
        // silencioso, que e o sintoma classico de decodificador BMP
        // solto no mundo real.
        if (index >= palette_entries) return std::nullopt;
        const uint8_t* entry = data + palette_start + static_cast<size_t>(index) * palette_entry_bytes;
        rgba[dst + 0] = entry[2];  // R
        rgba[dst + 1] = entry[1];  // G
        rgba[dst + 2] = entry[0];  // B
        rgba[dst + 3] = 255;
      } else if (bpp == 16) {
        const uint32_t px = ReadU16(row + x * 2);
        rgba[dst + 0] = ExtractChannel(px, r_mask);
        rgba[dst + 1] = ExtractChannel(px, g_mask);
        rgba[dst + 2] = ExtractChannel(px, b_mask);
        rgba[dst + 3] = a_mask.ok ? ExtractChannel(px, a_mask) : 255;
      } else if (bpp == 24) {
        const uint8_t* p = row + x * 3;
        rgba[dst + 0] = p[2];  // R <- B
        rgba[dst + 1] = p[1];  // G
        rgba[dst + 2] = p[0];  // B <- R
        rgba[dst + 3] = 255;
      } else {  // 32
        const uint32_t px = ReadU32(row + x * 4);
        rgba[dst + 0] = ExtractChannel(px, r_mask);
        rgba[dst + 1] = ExtractChannel(px, g_mask);
        rgba[dst + 2] = ExtractChannel(px, b_mask);
        rgba[dst + 3] = a_mask.ok ? ExtractChannel(px, a_mask) : 255;
      }
    }
  }

  out_width = static_cast<int>(width);
  out_height = static_cast<int>(height);
  return rgba;
}

}  // namespace

std::optional<std::vector<uint8_t>> DecodeBmp(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height) {
  return DecodeBmpImpl(data, size, out_width, out_height);
}

std::optional<std::vector<uint8_t>> DecodeBmp(const uint8_t* data, size_t size) {
  int width = 0;
  int height = 0;
  return DecodeBmpImpl(data, size, width, height);
}

}  // namespace zeebulator
