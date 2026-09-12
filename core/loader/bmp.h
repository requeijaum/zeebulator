#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zeebulator {

// Decodifica um BMP real do Windows (BITMAPFILEHEADER "BM" + cabecalho
// DIB), o formato que os recursos de imagem do Z-Wheel/Tectoy usam.
//
// Formato confirmado direto contra bytes reais, nao chutado: o payload
// de um recurso de imagem dos BRFs do console tem o layout
//   [u16 tamanho_do_cabecalho][string MIME terminada em NUL][bytes da imagem]
// e os recursos reais de `tectoy_pt.brf` (ids 5005, 5007, 5026 e 5103,
// tipo 6) dizem "image/bmp" e trazem bytes que comecam com "BM", com
// BITMAPINFOHEADER de 40 bytes, 24 bpp, compressao BI_RGB (0) e linhas
// em ordem bottom-up (o maior mede 214x34). Ver `core/brew/ishell.cpp`,
// que ja mapeia o MIME "image/bmp" para o CLSID do Windows Bitmap.
// Esta funcao recebe SO os bytes da imagem: quem resolve o recurso e
// quem tira o u16 de cabecalho e a string MIME da frente.
//
// Suporte deste decodificador:
//  - BITMAPINFOHEADER (biSize 40, e tambem os cabecalhos maiores da
//    mesma familia, >= 40) e BITMAPCOREHEADER (biSize 12);
//  - 8 bpp com paleta, 16 bpp (555 por padrao em BI_RGB, ou as mascaras
//    de BI_BITFIELDS: 565 ou qualquer mascara contigua declarada),
//    24 bpp e 32 bpp;
//  - altura negativa = linhas em ordem top-down.
// 1 bpp e 4 bpp SEM compressao, RLE4 (BI_RLE4), RLE8 (BI_RLE8),
// BI_JPEG e BI_PNG devolvem `std::nullopt`: erro explicito, nunca
// imagem parcial ou preta (nenhum recurso real medido usa esses
// modos; se um aparecer, o chamador descobre na hora em vez de
// receber pixels errados).
//
// Armadilhas reais do formato que este decodificador trata
// explicitamente:
//  - cada LINHA de pixels e alinhada em 4 bytes: o padding no fim da
//    linha nao e pixel e nao pode entrar na imagem (o arquivo real de
//    214x34 tem linhas de 642 bytes gravadas com 644, e `biSizeImage`
//    vem 2 bytes MAIOR que largura*altura calculado por linha, entao o
//    tamanho do pixel data e derivado de largura/altura/bpp/alinhamento,
//    nunca do campo `biSizeImage` nem de `bfSize`);
//  - `bfSize` (u32 no offset 2) e ignorado: e o campo que os
//    gravadores de BMP mais erram, enquanto `bfOffBits` (offset 10) e
//    quem de fato diz onde o pixel data comeca;
//  - ordem das linhas: `biHeight` positivo = bottom-up (o padrao, e o
//    que os recursos reais do console usam), negativo = top-down;
//  - entradas de paleta tem 4 bytes no BITMAPINFOHEADER (BGRX) e 3
//    bytes (BGR) no BITMAPCOREHEADER -- ler 4 bytes de paleta no
//    cabecalho CORE desalinha a paleta inteira;
//  - 32 bpp em BI_RGB nao tem alfa definido (a GDI ignora o 4o byte,
//    que ferramentas gravam como 0): aqui o alfa sai 255 (opaco), senao
//    todo BMP de 32 bpp viraria uma imagem totalmente transparente;
//  - 16 bpp em BI_RGB nao traz mascara nenhuma; a convencao do formato
//    e 555 (0x7C00/0x03E0/0x001F). 565 so aparece com BI_BITFIELDS,
//    e as tres mascaras ficam nos 12 bytes IMEDIATAMENTE apos o
//    BITMAPINFOHEADER de 40 bytes (offset 54 do arquivo);
//  - extensao de canal: 5 bits viram 8 por replicacao arredondada
//    (v*255 + max/2)/max, que leva 31 a 255 e nao a 248.
//
// Evidencia medida no corpus real deste projeto (os 65 .bmp do corpus
// `debug_nand`: 42 de 24 bpp, 10 de 16 bpp e 13 de 32 bpp, todos
// BITMAPINFOHEADER de 40 bytes e BI_RGB, 10 deles com altura negativa):
// a saida deste decodificador foi comparada pixel a pixel com um
// decodificador independente (Pillow) -- 24 bpp e 32 bpp batem
// exatamente, e 16 bpp bate com 555 (diferenca maxima de 1 unidade, so
// arredondamento de 5 para 8 bits; uma leitura 565 diferiria em ate
// 132). Os 13 arquivos de 32 bpp tem o 4o byte 0 em TODOS os pixels: e
// o caso em que ler alfa literal devolveria 13 imagens transparentes.
//
// Saida: RGBA8888, 4 bytes por pixel, row-major, TOPO PRIMEIRO (linha 0
// da saida = linha de cima da imagem) -- o mesmo layout que `DecodePng`,
// `DecodeTga`, `DecodeGif` e `DecodeAtitc` ja usam neste projeto, entao
// nenhum chamador precisa de um caminho especifico de formato depois de
// decodificar. O alfa e 255 para 8/16/24 bpp e para 32 bpp em BI_RGB;
// so um 32 bpp com mascara de alfa explicita produz alfa diferente.
//
// Em qualquer arquivo malformado (ponteiro nulo, assinatura errada,
// cabecalho truncado, bpp ou compressao nao suportados, paleta
// truncada, indice de paleta fora da faixa, mascara invalida ou pixel
// data insuficiente) a funcao devolve `std::nullopt`: erro e sempre
// explicito e detectavel, nunca uma imagem vazia, preta ou parcial.
std::optional<std::vector<uint8_t>> DecodeBmp(const uint8_t* data, size_t size, int& out_width,
                                               int& out_height);

// Conveniencia igual a dos outros loaders: mesma decodificacao, sem
// devolver as dimensoes por referencia.
std::optional<std::vector<uint8_t>> DecodeBmp(const uint8_t* data, size_t size);

}  // namespace zeebulator
