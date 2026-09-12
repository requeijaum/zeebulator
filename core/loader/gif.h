#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

// Decodifica um GIF real (GIF87a ou GIF89a), incluindo LZW, tabela de
// cores global e local, interlace, transparencia via Graphic Control
// Extension e multiplos frames.
//
// Formato confirmado direto contra bytes reais, nao chutado: o asset
// real `mod/274755/opening_low.gif` do corpus `debug_nand` deste
// projeto e um GIF89a de 30123 bytes, tela logica 640x480, byte
// "packed" do Logical Screen Descriptor = 0xF7 (bit7 = tabela global
// presente, tamanho 2<<7 = 256 entradas), um unico Image Descriptor
// 640x480 em (0,0) sem tabela local e sem interlace, LZW com minimum
// code size 8, uma Graphic Control Extension com flags 0 (sem
// transparencia) e o trailer 0x3B no fim.
//
// Armadilhas reais do formato que este decodificador trata
// explicitamente:
//  - o campo "packed" codifica o tamanho da tabela de cores como
//    `2 << (packed & 7)` entradas de 3 bytes (RGB), nunca como um
//    numero direto;
//  - a largura/altura do Image Descriptor pode ser MENOR que a tela
//    logica e ficar deslocada por (left, top): o frame e um retangulo
//    dentro do canvas, nao o canvas inteiro;
//  - o LZW do GIF le os codigos com os bits menos significativos
//    primeiro (LSB-first), ao contrario do LZW do TIFF; o tamanho do
//    codigo cresce de `min_code_size + 1` ate no maximo 12 bits e volta
//    ao inicial a cada Clear Code;
//  - o dicionario cresce quando ele ATINGE `1 << code_size`, e o
//    codificador pode legitimamente emitir o codigo que esta sendo
//    criado naquele instante (caso "KwKwK"), tratado aqui como
//    `prev + prev[0]`;
//  - os dados vem fatiados em sub-blocos de ate 255 bytes terminados
//    por um bloco de tamanho 0; um codigo LZW pode atravessar a
//    fronteira de dois sub-blocos, entao os sub-blocos sao concatenados
//    ANTES de decodificar;
//  - com o bit de interlace ligado, as linhas do frame chegam em 4
//    passadas (inicio 0 passo 8, inicio 4 passo 8, inicio 2 passo 4,
//    inicio 1 passo 2) e precisam ser reordenadas;
//  - a Graphic Control Extension vale para o PROXIMO Image Descriptor e
//    e consumida por ele (nao e global nem acumulativa).
//
// Saida: RGBA8888, 4 bytes por pixel, row-major, topo primeiro -- o
// mesmo layout que `DecodePng`, `DecodeTga` e `DecodeAtitc` ja usam
// neste projeto, entao nenhum chamador precisa de um caminho especifico
// de formato depois de decodificar (RGB565 e usado so pela paleta
// interna do OBM1, nao pela saida de imagem decodificada).
//
// Em qualquer arquivo malformado (assinatura errada, tabela de cores
// truncada, indice de cor fora da paleta, fluxo LZW invalido ou dados
// de pixel insuficientes) a funcao devolve `std::nullopt`: erro e
// sempre explicito e detectavel, nunca uma imagem vazia ou preta.

// Um frame real do GIF, ja com a paleta resolvida.
struct GifFrame {
  int x = 0;       // deslocamento esquerdo dentro da tela logica
  int y = 0;       // deslocamento superior dentro da tela logica
  int width = 0;   // largura do retangulo deste frame
  int height = 0;  // altura do retangulo deste frame
  // Atraso em centesimos de segundo (campo da Graphic Control
  // Extension). 0 quando nao ha GCE para este frame.
  int delay_cs = 0;
  // Disposal method da GCE (bits 2..4): 0 = nao especificado,
  // 1 = manter, 2 = restaurar cor de fundo, 3 = restaurar o anterior.
  int disposal_method = 0;
  // Indice transparente da GCE, ou -1 se este frame nao tem
  // transparencia.
  int transparent_index = -1;
  // width * height * 4 bytes, RGBA8888. Pixels transparentes mantem a
  // cor da paleta e recebem alpha 0 (mesma convencao do `Obm1Image`).
  std::vector<uint8_t> rgba;
};

struct GifImage {
  int width = 0;   // tela logica (Logical Screen Descriptor)
  int height = 0;
  int background_index = 0;
  bool has_global_color_table = false;
  // Numero de repeticoes da extensao NETSCAPE2.0 (0 = repetir para
  // sempre); -1 quando o arquivo nao traz essa extensao.
  int loop_count = -1;
  std::vector<GifFrame> frames;
};

// Decodifica o arquivo inteiro (todos os frames). Devolve nullopt em
// qualquer erro de formato.
std::optional<GifImage> DecodeGif(const uint8_t* data, size_t size);

// Conveniencia com a mesma assinatura de `DecodePng`/`DecodeTga`:
// devolve o PRIMEIRO frame ja composto sobre a tela logica inteira
// (`out_width` x `out_height` = tela logica), em RGBA8888. As areas do
// canvas que o primeiro frame nao cobre ficam totalmente transparentes
// (0,0,0,0). Devolve nullopt em erro de formato ou se o arquivo nao tem
// nenhum frame.
std::optional<std::vector<uint8_t>> DecodeGif(const uint8_t* data, size_t size, int& out_width,
                                              int& out_height);

}  // namespace zeebulator
