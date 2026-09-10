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


}  // namespace zeebulator
