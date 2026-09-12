#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zeebulator::gui {

// Descoberta de jogos para a GUI. Esta camada NAO conhece SDL, OpenGL nem
// ImGui: recebe um diretorio e devolve uma lista. Existe separada por dois
// motivos concretos:
//
//  1. poder ser testada sem abrir janela (requisito RNF-4);
//  2. a identificacao de titulo parar de viver dentro de um if de renderizacao.
//
// O layout do NAND foi medido nesta sessao:
//   <raiz>/mod/<pasta>/<nome>.mod      binario do modulo
//   <raiz>/mod/<pasta>/data.ggz        opcional
//   <raiz>/mod/<pasta>/sound.ggz       opcional
//   <raiz>/mod/<pasta>/<nome>.bar      opcional
//   <raiz>/mif/<pasta>.mif             metadados, quando existem

// De onde veio o nome exibido. A UI mostra isso: um nome inventado e pior que
// um numero de pasta, porque parece confiavel e nao e.
enum class NameSource {
  kMif,     // string legivel extraida do .mif: a melhor fonte
  kModStem, // nome do proprio arquivo .mod (ex.: "chessbots.mod" -> "chessbots")
  kFolder,  // ultimo recurso: o numero da pasta, marcado como tal
};

// De onde veio o ClsId. Esta procedencia e o que separa "resolvi" de "chutei" --
// e nesta sessao 11 titulos foram dados como mortos por ClsId errado.
enum class ClsidSource {
  kManifest,   // games.json: valor explicito, mais forte
  kMif,        // tabela de ClassIDs do .mif
  kMod,        // literal lido no proprio despacho do CreateInstance
  kUnknown,    // nao resolvido: a UI nao deve tentar iniciar
};

struct GameEntry {
  std::string folder;           // "274214"
  std::string mod_path;         // caminho absoluto do .mod
  std::string name;             // nome exibido
  // Cadeia de fallback do nome, medida: .mif -> basename do .mod -> pasta. Das
  // 63 pastas deste NAND, 41 tem nome no .mif; das 22 restantes, TODAS tem um
  // basename legivel (chessbots.mod, Rolimaz.mod, alice.mod...), que e melhor
  // que "278738". O numero da pasta so entra quando nem isso existe.
  NameSource name_source = NameSource::kFolder;
  std::string data_ggz;         // "" quando nao ha
  std::string sound_ggz;
  std::string bar;
  // Candidatos em ordem de declaracao. A maioria dos titulos tem um so; o
  // tectoy declara varios e o applet e o SEGUNDO -- por isso e lista e nao
  // valor unico (ver o comentario de ExtractMifClassIds em core/loader/mif.h).
  std::vector<uint32_t> clsid_candidates;
  uint32_t clsid = 0;           // 0 = desconhecido
  ClsidSource clsid_source = ClsidSource::kUnknown;
  bool launchable = false;      // so quando clsid != 0
  std::string status_reason;    // texto para a UI; vazio quando launchable
};

struct ScanResult {
  std::vector<GameEntry> entries;
  std::string nand_root;      // o que foi varrido (o caminho efetivo)
  bool root_exists = false;
  std::string error;          // preenchido quando root nao existe/nao e dir
  size_t folders_seen = 0;    // pastas em mod/, com ou sem .mod
  size_t folders_with_mod = 0;
};

struct ScanOptions {
  std::string nand_root;
  // ClsIds ja resolvidos, por pasta. Vem do manifesto do usuario e tem
  // precedencia sobre tudo -- e a unica fonte que o usuario pode corrigir a mao.
  std::map<std::string, uint32_t> manifest_clsids;
};

// Varre <nand_root>/mod e devolve uma entrada por pasta que contenha .mod.
// Ordena por nome exibido (e por pasta como desempate) para a lista nao mudar
// de ordem entre execucoes -- ordem instavel em lista de jogos e defeito.
ScanResult ScanNand(const ScanOptions& options);

// Manifesto de ClsIds: <data_dir>/games.json, mapa pasta -> ClsId.
// Formato deliberadamente simples e legivel a mao:
//   { "games": { "274214": "0x1081984", "277455": 3207913505 } }
// Devolve mapa vazio quando o arquivo nao existe (nao e erro).
std::map<std::string, uint32_t> LoadManifest(const std::string& path);

// Manifesto efetivo: a camada BASE (default_games.json, embarcado) mais a do
// usuario, que sobrescreve. Existe porque ha ClsIds que nem o .mif nem o .mod
// resolvem direito: medido, o .mif do tectoy (274755) declara DUAS classes e a
// do applet e a SEGUNDA (0x01070798), com 0x01077CF4 antes dela -- sem esta
// camada a GUI tentaria a errada no titulo mais importante do corpus.
std::map<std::string, uint32_t> MergeManifests(const std::map<std::string, uint32_t>& base,
                                               const std::map<std::string, uint32_t>& user);

// Argumentos para iniciar um titulo no frontend de emulacao. Devolve vetor vazio
// quando a entrada nao e iniciavel -- melhor recusar do que montar uma linha que
// o emulador vai rejeitar por falta de ClsId.
//
// Fica aqui, e nao dentro do codigo da janela, por um motivo pratico: e a parte
// do lancamento que mais erra (ordem dos assets, quando passar --bar, titulo sem
// ggz) e assim ela tem teste.
std::vector<std::string> BuildLaunchArgs(const GameEntry& entry,
                                         const std::string& emulator_binary);

// Preferencias da interface. Persistidas em ~/.local/share/zeebulator/ui.json.
// NUNCA no diretorio NAND: aquele e midia do jogo (requisito RF-8).
struct UiConfig {
  std::string nand_root;
  int scale = 2;          // 1x..4x
  bool audio_enabled = true;
  int volume = 100;
};

// Caminho padrao das preferencias: $XDG_DATA_HOME/zeebulator/ui.json, caindo em
// ~/.local/share/zeebulator/ui.json. Separado em funcao propria para o teste
// poder verificar a regra sem escrever no diretorio do usuario.
std::string DefaultUiConfigPath(const char* xdg_data_home, const char* home);

UiConfig LoadUiConfig(const std::string& path);
bool SaveUiConfig(const std::string& path, const UiConfig& config);

// Filtro da busca da UI. Case-insensitive, casa em nome e em pasta, para o
// usuario poder digitar "274214" e achar o titulo sem saber o nome.
bool MatchesFilter(const GameEntry& entry, const std::string& filter);

// Texto de procedencia do ClsId, para a UI nao ser opaca.
const char* ClsidSourceName(ClsidSource source);
const char* NameSourceName(NameSource source);

// Candidatos de ClsId lidos no proprio binario do modulo: `ldr rX, [pc, #imm]`
// seguido de `cmp` com o mesmo registrador, ate 12 instrucoes depois.
//
// COBERTURA MEDIDA nos .mod reais, nao suposta:
//   a3d             0x01081970  achado (11 candidatos)
//   heavyweaponbrew 0x010978A2  achado ( 6 candidatos)
//   cnk2            0x01081984  nao achado -- resolvido pelo .mif
//   zenonia         0xBF2E2021  nao achado -- resolvido pelo manifesto
//
// O caso zenonia derrubou a primeira versao: o ClsId dele e RETORNADO por um
// getter (`ldr r0, [pc]` + `bx lr` em 0x1793B4) e comparado longe, em 0x10217C.
// Aceitar todos os literais da faixa o acharia, mas devolve 1466 valores nesse
// .mod -- lista inutil. A cobertura fica declarada, e o caminho para a zenonia e
// o manifesto, nao uma heuristica que finge cobrir tudo.
//
// Dois detalhes deliberados, porque a primeira versao errou nos dois:
//   - NAO exigir alinhamento de 4 bytes nos literais (os reais nao sao
//     alinhados, e o filtro os escondia);
//   - a mascara do `ldr` e 0xFFFF0000 e nao 0xFFFFF000, porque bits 12..15 sao
//     o proprio Rd: mascara-los junto so casaria `ldr r0`.
std::vector<uint32_t> FindClsidCandidatesInMod(const std::vector<uint8_t>& mod,
                                               uint32_t base = 0x00100000);

}  // namespace zeebulator::gui
