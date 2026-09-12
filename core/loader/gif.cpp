#include "core/loader/gif.h"

#include <cstring>

namespace zeebulator {
namespace {

// Leitor sequencial com checagem de limite em cada leitura: qualquer
// estouro vira erro explicito (`ok_ = false`), nunca leitura fora do
// buffer nem valor neutro silencioso.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }
  size_t offset() const { return pos_; }
  bool Remaining(size_t n) const { return ok_ && pos_ + n <= size_; }

  uint8_t U8() {
    if (!Remaining(1)) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }

  // Todos os inteiros de 16 bits do GIF sao little-endian.
  uint16_t U16() {
    const uint8_t lo = U8();
    const uint8_t hi = U8();
    return static_cast<uint16_t>(lo | (hi << 8));
  }

  // Copia `n` bytes crus (usado para as tabelas de cores).
  bool Bytes(uint8_t* dst, size_t n) {
    if (!Remaining(n)) {
      ok_ = false;
      return false;
    }
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
    return true;
  }

  bool Skip(size_t n) {
    if (!Remaining(n)) {
      ok_ = false;
      return false;
    }
    pos_ += n;
    return true;
  }

  // Le uma cadeia de sub-blocos (cada um: 1 byte de tamanho + bytes),
  // terminada pelo bloco de tamanho zero. Um codigo LZW pode atravessar
  // a fronteira entre dois sub-blocos, por isso eles sao concatenados
  // antes de qualquer decodificacao.
  bool SubBlocks(std::vector<uint8_t>& out) {
    for (;;) {
      const uint8_t len = U8();
      if (!ok_) return false;
      if (len == 0) return true;
      if (!Remaining(len)) {
        ok_ = false;
        return false;
      }
      out.insert(out.end(), data_ + pos_, data_ + pos_ + len);
      pos_ += len;
    }
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// Decodifica o fluxo LZW do GIF em indices de paleta.
// `min_code_size` vem do byte que precede os sub-blocos de dados.
// Devolve false em fluxo invalido (codigo desconhecido, dicionario
// estourado, pixels de menos).
bool DecodeLzw(const std::vector<uint8_t>& in, int min_code_size, size_t expected_pixels,
               std::vector<uint8_t>& out) {
  // O spec permite min_code_size de 2 a 8 (paletas de 4 a 256 cores).
  // Fora disso o arquivo esta corrompido -- erro explicito.
  if (min_code_size < 2 || min_code_size > 8) return false;

  const int clear_code = 1 << min_code_size;
  const int end_code = clear_code + 1;
  const int kMaxCodes = 4096;  // 12 bits e o teto do formato

  // Dicionario como prefixo+sufixo (sem alocar strings): cada entrada
  // aponta para a entrada anterior, o que evita copias quadraticas.
  std::vector<int> prefix(kMaxCodes, -1);
  std::vector<uint8_t> suffix(kMaxCodes, 0);
  for (int i = 0; i < clear_code; ++i) suffix[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

  int code_size = min_code_size + 1;
  int next_code = end_code + 1;
  int prev_code = -1;

  out.clear();
  out.reserve(expected_pixels);

  std::vector<uint8_t> stack;
  stack.reserve(kMaxCodes);

  size_t bit_pos = 0;
  const size_t total_bits = in.size() * 8;

  for (;;) {
    if (bit_pos + static_cast<size_t>(code_size) > total_bits) {
      // Fim dos bytes sem End Of Information. Alguns codificadores
      // reais omitem o EOI; so aceitamos isso se todos os pixels ja
      // sairam, senao e arquivo truncado -> erro.
      break;
    }
    // Codigos do GIF sao LSB-first e atravessam bytes livremente.
    int code = 0;
    for (int bit = 0; bit < code_size; ++bit) {
      const size_t abs_bit = bit_pos + static_cast<size_t>(bit);
      const int bit_value = (in[abs_bit >> 3] >> (abs_bit & 7)) & 1;
      code |= bit_value << bit;
    }
    bit_pos += static_cast<size_t>(code_size);

    if (code == clear_code) {
      // Clear Code: volta ao tamanho de codigo inicial e esquece tudo o
      // que foi aprendido. Pode aparecer varias vezes no meio do fluxo.
      code_size = min_code_size + 1;
      next_code = end_code + 1;
      prev_code = -1;
      continue;
    }
    if (code == end_code) break;

    if (code > next_code || code == end_code) return false;
    // Caso "KwKwK": o codificador pode emitir exatamente o codigo que
    // esta sendo criado agora; a expansao correta e prev + prev[0].
    const bool deferred = (code == next_code);
    if (deferred && prev_code < 0) return false;

    stack.clear();
    int current = deferred ? prev_code : code;
    // Desenrola a cadeia prefixo->sufixo; o resultado sai invertido.
    int guard = 0;
    while (current >= 0) {
      if (++guard > kMaxCodes) return false;  // cadeia circular = corrompido
      stack.push_back(suffix[static_cast<size_t>(current)]);
      if (current < clear_code) break;
      current = prefix[static_cast<size_t>(current)];
      if (current < 0) return false;
    }
    const uint8_t first_byte = stack.back();
    for (size_t i = stack.size(); i > 0; --i) out.push_back(stack[i - 1]);
    if (deferred) out.push_back(first_byte);

    if (prev_code >= 0 && next_code < kMaxCodes) {
      prefix[static_cast<size_t>(next_code)] = prev_code;
      suffix[static_cast<size_t>(next_code)] = first_byte;
      ++next_code;
      // Crescer quando o dicionario ATINGE 1<<code_size (e nunca passar
      // de 12 bits): errar esse limiar por um desloca todos os codigos
      // seguintes e produz lixo.
      if (next_code == (1 << code_size) && code_size < 12) ++code_size;
    }
    prev_code = code;

    if (out.size() > expected_pixels) return false;  // dados demais = corrompido
  }

  // Pixels de menos e erro: nao preenchemos o resto com um valor neutro.
  return out.size() == expected_pixels;
}

// Ordem real das linhas quando o bit de interlace do Image Descriptor
// esta ligado: 4 passadas com inicio/passo fixos.
int InterlacedRow(int decoded_row, int height) {
  int row = decoded_row;
  // Passada 1: linhas 0, 8, 16, ...
  const int p1 = (height + 7) / 8;
  if (row < p1) return row * 8;
  row -= p1;
  // Passada 2: linhas 4, 12, 20, ...
  const int p2 = (height + 7 - 4) / 8;
  if (row < p2) return row * 8 + 4;
  row -= p2;
  // Passada 3: linhas 2, 6, 10, ...
  const int p3 = (height + 3 - 2) / 4;
  if (row < p3) return row * 4 + 2;
  row -= p3;
  // Passada 4: linhas 1, 3, 5, ...
  return row * 2 + 1;
}

}  // namespace

std::optional<GifImage> DecodeGif(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 13) return std::nullopt;
  // Assinatura + versao: so "GIF87a" e "GIF89a" existem de verdade.
  if (std::memcmp(data, "GIF87a", 6) != 0 && std::memcmp(data, "GIF89a", 6) != 0) {
    return std::nullopt;
  }

  Reader r(data, size);
  r.Skip(6);

  GifImage image;
  image.width = r.U16();
  image.height = r.U16();
  const uint8_t packed = r.U8();
  image.background_index = r.U8();
  r.U8();  // pixel aspect ratio: ignorado (0 em todo sample real visto)
  if (!r.ok()) return std::nullopt;
  if (image.width <= 0 || image.height <= 0) return std::nullopt;

  std::vector<uint8_t> global_table;
  if (packed & 0x80) {
    // Tamanho e 2 << (packed & 7) entradas, 3 bytes cada.
    const size_t entries = static_cast<size_t>(2) << (packed & 7);
    global_table.resize(entries * 3);
    if (!r.Bytes(global_table.data(), global_table.size())) return std::nullopt;
    image.has_global_color_table = true;
  }

  // Estado da ultima Graphic Control Extension lida; vale so para o
  // proximo Image Descriptor e e zerado depois de usado.
  bool have_gce = false;
  int gce_delay = 0;
  int gce_disposal = 0;
  int gce_transparent = -1;

  for (;;) {
    const uint8_t block = r.U8();
    if (!r.ok()) return std::nullopt;  // acabou o arquivo sem trailer

    if (block == 0x3B) break;  // trailer

    if (block == 0x21) {  // Extension Introducer
      const uint8_t label = r.U8();
      if (!r.ok()) return std::nullopt;
      std::vector<uint8_t> payload;
      if (label == 0xF9) {
        // Graphic Control Extension: tamanho fixo 4 nos arquivos reais,
        // mas ainda assim vem em sub-blocos.
        if (!r.SubBlocks(payload)) return std::nullopt;
        if (payload.size() < 4) return std::nullopt;
        const uint8_t flags = payload[0];
        gce_disposal = (flags >> 2) & 0x07;
        gce_transparent = (flags & 0x01) ? static_cast<int>(payload[3]) : -1;
        gce_delay = payload[1] | (payload[2] << 8);
        have_gce = true;
      } else if (label == 0xFF) {
        // Application Extension: so nos interessa o NETSCAPE2.0, que
        // carrega o numero de repeticoes da animacao.
        if (!r.SubBlocks(payload)) return std::nullopt;
        if (payload.size() >= 14 && std::memcmp(payload.data(), "NETSCAPE2.0", 11) == 0 &&
            payload[11] == 0x01) {
          image.loop_count = payload[12] | (payload[13] << 8);
        }
      } else {
        // Comment (0xFE), Plain Text (0x01) e qualquer outra extensao:
        // pulamos os sub-blocos, mas validando os tamanhos.
        if (!r.SubBlocks(payload)) return std::nullopt;
      }
      continue;
    }

    if (block != 0x2C) return std::nullopt;  // byte de bloco desconhecido

    // Image Descriptor.
    GifFrame frame;
    frame.x = r.U16();
    frame.y = r.U16();
    frame.width = r.U16();
    frame.height = r.U16();
    const uint8_t ipacked = r.U8();
    if (!r.ok()) return std::nullopt;
    if (frame.width <= 0 || frame.height <= 0) return std::nullopt;

    std::vector<uint8_t> local_table;
    if (ipacked & 0x80) {
      const size_t entries = static_cast<size_t>(2) << (ipacked & 7);
      local_table.resize(entries * 3);
      if (!r.Bytes(local_table.data(), local_table.size())) return std::nullopt;
    }
    // A tabela local, quando existe, substitui a global inteira neste
    // frame (nao se mistura com ela).
    const std::vector<uint8_t>& table = local_table.empty() ? global_table : local_table;
    if (table.empty()) return std::nullopt;  // frame sem paleta nenhuma

    const bool interlaced = (ipacked & 0x40) != 0;

    const uint8_t min_code_size = r.U8();
    std::vector<uint8_t> lzw_data;
    if (!r.SubBlocks(lzw_data)) return std::nullopt;

    const size_t pixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    std::vector<uint8_t> indices;
    if (!DecodeLzw(lzw_data, min_code_size, pixels, indices)) return std::nullopt;

    if (have_gce) {
      frame.delay_cs = gce_delay;
      frame.disposal_method = gce_disposal;
      frame.transparent_index = gce_transparent;
    }

    const size_t table_entries = table.size() / 3;
    frame.rgba.assign(pixels * 4, 0);
    for (int decoded_row = 0; decoded_row < frame.height; ++decoded_row) {
      const int dst_row = interlaced ? InterlacedRow(decoded_row, frame.height) : decoded_row;
      if (dst_row >= frame.height) return std::nullopt;  // calculo de passada inconsistente
      for (int x = 0; x < frame.width; ++x) {
        const size_t src = static_cast<size_t>(decoded_row) * frame.width + x;
        const size_t idx = indices[src];
        // Indice fora da paleta e arquivo invalido, nao um pixel preto.
        if (idx >= table_entries) return std::nullopt;
        const size_t dst = (static_cast<size_t>(dst_row) * frame.width + x) * 4;
        frame.rgba[dst + 0] = table[idx * 3 + 0];
        frame.rgba[dst + 1] = table[idx * 3 + 1];
        frame.rgba[dst + 2] = table[idx * 3 + 2];
        // Transparencia: mantemos a cor da paleta e zeramos so o alpha,
        // igual a convencao ja usada pelo `Obm1Image`.
        frame.rgba[dst + 3] =
            (frame.transparent_index >= 0 && static_cast<int>(idx) == frame.transparent_index)
                ? 0
                : 255;
      }
    }

    image.frames.push_back(std::move(frame));

    have_gce = false;
    gce_delay = 0;
    gce_disposal = 0;
    gce_transparent = -1;
  }

  // Um GIF sem nenhum frame nao e utilizavel por ninguem aqui: erro.
  if (image.frames.empty()) return std::nullopt;
  return image;
}

std::optional<std::vector<uint8_t>> DecodeGif(const uint8_t* data, size_t size, int& out_width,
                                              int& out_height) {
  auto decoded = DecodeGif(data, size);
  if (!decoded.has_value() || decoded->frames.empty()) return std::nullopt;

  const GifFrame& frame = decoded->frames.front();
  // Canvas da tela logica comeca totalmente transparente; o primeiro
  // frame pode cobrir so um retangulo dela.
  std::vector<uint8_t> canvas(static_cast<size_t>(decoded->width) * decoded->height * 4, 0);
  for (int y = 0; y < frame.height; ++y) {
    const int dst_y = frame.y + y;
    if (dst_y < 0 || dst_y >= decoded->height) continue;
    for (int x = 0; x < frame.width; ++x) {
      const int dst_x = frame.x + x;
      if (dst_x < 0 || dst_x >= decoded->width) continue;
      const size_t src = (static_cast<size_t>(y) * frame.width + x) * 4;
      const size_t dst = (static_cast<size_t>(dst_y) * decoded->width + dst_x) * 4;
      std::memcpy(&canvas[dst], &frame.rgba[src], 4);
    }
  }

  out_width = decoded->width;
  out_height = decoded->height;
  return canvas;
}

}  // namespace zeebulator
