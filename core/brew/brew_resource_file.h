#pragma once

// Arquivo de recurso do BREW: o `.brf` que fica ao lado do `.mod` (no corpus
// do Zeebo, `mod/<id>/<nome>_pt.brf`, `<nome>li.brf`, ...). E o container que o
// `IShell_LoadResString`/`LoadResData`/`LoadResDataEx` leem.
//
// O formato NAO esta nos headers que temos (nem em AEEIShell.h, nem em
// AEEResFile.h: eles declaram a interface, nao o container). O layout abaixo
// foi MEDIDO byte a byte em dois arquivos reais de mod/274755
// (`tectoy_pt.brf`, 242668 bytes, e `tectoyli.brf`, 1439595 bytes) e fecha nos
// dois em todos os campos:
//
//   +0x00 u16  versao            (0x11 nos dois)
//   +0x02 u16  1
//   +0x04 u16  1
//   +0x06 u16  nRegistros        (46 / 34)
//   +0x08 u32  inicio dos registros (0x20 nos dois)
//   +0x0c u32  tamanho da area de registros (== nRegistros*8)   (0x170 / 0x110)
//   +0x10 u32  inicio da tabela de deslocamentos (== 0x20 + 0x0c) (0x190 / 0x130)
//   +0x14 u32  nDeslocamentos    (209 / 85)
//   +0x18 u32  inicio dos dados  (0x4d8 / 0x288)
//   +0x1c u32  fim dos dados     (0x3aff6 / 0x194651)
//   +0x20       nRegistros registros de 8 bytes:
//               {u16 tipo, u16 id, u16 campo desconhecido, u16 INDICE}
//
//   O INDICE e o ULTIMO u16 do registro, nao o terceiro. Foi lido errado uma
//   vez (o terceiro campo), e o efeito e silencioso: o recurso achado e outro.
//   A prova esta nos dados: id 1002 (tipo 1) tem ultimo campo 9 e a tabela[9]
//   e a string "Nao e possivel inicializar."; os ids de imagem (tipo 6) tem
//   ultimo campo 0/1/2/5 e a tabela daqueles indices aponta blocos que comecam
//   com o MIME "image/bmp" e o cabecalho "BM". O terceiro u16 (0, 2, ...)
//   permanece DESCONHECIDO -- parece agrupamento/ordem, e nao e usado aqui.
//
//   tabela de deslocamentos: u32 ABSOLUTOS, um por recurso; o recurso de indice
//   i ocupa [tabela[i], tabela[i+1]), e o ultimo vai ate `fim dos dados`.
//
// ESTE CABECALHO JA FOI LIDO ERRADO UMA VEZ, com todos os campos deslocados em
// 4 bytes (0x0c como inicio da tabela, 0x10 como contagem, 0x18 como fim). O
// erro passou pelos testes porque o arquivo sintetico do teste foi montado com
// o mesmo erro -- o teste provava o parser contra a minha suposicao, nao contra
// o arquivo real. Quem pega isso e rodar o parser no arquivo real: com os
// campos certos, 0x190 (pt) e 0x130 (li) caem exatamente em 0x20 + nRegistros*8.
// Duas validacoes independentes do que foi inferido:
//   1. nRegistros*8 + 0x20 == inicio da tabela, nos dois arquivos;
//   2. o registro de id 1002 (0x3ea, tipo 1) aponta o indice 9, cujo dado e
//      UTF-16 com BOM 0xFFFE e le "Nao e possivel inicializar." em portugues --
//      exatamente a string que o guest pediu e nao recebeu (o guest pediu id
//      1178 e 1002 em tectoy.mod; 1002 esta no arquivo _pt, 1178 no `li`).
// Os tipos vistos: 1 = texto (UTF-16 com BOM), 6 = imagem (BM/... binario).

#include <cstdint>
#include <vector>

namespace zeebulator {

struct BrewResourceDirectory {
  uint16_t record_count = 0;
  uint32_t offset_table = 0;
  uint32_t offset_count = 0;
  uint32_t data_end = 0;
};

// Le e valida o cabecalho. Devolve false se o arquivo nao tem a forma minima de
// um `.brf` (inclusive se a relacao registros->tabela nao fecha).
bool ParseBrewResourceDirectory(const std::vector<uint8_t>& file, BrewResourceDirectory* out);

// Procura (tipo, id) e devolve o intervalo CRU do dado dentro do arquivo.
// `type_match_any` existe porque nem todo chamador sabe o tipo: um que peca so
// o id deve achar o recurso.
bool ReadBrewResourceRecord(const std::vector<uint8_t>& file, const BrewResourceDirectory& dir,
                            uint16_t type, uint16_t id, bool type_match_any, uint32_t* out_start,
                            uint32_t* out_size);

// Decodifica uma string de recurso. O BOM e tratado como parte da string:
// 0xFFFE = UTF-16LE, 0xFEFF = UTF-16BE, e a ordem vale para os code units
// seguintes.
bool ReadBrewResourceString(const std::vector<uint8_t>& file, const BrewResourceDirectory& dir,
                            uint16_t id, std::vector<uint16_t>* out);

}  // namespace zeebulator
