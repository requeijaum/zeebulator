#include "core/brew/sql_hle.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "core/brew/interface_object.h"
#include "sqlite3.h"

namespace zeebulator {
namespace {

// Codigos de retorno. O BREW usa SUCCESS=0 e EFAILED=1 em toda a AEE, e
// o proprio jogo trata "!= 0" como falha (o trace mostrou ele seguindo
// adiante quando o stub devolvia 0). O valor exato de erro que o
// firmware real devolveria NAO foi medido -- so o zero de sucesso foi.
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kEFailed = 1;

constexpr size_t kMaxGuestStringLength = 4096;

}  // namespace

SqlHle::SqlHle(Memory& memory, HleRuntime& hle, uint32_t db_object_region_start,
               uint32_t scratch_address, uint32_t scratch_size)
    : memory_(memory),
      hle_(hle),
      next_db_object_(db_object_region_start),
      scratch_address_(scratch_address),
      scratch_size_(scratch_size) {}

SqlHle::~SqlHle() { CloseAll(); }

void SqlHle::CloseAll() {
  for (auto& [address, db] : databases_) {
    if (db.handle != nullptr) sqlite3_close(db.handle);
    db.handle = nullptr;
  }
  databases_.clear();
}

uint32_t SqlHle::BuildManager(uint32_t mgr_vtable_address, uint32_t mgr_object_address,
                              uint32_t db_vtable_address) {
  db_vtable_ = db_vtable_address;

  std::vector<HleRuntime::HleFunction> db_methods;
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    if (slot == 0) {
      db_methods.push_back([this](IArmCore& core) { DbAddRef(core); });
    } else if (slot == 1) {
      db_methods.push_back([this](IArmCore& core) { DbRelease(core); });
    } else if (slot == 2) {
      db_methods.push_back([](IArmCore& core) {
        const uint32_t out = core.GetRegister(kR2);
        if (out != 0) core.GetMemory().Write32(out, 0);
        core.SetRegister(kR0, 3);  // ECLASSNOTSUPPORT
      });
    } else if (slot == 3) {
      db_methods.push_back([this](IArmCore& core) { Exec(core); });
    } else {
      db_methods.push_back([slot](IArmCore& core) {
        std::printf("[sqlite] ISQLDatabase slot %zu chamado -- nao implementado\n", slot);
        core.SetRegister(kR0, 20);  // EUNSUPPORTED; nunca sucesso vazio
      });
    }
  }
  // A vtable das ISQLDatabase e compartilhada por todos os bancos; o
  // objeto (r0) e que identifica qual. Por isso a vtable e escrita
  // sozinha aqui, sem um objeto junto -- cada banco aberto ganha o seu
  // em OpenDatabase.
  for (size_t slot = 0; slot < db_methods.size(); ++slot) {
    // Nomes so nos slots que a medicao provou (0/1 pela convencao IBase do
    // BREW, 3 = Exec pelo trace); o resto fica com o indice cru de proposito.
    static const char* kDbSlotNames[] = {"AddRef", "Release", "", "Exec"};
    std::string label = "ISQLDatabase::slot" + std::to_string(slot);
    if (slot < 4 && kDbSlotNames[slot][0] != '\0') label += " " + std::string(kDbSlotNames[slot]);
    uint32_t sentinel = hle_.RegisterLabeled(db_methods[slot], label);
    memory_.Write32(db_vtable_address + static_cast<uint32_t>(slot) * 4u, sentinel);
  }

  std::vector<HleRuntime::HleFunction> mgr_methods;
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    if (slot == 0) {
      mgr_methods.push_back([this](IArmCore& core) { MgrAddRef(core); });
    } else if (slot == 1) {
      mgr_methods.push_back([this](IArmCore& core) { MgrRelease(core); });
    } else if (slot == 2) {
      mgr_methods.push_back([](IArmCore& core) {
        const uint32_t out = core.GetRegister(kR2);
        if (out != 0) core.GetMemory().Write32(out, 0);
        core.SetRegister(kR0, 3);
      });
    } else if (slot == 3) {
      mgr_methods.push_back([this](IArmCore& core) { OpenDatabase(core); });
    } else {
      mgr_methods.push_back([slot](IArmCore& core) {
        std::printf("[sqlite] ISQLMgr slot %zu chamado -- nao implementado\n", slot);
        core.SetRegister(kR0, 20);
      });
    }
  }
  mgr_object_ = BuildInterfaceObject(memory_, hle_, mgr_vtable_address, mgr_object_address,
                                     mgr_methods);
  return mgr_object_;
}

std::string SqlHle::ReadGuestString(uint32_t address) const {
  std::string out;
  if (address == 0) return out;
  for (size_t i = 0; i < kMaxGuestStringLength; ++i) {
    uint8_t byte = memory_.Read8(address + static_cast<uint32_t>(i));
    if (byte == 0) break;
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

uint32_t SqlHle::PushScratchString(const std::string& text) {
  uint32_t needed = static_cast<uint32_t>(text.size()) + 1;
  if (scratch_used_ + needed > scratch_size_) return 0;
  uint32_t address = scratch_address_ + scratch_used_;
  for (size_t i = 0; i < text.size(); ++i) {
    memory_.Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory_.Write8(address + static_cast<uint32_t>(text.size()), 0);
  scratch_used_ += needed;
  return address;
}

uint32_t SqlHle::PushScratchWords(const std::vector<uint32_t>& words) {
  scratch_used_ = (scratch_used_ + 3u) & ~3u;
  uint32_t needed = static_cast<uint32_t>(words.size()) * 4u;
  if (scratch_used_ + needed > scratch_size_) return 0;
  uint32_t address = scratch_address_ + scratch_used_;
  for (size_t i = 0; i < words.size(); ++i) {
    memory_.Write32(address + static_cast<uint32_t>(i) * 4u, words[i]);
  }
  scratch_used_ += needed;
  return address;
}

void SqlHle::MgrAddRef(IArmCore& core) {
  ++mgr_ref_count_;
  core.SetRegister(kR0, mgr_ref_count_);
}

void SqlHle::MgrRelease(IArmCore& core) {
  if (mgr_ref_count_ > 0) --mgr_ref_count_;
  core.SetRegister(kR0, mgr_ref_count_);
}

void SqlHle::OpenDatabase(IArmCore& core) {
  const uint32_t name_address = core.GetRegister(1);
  const uint32_t out_address = core.GetRegister(2);
  const std::string name = ReadGuestString(name_address);
  ++stats_.opens;
  if (out_address == 0) {
    ++stats_.open_failures;
    core.SetRegister(kR0, 14);  // EBADPARM
    return;
  }

  std::string host_path = resolver_ ? resolver_(name) : std::string();
  if (host_path.empty()) {
    ++stats_.open_failures;
    std::printf("[sqlite] OpenDatabase(\"%s\") recusado: sem caminho no host\n", name.c_str());
    if (out_address != 0) memory_.Write32(out_address, 0);
    core.SetRegister(kR0, kEFailed);
    return;
  }

  sqlite3* handle = nullptr;
  int rc = sqlite3_open(host_path.c_str(), &handle);
  if (rc != SQLITE_OK || handle == nullptr) {
    ++stats_.open_failures;
    std::printf("[sqlite] OpenDatabase(\"%s\") falhou em %s: %s\n", name.c_str(),
                host_path.c_str(), handle != nullptr ? sqlite3_errmsg(handle) : "sem handle");
    if (handle != nullptr) sqlite3_close(handle);
    if (out_address != 0) memory_.Write32(out_address, 0);
    core.SetRegister(kR0, kEFailed);
    return;
  }

  // Inicializacao minima apenas do banco que pode nascer vazio. Cada rc importa:
  // antes OpenDatabase publicava sucesso mesmo com schema parcialmente falho.
  if (name == "tt_dlqueue.db" || host_path.find("tt_dlqueue.db") != std::string::npos) {
    const char* setup[] = {
      "CREATE TABLE IF NOT EXISTS DBINFO(version INTEGER, subversion INTEGER);",
      "INSERT INTO DBINFO(version,subversion) SELECT 1,0 "
      "WHERE NOT EXISTS(SELECT 1 FROM DBINFO WHERE version=1 AND subversion=0);",
      "CREATE TABLE IF NOT EXISTS DLITEMINFO(item_id INTEGER PRIMARY KEY, price INTEGER, "
      "size INTEGER, titletext TEXT, boxart_path TEXT, flags INTEGER, upgrade_id INTEGER);"
    };
    for (const char* statement : setup) {
      if (sqlite3_exec(handle, statement, nullptr, nullptr, nullptr) != SQLITE_OK) {
        ++stats_.open_failures;
        sqlite3_close(handle);
        memory_.Write32(out_address, 0);
        core.SetRegister(kR0, kEFailed);
        return;
      }
    }
  }
  // Nunca reescreva idioma/preferencia do jogador durante um simples Open.

  if (next_db_object_ > scratch_address_ - 16u) {
    ++stats_.open_failures;
    sqlite3_close(handle);
    memory_.Write32(out_address, 0);
    core.SetRegister(kR0, 2);  // ENOMEMORY
    return;
  }
  uint32_t object = next_db_object_;
  next_db_object_ += 16;
  memory_.Write32(object, db_vtable_);
  Database db;
  db.handle = handle;
  db.name = name;
  db.host_path = host_path;
  databases_[object] = db;

  std::printf("[sqlite] OpenDatabase(\"%s\") -> %s (objeto 0x%08x)\n", name.c_str(),
              host_path.c_str(), object);
  if (out_address != 0) memory_.Write32(out_address, object);
  core.SetRegister(kR0, kSuccess);
}

void SqlHle::DbAddRef(IArmCore& core) {
  auto it = databases_.find(core.GetRegister(kR0));
  if (it == databases_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }
  ++it->second.ref_count;
  core.SetRegister(kR0, it->second.ref_count);
}

void SqlHle::DbRelease(IArmCore& core) {
  const uint32_t object = core.GetRegister(kR0);
  auto it = databases_.find(object);
  if (it == databases_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }
  if (it->second.ref_count > 0) --it->second.ref_count;
  uint32_t remaining = it->second.ref_count;
  if (remaining == 0) {
    if (it->second.handle != nullptr) sqlite3_close(it->second.handle);
    databases_.erase(it);
  }
  core.SetRegister(kR0, remaining);
}

void SqlHle::Exec(IArmCore& core) {
  const uint32_t object = core.GetRegister(kR0);
  const uint32_t sql_address = core.GetRegister(1);
  const uint32_t callback = core.GetRegister(2);
  const uint32_t context = core.GetRegister(3);
  // 5o argumento (na pilha, pela AAPCS): `char** ppErrMsg`. Vem da
  // documentacao real do SDK -- research/docs/sdk-extract/BrewMPSDK-7.12.5/
  // .../documentation/API Reference/Databases/Database Connect - SQL/
  // methods/ISQL_Exec.htm: "uint32 ISQL_Exec(ISQL* piSQL, const char* pSQL,
  // SQLExecCallBack pCallback, void* pUserData, char** ppErrMsg)". Bate com
  // a medicao: no trace, [sp] valia 0x00390094 com sp=0x00390088, ou seja um
  // endereco da propria pilha do chamador. Zerado sempre: o contrato diz que
  // a string, se existir, e alocada e liberada pelo chamador, e devolver um
  // ponteiro para memoria que o guest nao pode liberar seria pior que nao
  // devolver mensagem nenhuma -- o proprio jogo ja imprime "(error: %d, %s)"
  // com %s = NULL quando falha.
  const uint32_t err_msg_out = HleRuntime::ReadStackArg(core, 0);
  if (err_msg_out != 0 && (err_msg_out & 3u) == 0) memory_.Write32(err_msg_out, 0);

  auto it = databases_.find(object);
  if (it == databases_.end()) {
    std::printf("[sqlite] Exec num objeto desconhecido 0x%08x\n", object);
    core.SetRegister(kR0, kEFailed);
    return;
  }

  const std::string sql = ReadGuestString(sql_address);
  ++stats_.execs;
  executed_sql_.push_back(sql);
  if (trace_) {
    std::printf("[sqlite] Exec(\"%s\") callback=0x%08x ctx=0x%08x\n", sql.c_str(), callback,
                context);
  }

  sqlite3* handle = it->second.handle;
  const char* cursor = sql.c_str();
  bool aborted = false;
  while (cursor != nullptr && *cursor != '\0' && !aborted) {
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v2(handle, cursor, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
      ++stats_.exec_failures;
      std::printf("[sqlite] Exec falhou: %s -- SQL: %s\n", sqlite3_errmsg(handle), sql.c_str());
      if (stmt != nullptr) sqlite3_finalize(stmt);
      core.SetRegister(kR0, kEFailed);
      return;
    }
    if (stmt == nullptr) {  // so espaco/comentario ate o fim
      cursor = tail;
      continue;
    }
    while (!aborted) {
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) {
        ++stats_.exec_failures;
        std::printf("[sqlite] Exec parou em %s -- SQL: %s\n", sqlite3_errmsg(handle), sql.c_str());
        sqlite3_finalize(stmt);
        core.SetRegister(kR0, kEFailed);
        return;
      }
      ++stats_.rows;
      if (callback == 0) continue;

      // Uma linha por chamada de callback, com os valores ja em texto:
      // e o contrato do sqlite3_exec, cujo callback recebe `char**`.
      const int column_count = sqlite3_column_count(stmt);
      scratch_used_ = 0;
      if (trace_) std::printf("[sqlite]   linha\n");
      std::vector<uint32_t> values;
      std::vector<uint32_t> names;
      values.reserve(static_cast<size_t>(column_count));
      names.reserve(static_cast<size_t>(column_count));
      for (int column = 0; column < column_count; ++column) {
        if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
          values.push_back(0);
        } else {
          const unsigned char* text = sqlite3_column_text(stmt, column);
          values.push_back(
              PushScratchString(text != nullptr ? reinterpret_cast<const char*>(text) : ""));
        }
        const char* name = sqlite3_column_name(stmt, column);
        names.push_back(PushScratchString(name != nullptr ? name : ""));
      }
      const uint32_t values_array = PushScratchWords(values);
      const uint32_t names_array = PushScratchWords(names);
      if (values_array == 0 || names_array == 0) {
        ++stats_.exec_failures;
        std::printf("[sqlite] Exec sem espaco de rascunho para a linha -- SQL: %s\n", sql.c_str());
        sqlite3_finalize(stmt);
        core.SetRegister(kR0, kEFailed);
        return;
      }
      // ISQL::Exec callbacks are synchronous by API contract, but this HLE
      // handler itself was entered from guest code. The ordinary helper call
      // replaces PC/LR with its return sentinel and never restores the outer
      // SQL call, skipping the rest of the caller (Z-Wheel then never reached
      // tectoy.cfg/boot animation). Preserve CPU context, not callback memory
      // writes, which are the point of the callback.
      const uint32_t result = hle_.CallArmFunctionPreservingContext(
          callback, context, static_cast<uint32_t>(column_count), values_array, names_array);
      // Callback != 0 aborta a consulta, como no sqlite3_exec.
      if (result != 0) aborted = true;
    }
    sqlite3_finalize(stmt);
    cursor = tail;
  }

  core.SetRegister(kR0, aborted ? kEFailed : kSuccess);
}

}  // namespace zeebulator
