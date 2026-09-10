#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zeebulator {

struct AezEntry {
  std::string name;             // caminho declarado, ex. "/data/meshes/alien weapon.aem"
  uint32_t decompressed_size;   // tamanho do conteudo apos descompactar
  uint32_t payload_offset;      // offset do payload no arquivo
  uint32_t payload_size;        // bytes ocupados no arquivo
  bool stored;                  // true = cru (nao comprimido)
};

// Le um arquivo AEZ (containers da Fishlabs: gof, pbc, e a serie ter*/cars/
// tracks). Formato levantado por medicao direta sobre os 12 .aez do corpus --
// nao ha especificacao publica. Registros sequenciais, sem tabela de indice:
//
//   u8   tamanho do nome
//   char nome[tamanho]                 (ASCII, com barra inicial)
//   u32  tamanho descomprimido         (little-endian)
//   u32  tamanho comprimido            (little-endian)
//   u8   payload[...]
//
// Regra que so aparece no meio dos arquivos: quando o tamanho comprimido e
// 0xFFFFFFFF a entrada esta ARMAZENADA CRUA, e o payload ocupa exatamente o
// tamanho descomprimido. Sem essa regra o parser lia 0xFFFFFFFF como tamanho,
// o offset seguinte estourava o u32 e a leitura parava na 19a entrada de 182
// (medido em res.aez do Galaxy on Fire). As demais entradas sao streams gzip
// RFC 1952 comuns (magic 1f 8b 08).
//
// Validacao: os 12 arquivos .aez da NAND sao consumidos 100%, sem sobra de
// bytes e sem estouro -- 1390 entradas no total.
class AezArchive {
 public:
  // Devolve std::nullopt para qualquer arquivo malformado, truncado ou que
  // nao seja um AEZ. NAO lanca excecao: um container desconhecido nao pode
  // derrubar quem chama (ver o caso do .vfs em tools/game_probe.cpp, onde uma
  // excecao nao capturada abortava o processo inteiro com core dump).
  static std::optional<AezArchive> Parse(std::vector<uint8_t> data);

  const std::vector<AezEntry>& Entries() const { return entries_; }

  // Descompacta uma entrada (ou devolve os bytes crus, se stored). Devolve
  // std::nullopt em erro de zlib ou se o tamanho nao bater com o declarado.
  std::optional<std::vector<uint8_t>> Extract(const AezEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<AezEntry> entries_;
};

}  // namespace zeebulator
