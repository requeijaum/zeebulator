#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zeebulator {

struct SarEntry {
  std::string name;         // nome da entrada, ex. "board5.m3g" (ASCII, sem diretorio)
  uint32_t timestamp;       // data/hora unix do asset original (medido: 2007..2009)
  uint32_t payload_offset;  // offset do payload dentro do arquivo
  uint32_t payload_size;    // bytes do payload (sempre armazenado CRU, sem compressao)
  uint32_t name_hash;       // 4 bytes gravados antes do nome; funcao nao identificada
  uint32_t content_hash;    // 4 bytes gravados depois do nome; varia com o payload
};

// Le um container SAR ("SWVARC"), usado pelo chessbots (mod/263019) -- titulo 3D
// da Superscape, dai o "SW" de Superscape Archive. Nao ha especificacao publica:
// o layout abaixo foi levantado por medicao direta sobre os 53 arquivos
// .sar/.sar.net da NAND de referencia (12 .sar + 41 .sar.net; os 9 pares
// <nome>.sar / <nome>.sar.net que existem sao byte a byte IDENTICOS -- medido
// por comparacao direta, o sufixo ".net" nao muda nada no formato).
//
// Cabecalho, tudo little-endian (os campos que pareciam big-endian na primeira
// olhada eram desalinhamento de leitura):
//
//   0x00  u8[12]  assinatura AB 'S' 'W' 'V' 'A' 'R' 'C' BB 0D 0A 1A 0A
//                 (mesmo truque do PNG: o 0D 0A 1A 0A denuncia transferencia
//                  em modo texto)
//   0x0C  u8      versao/flags -- 0x00 nos 53 arquivos medidos
//   0x0D  u32     offset do inicio da area de payloads (= tamanho do indice)
//   0x11  u32     tamanho TOTAL do arquivo (bate com o tamanho em disco nos 53)
//   0x15  u32     timestamp unix do container
//   0x19  u32     segundo timestamp unix (igual ao primeiro nos 53 medidos)
//   0x1D  u32     hash do container (funcao nao identificada)
//   0x21  u32     numero de entradas (1 a 146 no corpus)
//
// Indice a partir de 0x25, um registro por entrada, sem tabela de offsets:
//
//   u32    tamanho do registro, incluindo ele proprio == 21 + strlen(nome)
//   u32    timestamp unix do asset
//   u32    tamanho do payload
//   u32    hash do nome
//   char   nome[] terminado em NUL
//   u32    hash do conteudo
//
// Depois do ultimo registro vem UM byte 0x00 (terminador do indice) e so entao
// o offset declarado em 0x0D. Os payloads sao contiguos, na ordem do indice,
// crus (nenhum comprimido) e cobrem exatamente o resto do arquivo:
// 0x0D + soma(tamanhos) == tamanho total, sem sobra e sem sobreposicao nos 53.
//
// Evidencia de que a fronteira do campo "nome" esta certa: o nome "_0.lng"
// aparece em en_main.sar, es_main.sar e main.sar com payloads DIFERENTES e o
// mesmo hash de nome 0x7AA66448 nos tres -- o campo antes do nome depende so do
// nome, o de depois depende do conteudo. Nenhuma das funcoes testadas
// (CRC-32, Adler-32, FNV-1/1a, djb2, sdbm, nas duas ordens de byte) reproduz
// esses valores; por isso os dois hashes ficam expostos como bytes opacos e
// NAO sao usados para validar.
//
// Conteudo medido (assinaturas reais nos payloads extraidos): 85 PNG, 15 M3G
// (JSR184, "AB 'JSR184' BB 0D 0A 1A 0A"), 19 .pmd Qualcomm CMX (magic "cmid"),
// 9 MIDI ("MThd"), .asc/.lng binarios do proprio motor.
class SarArchive {
 public:
  // Devolve std::nullopt para qualquer arquivo malformado, truncado ou que nao
  // seja um SAR. NAO lanca excecao: um container desconhecido nao pode derrubar
  // quem chama (ver o caso do .vfs em tools/game_probe.cpp, onde uma excecao
  // nao capturada abortava o processo com core dump).
  static std::optional<SarArchive> Parse(std::vector<uint8_t> data);

  const std::vector<SarEntry>& Entries() const { return entries_; }

  // Timestamp e hash do cabecalho, expostos para diagnostico.
  uint32_t Timestamp() const { return timestamp_; }
  uint32_t HeaderHash() const { return header_hash_; }

  // Payload cru da entrada. Devolve nullopt se a entrada nao pertence a este
  // arquivo (geometria fora dos limites).
  std::optional<std::vector<uint8_t>> Extract(const SarEntry& entry) const;

  // Busca por nome exato. Devolve nullptr quando nao existe.
  const SarEntry* Find(std::string_view name) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<SarEntry> entries_;
  uint32_t timestamp_ = 0;
  uint32_t header_hash_ = 0;
};

}  // namespace zeebulator
