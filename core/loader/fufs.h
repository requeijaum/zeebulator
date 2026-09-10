#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zeebulator {

struct FufsEntry {
  std::string name;    // vazio quando o arquivo nao traz a tabela de nomes
  uint32_t name_hash;  // chave real da tabela (ver FufsArchive::HashName)
  uint32_t offset;     // offset absoluto do payload no arquivo
  uint32_t size;       // bytes do payload
};

// Le um container FUFS (".vfs") -- o empacotador de assets usado por, ao
// menos, Crash Nitro Kart 2 (mod/274214), Dirt Bike (277229) e a dupla
// 280394/280602 da NAND de referencia. Nao existe especificacao publica;
// tudo abaixo foi levantado por medicao direta sobre os 6 .vfs do corpus
// (7,9 MB a 22,9 MB; 67 a 448 entradas).
//
// Layout confirmado nos 6 arquivos:
//
//   offset 0:  magic "FUFS"
//   offset 4:  u32 LE -- bit 31 sempre setado nos 6 arquivos; os 31 bits
//              baixos sao o offset absoluto do fim da area de dados +
//              nomes, isto e, o inicio do rodape comprimido de nomes.
//              Quando o arquivo nao tem tabela de nomes esse valor e o
//              proprio tamanho do arquivo (medido: 274214 e 277229).
//   offset 8:  u32 LE -- numero de entradas
//   offset 12: `numero de entradas` registros de 12 bytes, ordenados por
//              hash CRESCENTE (estrito nos 6 arquivos -- e o que permite
//              busca binaria):
//                +0 u32 LE offset absoluto do payload
//                +4 u32 LE hash do nome
//                +8 u32 LE tamanho do payload
//   os payloads comecam exatamente no fim da tabela (12 + n*12) e sao
//   contiguos: offset[i+1] == offset[i] + size[i] nos 6 arquivos, sem
//   buraco e sem sobreposicao.
//   depois dos payloads (opcional): a lista de nomes em texto puro, um
//   por entrada, terminados em NUL, na MESMA ordem da tabela.
//   no offset do campo 4: u32 LE com o tamanho da lista de nomes seguido
//   de um stream zlib com exatamente o mesmo conteudo (redundante; os 4
//   arquivos que tem nomes trazem as duas copias, byte a byte iguais).
//
// A funcao de hash (FufsArchive::HashName) foi quebrada: multiplicativa
// com base 67, sem semente, insensivel a maiusculas/minusculas:
//
//   h = 0; para cada caractere c:  h = h * 67 + (toupper(c) - 113)   (mod 2^32)
//
// Como foi obtida (nao e chute): CRC32/djb2/sdbm/FNV-1/FNV-1a haviam sido
// descartados com 0 acertos. Os 4 arquivos que trazem a lista de nomes dao
// 782 pares (nome, hash) alinhados por indice. Pares de nomes de mesmo
// tamanho que diferem em UM caractere mostraram delta de hash exatamente
// igual a delta_do_caractere * 67^(caracteres_a_direita), o que fixa a base
// 67 e prova que a funcao e linear posicional. Resolvendo o sistema linear
// mod 2^32 com um valor incognito por caractere (782 equacoes, 39 colunas)
// saiu T[c] = toupper(c) - 113 para TODOS os caracteres do corpus, com
// semente 0. Resultado: 782/782 nomes reproduzem o hash da tabela.
// Confirmacao independente, fora da lista de nomes: extraindo as strings
// literais dos .mod, 56 caminhos de cnk2.mod e 10 de game.mod (277229)
// batem com entradas das tabelas dos .vfs correspondentes -- e esses dois
// arquivos NAO tem lista de nomes embutida.
// Ressalva honesta: o corpus so exercita os caracteres "-./0-9_a-z" (mais
// as versoes maiusculas). O valor de T[c] para qualquer outro caractere
// (espaco, acentos, etc.) nao foi medido.
//
// Conteudo das entradas: sem compressao no nivel do container. Medido nas
// 1642 entradas dos 6 arquivos, nos offsets que este parser calcula
// sozinho: 624 comecam com o magic PNG e TODAS as 624 terminam com o
// chunk IEND exatamente em offset+size-8 (0 falhas -- isto valida o campo
// de tamanho, nao so o de offset), 1 e RIFF, 893 comecam com o magic
// "PLZP" (formato proprio da engine: magic + u32 tamanho cru + u32
// tamanho comprimido + stream zlib), 64 sao streams zlib crus e 60 nao
// foram identificadas.
class FufsArchive {
 public:
  // Devolve std::nullopt para qualquer arquivo malformado, truncado ou que
  // nao seja um FUFS. NAO lanca excecao: um container desconhecido nao pode
  // derrubar quem chama (ver o bug ja corrigido em tools/game_probe.cpp, em
  // que uma excecao nao capturada abortava o processo inteiro).
  static std::optional<FufsArchive> Parse(std::vector<uint8_t> data);

  // Hash de nome do formato (ver o comentario da classe). Insensivel a
  // caixa; nao normaliza barras nem prefixos -- quem chama deve passar o
  // caminho como o jogo o pede (ex.: "data/carts/chars/crash.pof").
  static uint32_t HashName(std::string_view name);

  const std::vector<FufsEntry>& Entries() const { return entries_; }

  // true quando a lista de nomes existia E todos os nomes conferiram com o
  // hash da sua propria entrada. Se algum nome nao conferir, os nomes sao
  // descartados por inteiro (o parser nao inventa nome) e isto vira false.
  bool HasNames() const { return has_names_; }

  // Busca binaria pelo hash do nome. Devolve nullptr se nao existir.
  // Funciona mesmo sem lista de nomes -- e assim que da para servir os
  // arquivos de cnk2, que pedem os caminhos por string via IFILEMGR.
  const FufsEntry* Find(std::string_view name) const {
    return FindByHash(HashName(name));
  }
  const FufsEntry* FindByHash(uint32_t hash) const;

  // Bytes crus da entrada, verbatim. Os limites ja foram validados no
  // Parse, entao isto nao falha para uma entrada devolvida por Entries().
  std::vector<uint8_t> Extract(const FufsEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<FufsEntry> entries_;
  bool has_names_ = false;
};

}  // namespace zeebulator
