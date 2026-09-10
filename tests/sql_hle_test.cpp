#include "core/brew/sql_hle.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::kR0;
using zeebulator::SqlHle;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kMgrVtable = 0x80004000;
constexpr uint32_t kMgrObject = 0x80005000;
constexpr uint32_t kDbVtable = 0x80006000;
constexpr uint32_t kDbObjectRegion = 0x80007000;
constexpr uint32_t kScratch = 0x80008000;
constexpr uint32_t kScratchSize = 0x2000;
constexpr uint32_t kOutParam = 0x8000A000;

// Slots medidos no tectoy.mod (ver core/brew/sql_hle.h).
constexpr uint32_t kSlotAddRef = 0;
constexpr uint32_t kSlotRelease = 1;
constexpr uint32_t kSlotOpenOrExec = 3;

// Uma linha entregue ao callback, ja lida de volta do espaco do guest.
struct CapturedRow {
  std::vector<std::string> values;
  std::vector<std::string> names;
};

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  SqlHle sql{cpu.GetMemory(), hle, kDbObjectRegion, kScratch, kScratchSize};
  uint32_t mgr = 0;
  std::vector<CapturedRow> rows;
  uint32_t callback_result = 0;

  Fixture() { mgr = sql.BuildManager(kMgrVtable, kMgrObject, kDbVtable); }

  uint32_t MgrSlot(uint32_t slot) { return cpu.GetMemory().Read32(kMgrVtable + slot * 4); }
  uint32_t DbSlot(uint32_t slot) { return cpu.GetMemory().Read32(kDbVtable + slot * 4); }

  uint32_t PutString(uint32_t address, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
      cpu.GetMemory().Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
    }
    cpu.GetMemory().Write8(address + static_cast<uint32_t>(text.size()), 0);
    return address;
  }

  std::string GetString(uint32_t address) {
    std::string out;
    if (address == 0) return out;
    for (int i = 0; i < 512; ++i) {
      uint8_t byte = cpu.GetMemory().Read8(address + static_cast<uint32_t>(i));
      if (byte == 0) break;
      out.push_back(static_cast<char>(byte));
    }
    return out;
  }

  // Um "callback do jogo": um trap HLE no lugar do codigo ARM real, com
  // a mesma assinatura medida -- (ctx, ncols, char** valores, char** nomes).
  uint32_t RegisterCallback() {
    return hle.Register([this](zeebulator::IArmCore& core) {
      const uint32_t column_count = core.GetRegister(1);
      const uint32_t values = core.GetRegister(2);
      const uint32_t names = core.GetRegister(3);
      CapturedRow row;
      for (uint32_t i = 0; i < column_count; ++i) {
        row.values.push_back(GetString(cpu.GetMemory().Read32(values + i * 4)));
        row.names.push_back(GetString(cpu.GetMemory().Read32(names + i * 4)));
      }
      rows.push_back(row);
      core.SetRegister(kR0, callback_result);
    });
  }
};

std::filesystem::path MakeTempDir(const std::string& name) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("zeebulator-sql-" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// O mesmo esquema do tt_prefs.db que vem no pacote da Z-Wheel (lido do
// arquivo real com sqlite_master).
void SeedPrefsDatabase(Fixture& f, const std::filesystem::path& dir) {
  f.sql.SetPathResolver([dir](const std::string& name) { return (dir / name).string(); });
  uint32_t name_address = f.PutString(kScratch + 0x1000, "tt_prefs.db");
  uint32_t rc = f.hle.CallArmFunction(f.MgrSlot(kSlotOpenOrExec), f.mgr, name_address, kOutParam);
  ASSERT_EQ(rc, 0u);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);
  ASSERT_NE(db, 0u);
  const char* schema[] = {
      "CREATE TABLE DBINFO(version INTEGER, subversion INTEGER)",
      "INSERT OR REPLACE INTO DBINFO values (1, 0)",
      "CREATE TABLE PREFSINFO(name TEXT PRIMARY KEY, strValue TEXT, dwValue INTEGER, flags INTEGER)",
      "INSERT OR REPLACE INTO PREFSINFO values ('Initialized', '', 1, 2)",
  };
  for (const char* sql : schema) {
    uint32_t sql_address = f.PutString(kScratch + 0x1100, sql);
    ASSERT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, 0, 0), 0u);
  }
}

}  // namespace

TEST(SqlHle, OpenDatabaseDevolveObjetoNoPonteiroDeSaida) {
  Fixture f;
  auto dir = MakeTempDir("open");
  f.sql.SetPathResolver([dir](const std::string& name) { return (dir / name).string(); });

  uint32_t name_address = f.PutString(kScratch + 0x1000, "tt_prefs.db");
  uint32_t rc = f.hle.CallArmFunction(f.MgrSlot(kSlotOpenOrExec), f.mgr, name_address, kOutParam);

  EXPECT_EQ(rc, 0u);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);
  EXPECT_NE(db, 0u);
  // O objeto devolvido precisa apontar para a vtable de ISQLDatabase.
  EXPECT_EQ(f.cpu.GetMemory().Read32(db), kDbVtable);
  EXPECT_EQ(f.sql.stats().opens, 1u);
  EXPECT_EQ(f.sql.stats().open_failures, 0u);
  EXPECT_TRUE(std::filesystem::exists(dir / "tt_prefs.db"));
}

// Controle negativo: sem caminho no host o Open precisa FALHAR e zerar o
// ponteiro de saida, e nao devolver um objeto qualquer.
TEST(SqlHle, OpenDatabaseSemCaminhoFalhaEZeraSaida) {
  Fixture f;
  f.cpu.GetMemory().Write32(kOutParam, 0xDEADBEEF);
  f.sql.SetPathResolver([](const std::string&) { return std::string(); });

  uint32_t name_address = f.PutString(kScratch + 0x1000, "tt_prefs.db");
  uint32_t rc = f.hle.CallArmFunction(f.MgrSlot(kSlotOpenOrExec), f.mgr, name_address, kOutParam);

  EXPECT_NE(rc, 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(kOutParam), 0u);
  EXPECT_EQ(f.sql.stats().open_failures, 1u);
}

// A primeira instrucao que o tectoy.mod manda depois de abrir o banco.
TEST(SqlHle, PragmaIntegrityCheckEntregaOkAoCallback) {
  Fixture f;
  auto dir = MakeTempDir("integrity");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t callback = f.RegisterCallback();
  uint32_t sql_address = f.PutString(kScratch + 0x1100, "PRAGMA integrity_check");
  uint32_t rc = f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, callback,
                                       0x12345678);

  EXPECT_EQ(rc, 0u);
  ASSERT_EQ(f.rows.size(), 1u);
  EXPECT_EQ(f.rows[0].values[0], "ok");
}

// A segunda instrucao medida: o jogo recusa o banco se a versao nao vier.
TEST(SqlHle, SelectDaVersaoEntregaValoresENomesDeColuna) {
  Fixture f;
  auto dir = MakeTempDir("versao");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t callback = f.RegisterCallback();
  uint32_t sql_address =
      f.PutString(kScratch + 0x1100, "SELECT version, subversion FROM DBINFO");
  uint32_t rc =
      f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, callback, 0);

  EXPECT_EQ(rc, 0u);
  ASSERT_EQ(f.rows.size(), 1u);
  EXPECT_EQ(f.rows[0].values, (std::vector<std::string>{"1", "0"}));
  EXPECT_EQ(f.rows[0].names, (std::vector<std::string>{"version", "subversion"}));
}

// O contexto (r3 na medicao) precisa chegar inteiro no callback: e por ele
// que o jogo sabe onde guardar o que leu.
TEST(SqlHle, ContextoChegaNoCallback) {
  Fixture f;
  auto dir = MakeTempDir("contexto");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t seen_context = 0;
  uint32_t callback = f.hle.Register([&](zeebulator::IArmCore& core) {
    seen_context = core.GetRegister(kR0);
    core.SetRegister(kR0, 0);
  });
  uint32_t sql_address = f.PutString(kScratch + 0x1100, "SELECT version FROM DBINFO");
  f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, callback, 0xCAFEF00D);

  EXPECT_EQ(seen_context, 0xCAFEF00Du);
}

TEST(SqlHle, EscritaSobreviveAoProximoSelect) {
  Fixture f;
  auto dir = MakeTempDir("escrita");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t callback = f.RegisterCallback();
  uint32_t insert = f.PutString(kScratch + 0x1100,
                                "INSERT OR REPLACE INTO PREFSINFO values ('TermsAccepted', '', 1, 2)");
  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, insert, callback, 0), 0u);
  EXPECT_TRUE(f.rows.empty());

  uint32_t select = f.PutString(kScratch + 0x1100,
                                "SELECT dwValue FROM PREFSINFO WHERE name = 'TermsAccepted'");
  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, select, callback, 0), 0u);
  ASSERT_EQ(f.rows.size(), 1u);
  EXPECT_EQ(f.rows[0].values[0], "1");
}

// Controle negativo: SQL invalido tem de virar erro, sem linha nenhuma.
TEST(SqlHle, SqlInvalidoFalhaESemLinhas) {
  Fixture f;
  auto dir = MakeTempDir("invalido");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t callback = f.RegisterCallback();
  uint32_t sql_address = f.PutString(kScratch + 0x1100, "SELECT * FROM NAO_EXISTE");
  uint32_t rc = f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, callback, 0);

  EXPECT_NE(rc, 0u);
  EXPECT_TRUE(f.rows.empty());
  EXPECT_EQ(f.sql.stats().exec_failures, 1u);
}

// Controle negativo: Exec num objeto que nao veio do OpenDatabase falha.
TEST(SqlHle, ExecEmObjetoDesconhecidoFalha) {
  Fixture f;
  uint32_t sql_address = f.PutString(kScratch + 0x1100, "PRAGMA integrity_check");
  uint32_t rc = f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), 0x11112222, sql_address, 0, 0);
  EXPECT_NE(rc, 0u);
}

// Callback devolvendo != 0 aborta a consulta, como no sqlite3_exec.
TEST(SqlHle, CallbackQueDevolveNaoZeroAborta) {
  Fixture f;
  auto dir = MakeTempDir("aborta");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t insert = f.PutString(kScratch + 0x1100,
                                "INSERT OR REPLACE INTO PREFSINFO values ('B', '', 2, 2)");
  ASSERT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, insert, 0, 0), 0u);

  f.callback_result = 1;
  uint32_t callback = f.RegisterCallback();
  uint32_t select = f.PutString(kScratch + 0x1100, "SELECT name FROM PREFSINFO ORDER BY name");
  uint32_t rc = f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, select, callback, 0);

  EXPECT_NE(rc, 0u);
  EXPECT_EQ(f.rows.size(), 1u);
}

TEST(SqlHle, ReleaseFechaOBancoNaUltimaReferencia) {
  Fixture f;
  auto dir = MakeTempDir("release");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotAddRef), db), 2u);
  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotRelease), db), 1u);
  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotRelease), db), 0u);

  // Depois do ultimo Release o objeto nao existe mais: Exec nele falha.
  uint32_t sql_address = f.PutString(kScratch + 0x1100, "PRAGMA integrity_check");
  EXPECT_NE(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, 0, 0), 0u);
}

// O banco de verdade que vem no pacote da Z-Wheel, se estiver presente
// nesta maquina: DBINFO com version=1. Sem o arquivo, o teste se
// declara pulado em vez de passar de mentira.
TEST(SqlHle, BancoRealDaZWheelResponde) {
  const std::filesystem::path original =
      "/home/rafaelfrequiao/projects/zeebo-lab/games/brew/mod/274755/tt_prefs.db";
  if (!std::filesystem::exists(original)) GTEST_SKIP() << "tt_prefs.db real nao esta nesta maquina";

  Fixture f;
  auto dir = MakeTempDir("real");
  std::filesystem::copy_file(original, dir / "tt_prefs.db");
  f.sql.SetPathResolver([dir](const std::string& name) { return (dir / name).string(); });

  uint32_t name_address = f.PutString(kScratch + 0x1000, "tt_prefs.db");
  ASSERT_EQ(f.hle.CallArmFunction(f.MgrSlot(kSlotOpenOrExec), f.mgr, name_address, kOutParam), 0u);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  uint32_t callback = f.RegisterCallback();
  uint32_t integrity = f.PutString(kScratch + 0x1100, "PRAGMA integrity_check");
  ASSERT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, integrity, callback, 0), 0u);
  ASSERT_EQ(f.rows.size(), 1u);
  EXPECT_EQ(f.rows[0].values[0], "ok");

  f.rows.clear();
  uint32_t version = f.PutString(kScratch + 0x1100, "SELECT version, subversion FROM DBINFO");
  ASSERT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, version, callback, 0), 0u);
  ASSERT_EQ(f.rows.size(), 1u);
  EXPECT_EQ(f.rows[0].values, (std::vector<std::string>{"1", "0"}));
}


// O 5o argumento do ISQL_Exec e `char** ppErrMsg` (documentacao real do
// SDK, ver core/brew/sql_hle.cpp). Precisa sair zerado, nunca com lixo.
TEST(SqlHle, ExecZeraOPonteiroDeMensagemDeErro) {
  Fixture f;
  auto dir = MakeTempDir("errmsg");
  SeedPrefsDatabase(f, dir);
  uint32_t db = f.cpu.GetMemory().Read32(kOutParam);

  // Coloca o 5o argumento na pilha, como um chamador AAPCS faria.
  const uint32_t err_slot = kOutParam + 0x100;
  f.cpu.GetMemory().Write32(err_slot, 0xBADF00D0);
  const uint32_t stack = 0x00390000;
  f.cpu.SetRegister(13, stack);
  f.cpu.GetMemory().Write32(stack, err_slot);

  uint32_t sql_address = f.PutString(kScratch + 0x1100, "PRAGMA integrity_check");
  EXPECT_EQ(f.hle.CallArmFunction(f.DbSlot(kSlotOpenOrExec), db, sql_address, 0, 0), 0u);
  EXPECT_EQ(f.cpu.GetMemory().Read32(err_slot), 0u);
}
