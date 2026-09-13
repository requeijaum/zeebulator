#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"

namespace zeebulator {

// Carregador de MODULO DE EXTENSAO BREW.
//
// O QUE E, MEDIDO: um .mif sem registro de applet declara as classes que o
// modulo dele FORNECE (ver ExtractMifExtensionClassIds em core/loader/mif.h).
// Quando o jogo chama ISHELL_CreateInstance com uma classe que o shell nao
// conhece, o BREW procura quem a fornece, carrega o .mod dessa extensao, chama
// o AEEMod_Load dele e depois o IModule::CreateInstance dele. O objeto que volta
// e codigo do proprio modulo rodando -- nao ha interface para a HLE implementar.
//
// EVIDENCIA QUE MOTIVOU ISTO (medida antes de escrever uma linha):
//   ZEEB_LOG_CREATEINSTANCE=1 zeebulator_game_probe .../mod/274259/a3d.mod - - 17308016
//   [createinstance] cls=UNKNOWN (0x010292c3) -> UNKNOWN (ECLASSNOTSUPPORT)
//   [guest dbgprintf] IMICRO3D failed creation
// e mif/12875.mif (unico .mif do corpus sem applet) fornece 0x010292c3, com o
// mod/12875/imicro3d.mod ao lado.
//
// ONDE O MODULO DE EXTENSAO E CARREGADO -- ESCOLHA MEDIDA, NAO ARBITRARIA.
// Este projeto ja teve tres bugs por reusar faixa ocupada, entao a base saiu de
// um levantamento do que cada faixa ja usa:
//
//   0x00000000-0x000000ff  landing pad de 'bx lr' (game_probe.cpp)
//   0x00090000-0x00098010  scratch do probe (ppMod, ppObj, AEEAppStart, rect)
//   0x00100000-...         modulo principal. O maior .mod do corpus e o
//                          quake2brew.mod, 8.705.936 bytes -> termina em
//                          ~0x0094d550 (medido com `ls -S` no corpus inteiro).
//   ...-0x00b4d550         pilha do convidado: game_probe faz
//                          SP = 0x00100000 + mod_size + 0x00200000, ou seja, no
//                          pior caso do corpus o topo da pilha fica em
//                          0x00b4d550 e ela desce dali.
//   0x80000000-0x800b1800  objetos/vtables de HLE (IDisplay, IFile, GL, ...)
//   0x80280000             tabela de helpers do ModRuntime (static base)
//   0x80300000-0x84300000  heap do modulo (64 MiB)
//   0x86000000, 0x87000000 widgets e bitmaps de compatibilidade
//   0xf0000000             traps da HLE
//
// A base fica em 0x02000000: acima de todo o bloco "modulo principal + pilha"
// (0x00b4d550 no pior caso, 20 MiB de folga) e muito abaixo de 0x80000000. Um
// `grep -rE '0x0[0-9a-fA-F]{7}'` em core/, tools/, frontends/ e tests/ nao acha
// NENHUMA constante na faixa 0x01000000-0x0fffffff usada como endereco (so
// codificacoes de instrucao ARM nos testes de CPU) -- a faixa esta livre.
// Escolhida abaixo de 0x10000000 de proposito: o CallStackTracer trata
// [0x00010000, 0x10000000) como "ponteiro de codigo plausivel", entao o PC de
// dentro de uma extensao continua aparecendo no stack trace.
//
// Modulos sucessivos ficam a 16 MiB um do outro (kExtensionModuleStride). O
// imicro3d.mod tem 90.068 bytes; 16 MiB da folga de duas ordens de grandeza e
// mantem a palavra de "static base" de cada modulo (base-4) fora do modulo
// anterior.
inline constexpr uint32_t kExtensionModuleBase = 0x02000000;
inline constexpr uint32_t kExtensionModuleStride = 0x01000000;
// Pagina de rascunho logo abaixo da primeira base: guarda o ppMod do
// AEEMod_Load e o ppObj do IModule::CreateInstance da extensao. Fica fora do
// scratch do probe (0x00090000) de proposito, para uma extensao carregada no
// meio de um CreateInstance do jogo nao pisar no ppObj do applet.
inline constexpr uint32_t kExtensionScratchBase = kExtensionModuleBase - 0x1000;

// Uma classe fornecida por uma extensao e o .mod que a implementa.
struct BrewExtensionEntry {
  uint32_t cls_id = 0;
  std::string mif_path;
  std::string mod_path;
};

// Varre <nand_root>/mif/*.mif e devolve, para cada .mif de extensao, as classes
// que ele fornece junto com o .mod correspondente em <nand_root>/mod/<nome>/.
//
// Silenciosa em erro de I/O: um .mif ilegivel (11839.mif do corpus esta cifrado)
// simplesmente nao contribui entrada nenhuma.
std::vector<BrewExtensionEntry> ScanBrewExtensionCatalog(const std::string& nand_root);

// Acha as extensoes disponiveis PARA UM MODULO especifico, a partir do caminho
// do .mod do jogo. Cobre os dois arranjos de disco ja medidos:
//
//   NAND do console:   <raiz>/mod/12875/imicro3d.mod   com <raiz>/mif/12875.mif
//   pacote baixado:    <jogo>/Kingdon Hearts_/swv21brew.mod
//                      com <jogo>/Kingdon Hearts_.mif ao lado da pasta
//
// A regra comum, medida nos dois: O .MIF TEM O NOME DA PASTA DO MODULO. Entao a
// busca e por nome de pasta, nunca por conteudo -- para cada pasta irma da pasta
// do jogo, procura-se "<pasta>.mif" ao lado dela e "../mif/<pasta>.mif".
std::vector<BrewExtensionEntry> ScanBrewExtensionsForModule(const std::string& game_mod_path);

class BrewExtensionLoader {
 public:
  BrewExtensionLoader(IArmCore& cpu, HleRuntime& hle);

  // Ponteiro do IShell da HLE: e o 1o argumento tanto do AEEMod_Load quanto do
  // IModule::CreateInstance da extensao.
  void SetShellPointer(uint32_t shell_ptr) { shell_ptr_ = shell_ptr; }

  // Endereco da tabela de helpers da stdlib BREW que o ModRuntime instalou. O
  // codigo ROPI da extensao le esse ponteiro em (base_do_modulo - 4), igual ao
  // modulo principal -- ver core/brew/mod_runtime.h.
  void SetStaticBaseTable(uint32_t table_address) { static_base_table_ = table_address; }

  void AddEntries(const std::vector<BrewExtensionEntry>& entries);
  size_t ScanNandRoot(const std::string& nand_root);
  // Ver ScanBrewExtensionsForModule: descobre pelo caminho do .mod do jogo.
  size_t ScanForModule(const std::string& game_mod_path);

  bool Provides(uint32_t cls_id) const { return providers_.count(cls_id) != 0; }
  size_t ClassCount() const { return providers_.size(); }

  // Faixas (base, tamanho) dos modulos ja carregados. Quem executa o convidado
  // precisa disto para nao confundir uma chamada a extensao com um PC perdido.
  std::vector<std::pair<uint32_t, uint32_t>> LoadedRanges() const;

  // Carrega (uma vez) o .mod que fornece `cls_id` e chama o
  // IModule::CreateInstance dele. Devolve o ponteiro do objeto, ou 0 se nao ha
  // fornecedor, se o modulo nao carrega, ou se o proprio modulo recusa a classe.
  //
  // Pode ser chamada de DENTRO de um handler de HLE (e o caso normal: o jogo
  // esta no meio do proprio CreateInstance quando pede a classe), por isso toda
  // entrada no convidado passa por HleRuntime::CallArmFunctionPreservingContext,
  // a primitiva de reentrancia deste projeto.
  uint32_t CreateInstance(uint32_t cls_id);

 private:
  struct LoadedModule {
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t module_ptr = 0;
  };

  // Carrega o .mod em memoria do convidado e chama o AEEMod_Load dele.
  // Devolve nullptr se qualquer etapa falhar.
  const LoadedModule* EnsureLoaded(const std::string& mod_path);

  IArmCore& cpu_;
  HleRuntime& hle_;
  uint32_t shell_ptr_ = 0;
  uint32_t static_base_table_ = 0;
  uint32_t next_base_ = kExtensionModuleBase;
  std::map<uint32_t, std::string> providers_;         // cls_id -> mod_path
  std::map<std::string, LoadedModule> loaded_;        // mod_path -> modulo
  std::map<std::string, bool> load_failed_;           // nao tentar de novo
};

}  // namespace zeebulator
