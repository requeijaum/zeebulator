#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zeebulator {

// Extracts human-readable strings from a MIF (Module Information File).
//
// MIF's full binary structure (resource tables, class IDs, privilege
// bits) is NOT understood yet -- there's no public spec, and reverse
// engineering it fully is deferred (see TASKS.md Phase 2). What IS
// confirmed, cross-checked against real SDK samples and a real
// commercial game's MIF: human-readable metadata (app name, publisher,
// version, ...) is stored as UTF-16LE strings, each prefixed with an
// 0xFFFE BOM marker, ending at either a null code unit or the next BOM
// (strings can sit back-to-back with no separator). That's independently
// extractable without solving the rest of the format, which is all a
// game-library UI actually needs.
struct MifString {
  uint32_t offset;
  std::string text;
};

// Scans for BOM-prefixed UTF-16LE strings. Sequences containing any
// non-printable-ASCII code unit are dropped rather than returned with
// placeholder characters -- in practice these come from the BOM byte
// pattern (0xFF 0xFE) occurring by coincidence inside binary icon data
// elsewhere in the file, not from real text.
std::vector<MifString> ExtractMifStrings(const uint8_t* data, size_t size);

// Extrai os ClassIDs declarados por um MIF, em ordem de declaracao.
//
// A estrutura completa do MIF continua sem especificacao publica, mas a parte
// que importa para LANCAR um applet foi levantada por medicao sobre os 63 MIFs
// da NAND, com 60 pares (mif, ClassID) conhecidos como gabarito:
//
//   0x00  6 bytes fixos: 11 00 01 00 01 00
//   0x06  u16  numero de registros
//   0x10  u32  offset da tabela de offsets de secao
//   0x14  u32  quantidade de entradas dessa tabela (ha cnt+1 offsets)
//
// Cada offset da tabela aponta um registro de 8 bytes {ClassID u32, flags u32}.
// Resultado da validacao: em 60/60 MIFs o ClassID conhecido cai EXATAMENTE em
// uma dessas entradas -- nunca em outro lugar do arquivo.
//
// Por que devolver uma LISTA e nao um unico valor: a maioria dos titulos declara
// so uma classe e o primeiro candidato ja e o applet (59 de 60 casos). O tectoy
// (o menu do proprio console) declara varias, e a do applet e a SEGUNDA
// (0x01070798, com 0x01077cf4 antes dela). Escolher "a primeira" seria uma regra
// que falha exatamente no titulo mais importante do corpus, e nao ha, ate agora,
// campo medido que distinga o applet dos demais. Entregar a ordem de declaracao
// deixa quem lanca tentar cada uma ate obter um applet valido, que e uma decisao
// verificavel, em vez de um palpite embutido no parser.
std::vector<uint32_t> ExtractMifClassIds(const uint8_t* data, size_t size);

// Igual a ExtractMifStrings, mas TOLERANTE ao terminador.
//
// Diferenca medida, e motivo de existir: ha .mif em que a string do titulo NAO
// termina em nulo. No 277455.mif a string "zenonia" e seguida pelo codigo 0x1000
// e so depois por zeros; a versao estrita le esse 0x1000, marca a string inteira
// como suja e a DESCARTA -- ou seja, perde exatamente o nome do jogo. O mesmo
// acontece com "GOF" (277380), "3.0.0 B" (12875) e "VMGAME" (278200).
//
// Esta versao TERMINA a string no primeiro codigo nao imprimivel em vez de
// invalidar tudo, e devolve o prefixo legivel. O risco que a versao estrita
// evita (BOM que aparece por coincidencia dentro de dados binarios virar
// "string") continua controlado por `min_len`: sequencias curtas demais sao
// descartadas. A versao estrita NAO mudou: o contrato dela esta documentado e
// testado, e altera-lo mexeria em quem ja depende dele.
std::vector<MifString> ExtractMifStringPrefixes(const uint8_t* data, size_t size,
                                               size_t min_len = 3);

// Uma secao do MIF: o arquivo e um indice de registros + uma lista de secoes.
struct MifSection {
  uint32_t offset;
  uint32_t size;
};

// Recorta as secoes do MIF pela tabela de offsets do cabecalho.
//
// CABECALHO MEDIDO (62 dos 63 .mif do corpus debug_nand; o 63o, 11839.mif, nao
// tem o magico -- esta cifrado, e recusado aqui):
//
//   0x00  u16  magico 0x0011
//   0x02  u16  1
//   0x04  u16  1
//   0x06  u16  numero de entradas do indice
//   0x08  u32  offset do indice
//   0x0c  u32  tamanho do indice em bytes  (== entradas * 8)
//   0x10  u32  offset da tabela de secoes
//   0x14  u32  numero de secoes            (a tabela tem N+1 offsets)
//   0x18  u32  offset da primeira secao
//   0x1c  u32  tamanho total das secoes
//
// Como isto foi confirmado, e nao suposto: em todos os 62 arquivos com magico
// valido o campo 0x0c bate exatamente com `u16(0x06) * 8`, o campo 0x18 bate
// com o primeiro offset da tabela de secoes, e `u32(0x18) + u32(0x1c)` bate com
// o tamanho do arquivo. Quatro invariantes independentes, 62/62.
std::vector<MifSection> ExtractMifSections(const uint8_t* data, size_t size);

// ClassIDs dos APPLETS que o modulo expoe (secoes de 20 bytes cuja 2a e 4a
// palavra sao zero -- a mesma forma que o zeebx usa e que aqui foi
// reconfirmada: 62 dos 63 .mif do corpus produzem exatamente um applet, e o
// ClassID sai igual ao ja conhecido por outra fonte, p.ex. 274259 -> 0x01081970
// (Action Hero 3D) e 274755 -> 0x01070798 (a Z-Wheel/TECTOY)).
std::vector<uint32_t> ExtractMifAppletClassIds(const uint8_t* data, size_t size);

// ClassIDs que este MIF fornece como MODULO DE EXTENSAO -- vazio para um MIF de
// applet.
//
// MEDICAO (corpus debug_nand inteiro, 63 arquivos):
//
//  1. Ha um registro de 8 bytes {u32 ClassID, u32 flags=0} em varios .mif. Ele
//     e uma secao inteira, nunca apontada pelo indice.
//  2. Esse registro NAO distingue sozinho fornecedor de cliente: o a3d.mif
//     (274259, um jogo) tem {0x010292c3, 0} e o 12875.mif tem {0x010292c3, 0}
//     tambem. O primeiro PEDE a classe em execucao (medido: ISHELL_CreateInstance
//     (0x010292c3) e o dbgprintf "IMICRO3D failed creation" logo depois); o
//     segundo e a pasta que traz o imicro3d.mod que a implementa.
//  3. O que separa os dois casos e o registro de applet: 12875.mif e o UNICO
//     .mif do corpus SEM registro de applet (62 tem exatamente um; ele tem
//     zero). E exatamente o que o BREW espera de um MIF de extensao -- um modulo
//     que so exporta classes, sem nada para lancar no menu.
//
// Daí a regra implementada: sem applet => os registros de 8 bytes sao as classes
// FORNECIDAS; com applet => sao dependencias (classes pedidas), e esta funcao
// devolve vazio.
//
// LIMITE HONESTO: o corpus tem UMA extensao so (12875/imicro3d), entao a regra
// tem um positivo e 62 negativos. Ela nao foi validada contra um segundo
// fornecedor porque nao existe nenhum aqui -- as outras dependencias que
// aparecem no corpus (0x0103081d, 0x0101e4e1, 0x01077cf4) nao tem .mif
// correspondente na NAND, ou seja, sao classes da propria firmware. Por isso o
// carregador de extensao (core/brew/extension_module.h) nao confia so nesta
// regra: ele AINDA verifica, chamando o IModule::CreateInstance do .mod de
// verdade, se o modulo aceita a classe.
//
// Filtro dos falsos positivos: uma string UTF-16 de 8 bytes tambem forma uma
// secao de 8 bytes (p.ex. "1.0" grava ff fe 31 00 2e 00 30 00, que lido como
// registro daria ClassID 0x0031feff). Exigir flags==0 e ClassID em faixa
// plausivel descarta todas elas -- medido: 8 secoes assim no corpus, todas
// recusadas, nenhum ClassID real perdido.
std::vector<uint32_t> ExtractMifExtensionClassIds(const uint8_t* data, size_t size);

}  // namespace zeebulator
