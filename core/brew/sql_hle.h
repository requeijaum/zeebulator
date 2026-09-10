#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

struct sqlite3;

namespace zeebulator {

// ISQLMgr (AEECLSID_SQLMGR = 0x0102c4e8), o gerenciador de bancos do
// console, e a ISQLDatabase que ele devolve -- ambos sobre SQLite de
// verdade.
//
// A forma da interface aqui NAO foi adivinhada nem copiada de header
// (nao existe header publico para esta classe): foi MEDIDA. O scaffold
// generico que ja registrava a classe foi instrumentado para imprimir
// indice de slot, r0-r3, LR e o topo da pilha a cada chamada, e o
// tectoy.mod (a Z-Wheel, o menu do proprio Zeebo) produziu, nesta ordem:
//
//   [SQLTRACE] slot=3 r0=<this> r1=<"tt_prefs.db"> r2=<ponteiro de saida>
//              r3=00000000 lr=001436c4
//   [DBTRACE]  slot=3 r0=<db> r1=<"PRAGMA integrity_check">
//              r2=0x00184ce8 r3=0x80302f1c lr=0012fba4
//   [DBTRACE]  slot=3 r0=<db> r1=<"SELECT version, subversion FROM DBINFO">
//              r2=0x0012e214 r3=0x80302f1c lr=0012fba4
//   [DBTRACE]  slot=1 r0=<db> ... (depois da falha, um Release)
//
// Disso saem os unicos slots que este codigo afirma conhecer:
//   ISQLMgr slot 3      = OpenDatabase(this, const char* nome, ISQLDatabase** saida)
//   ISQLDatabase slot 3 = Exec(this, const char* sql, callback, void* ctx)
//   slot 0/1            = AddRef/Release (convencao IBase do BREW, e o
//                         slot 1 aparece na medicao logo depois do erro)
// O r2 do Exec e um ponteiro para codigo ARM do proprio modulo
// (0x00184ce8 e 0x0012e214 estao dentro da faixa carregada
// 0x00100000-0x001900e0) e o r3 se repete entre as duas chamadas: e a
// forma do sqlite3_exec (callback + contexto), que e coerente com o
// dialeto que o jogo manda. O callback e chamado com a assinatura do
// sqlite3_exec -- (ctx, ncols, char** valores, char** nomes) -- e esta e
// a unica parte AINDA NAO confirmada isoladamente: a confirmacao e
// indireta, pelo comportamento do jogo depois de receber as linhas.
//
// Os demais slots continuam sem implementacao de proposito: chamada em
// slot desconhecido imprime um aviso nomeando o indice, para aparecer no
// log em vez de passar por implementada.
class SqlHle {
 public:
  static constexpr uint32_t kClsidSqlMgr = 0x0102c4e8u;

  // Quantos slots cada vtable expoe. O trace so provou o 3 (e o 1 na
  // ISQLDatabase); o resto existe para que uma chamada inesperada caia
  // num aviso e nao em memoria nao mapeada.
  static constexpr size_t kSlotCount = 16;

  // Resolve o nome que o jogo pede ("tt_prefs.db") para um caminho de
  // arquivo real do host, ja gravavel. Devolver string vazia = recusar a
  // abertura. Quem instala isso e o harness (tools/game_probe.cpp): o
  // jogo nunca ve o sistema de arquivos do host por conta propria, do
  // mesmo jeito que o VirtualFilesystem faz para IFile/IFileMgr.
  using PathResolver = std::function<std::string(const std::string& name)>;

  SqlHle(Memory& memory, HleRuntime& hle, uint32_t db_object_region_start,
         uint32_t scratch_address, uint32_t scratch_size);
  ~SqlHle();

  SqlHle(const SqlHle&) = delete;
  SqlHle& operator=(const SqlHle&) = delete;

  // Constroi a vtable/objeto do ISQLMgr e a vtable compartilhada das
  // ISQLDatabase. Devolve o ponteiro de ISQLMgr que o app deve receber
  // do ISHELL_CreateInstance.
  uint32_t BuildManager(uint32_t mgr_vtable_address, uint32_t mgr_object_address,
                        uint32_t db_vtable_address);

  void SetPathResolver(PathResolver resolver) { resolver_ = std::move(resolver); }

  // Contadores para diagnostico e para os testes -- medida, nao
  // impressao de que funcionou.
  struct Stats {
    uint32_t opens = 0;
    uint32_t open_failures = 0;
    uint32_t execs = 0;
    uint32_t exec_failures = 0;
    uint32_t rows = 0;
  };
  const Stats& stats() const { return stats_; }

  // Todo SQL que o guest mandou, na ordem. E o que revela o que o jogo
  // realmente quer do banco.
  const std::vector<std::string>& executed_sql() const { return executed_sql_; }

  // Fecha todos os bancos abertos (o destrutor tambem faz isso).
  void CloseAll();

 private:
  struct Database {
    sqlite3* handle = nullptr;
    std::string name;
    std::string host_path;
    uint32_t ref_count = 1;
  };

  // ISQLMgr
  void MgrAddRef(IArmCore& core);
  void MgrRelease(IArmCore& core);
  void OpenDatabase(IArmCore& core);

  // ISQLDatabase
  void DbAddRef(IArmCore& core);
  void DbRelease(IArmCore& core);
  void Exec(IArmCore& core);

  // Le uma string C do espaco do guest (limite defensivo).
  std::string ReadGuestString(uint32_t address) const;
  // Copia `text` para a area de rascunho do guest e devolve o endereco.
  // Devolve 0 se nao couber -- o chamador trata como falha, sem escrever
  // fora da regiao.
  uint32_t PushScratchString(const std::string& text);
  uint32_t PushScratchWords(const std::vector<uint32_t>& words);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t next_db_object_;
  uint32_t scratch_address_;
  uint32_t scratch_size_;
  uint32_t scratch_used_ = 0;
  uint32_t mgr_object_ = 0;
  uint32_t mgr_ref_count_ = 1;
  uint32_t db_vtable_ = 0;
  // ZEEB_SQL_TRACE=1 imprime cada instrucao e cada linha entregue. E o
  // que mostra, sem adivinhacao, o que o jogo pede ao banco.
  bool trace_ = std::getenv("ZEEB_SQL_TRACE") != nullptr;
  PathResolver resolver_;
  std::unordered_map<uint32_t, Database> databases_;
  Stats stats_;
  std::vector<std::string> executed_sql_;
};

}  // namespace zeebulator
