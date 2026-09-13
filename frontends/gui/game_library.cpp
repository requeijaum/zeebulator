#include "frontends/gui/game_library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <sstream>

#include "core/loader/mif.h"

namespace zeebulator::gui {

namespace fs = std::filesystem;

namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

// Nome do titulo a partir do .mif.
//
// Os .mif reais misturam, na mesma lista de strings UTF-16: fornecedor, aviso de
// copyright, versao, o nome do jogo e as vezes configuracao. Exemplos MEDIDOS
// neste NAND:
//
//   274214 -> ["Tectoy Digital", "(c) Polarbit 2008", "1.08",
//              "Crash Bandicoot Nitro Kart 3D", "display1=a%3A0"]
//   277455 -> ["(C)Gamevil", "(C)Copyright 2009", "zenonia"]
//   278962 -> ["PopCap Games", "1.0", "Peggle", "Bookworm"]
//   274755 -> ["1.0.696", "Zeebo", "display1=w%3A640%2Ch%3A480"]   (sem titulo)
//
// Regra, escolhida por medicao e nao por gosto: descarta versao, copyright,
// plataforma e configuracao; entre o que sobra, prefere o que casa com o nome do
// .mod (normalizado) e, na falta dele, o ULTIMO da lista. "Primeiro da lista"
// seria a regra errada: pegaria "Tectoy Digital" e "PopCap Games".
std::string NormalizeForCompare(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

std::string NameFromMifStrings(const std::vector<MifString>& strings, const std::string& mod_stem) {
  static const char* const kNeverTitle[] = {"zeebo", "qualcomm", "brew"};
  std::vector<std::string> candidates;
  for (const MifString& s : strings) {
    if (s.text.empty()) continue;
    // Configuracao do proprio MIF (ex.: "display1=w%3A640%2Ch%3A480").
    if (s.text.find('=') != std::string::npos) continue;
    const std::string lower = ToLower(s.text);
    // Aviso de copyright: "(c) Polarbit 2008", "(C)Copyright 2009".
    if (lower.rfind("(c)", 0) == 0) continue;
    bool blocked = false;
    for (const char* bad : kNeverTitle) {
      if (lower == bad) { blocked = true; break; }
    }
    if (blocked) continue;
    // Linha de copyright sem o "(c)": medido, aparece como "2009 Fishlabs" e
    // "2004, HI Corporation" -- comeca com ano de 4 digitos seguido de espaco ou
    // virgula. Um titulo real nao costuma comecar assim.
    if (lower.size() > 5 && std::isdigit(static_cast<unsigned char>(lower[0])) &&
        std::isdigit(static_cast<unsigned char>(lower[1])) &&
        std::isdigit(static_cast<unsigned char>(lower[2])) &&
        std::isdigit(static_cast<unsigned char>(lower[3])) &&
        (lower[4] == ' ' || lower[4] == ',')) {
      continue;
    }
    // Versao: so digitos e pontos, ou isso seguido de UMA letra solta. O caso da
    // letra e medido, nao suposto: o 12875.mif declara "3.0.0 B" como string, e
    // sem esta regra o titulo exibido era "3.0.0 B" em vez do nome do arquivo.
    bool version_like = true;
    size_t letters = 0;
    for (char c : s.text) {
      if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') continue;
      if (c == ' ' || std::isalpha(static_cast<unsigned char>(c))) {
        if (std::isalpha(static_cast<unsigned char>(c))) ++letters;
        continue;
      }
      version_like = false;
      break;
    }
    if (version_like && letters <= 1) continue;
    candidates.push_back(s.text);
  }
  if (candidates.empty()) return {};
  const std::string want = NormalizeForCompare(mod_stem);
  if (!want.empty()) {
    for (const std::string& c : candidates) {
      if (NormalizeForCompare(c) == want) return c;
    }
  }
  return candidates.back();
}

// Literal carregado por `ldr rX, [pc, #imm]` (ARM). Apartir da versao ARMv4 o
// endereco e (pc + 8), alinhado por palavra -- o alinhamento e o que faz a
// primeira implementacao (que exigia literais alinhados no arquivo) perder
// casos reais.
bool DecodeLdrPcLiteral(const uint8_t* mod, size_t size, size_t off, uint32_t addr,
                        uint32_t* out_value, int* out_reg) {
  if (off + 4 > size) return false;
  const uint32_t w = static_cast<uint32_t>(mod[off]) | (static_cast<uint32_t>(mod[off + 1]) << 8) |
                     (static_cast<uint32_t>(mod[off + 2]) << 16) |
                     (static_cast<uint32_t>(mod[off + 3]) << 24);
  // ldr rX, [pc, #imm12]: cond=AL, 01=LDR, P=1, U=1, B=0, W=0, L=1, Rn=pc,
  // Rd = rX, imm12 = deslocamento.
  //
  // A mascara e 0xFFFF0000, NAO 0xFFFFF000: bits 12..15 sao o proprio Rd, e
  // mascara-los junto com o Rn so casaria `ldr r0` -- todo `ldr r1` ou superior
  // passava batido. Foi esse o defeito que fez o teste sintetico falhar.
  if ((w & 0xFFFF0000u) != 0xE59F0000u) return false;
  const int rd = static_cast<int>((w >> 12) & 0xF);
  const uint32_t imm = w & 0xFFFu;
  const uint32_t literal_addr = (addr + 8u + imm) & ~3u;
  if (literal_addr < 0x00100000u) return false;
  const size_t literal_off = static_cast<size_t>(literal_addr - 0x00100000u);
  if (literal_off + 4 > size) return false;
  *out_value = static_cast<uint32_t>(mod[literal_off]) |
               (static_cast<uint32_t>(mod[literal_off + 1]) << 8) |
               (static_cast<uint32_t>(mod[literal_off + 2]) << 16) |
               (static_cast<uint32_t>(mod[literal_off + 3]) << 24);
  *out_reg = rd;
  return true;
}

// `cmp rY, rZ` (registrador com registrador): 0xE1500000 | Rn<<16 | Rm.
bool IsCmpReg(const uint8_t* mod, size_t size, size_t off, int* rn, int* rm) {
  if (off + 4 > size) return false;
  const uint32_t w = static_cast<uint32_t>(mod[off]) | (static_cast<uint32_t>(mod[off + 1]) << 8) |
                     (static_cast<uint32_t>(mod[off + 2]) << 16) |
                     (static_cast<uint32_t>(mod[off + 3]) << 24);
  if ((w & 0xFFF00FF0u) != 0xE1500000u) return false;
  *rn = static_cast<int>((w >> 16) & 0xF);
  *rm = static_cast<int>(w & 0xF);
  return true;
}

}  // namespace

namespace {

// A faixa em que os ClsId de applet realmente vivem no corpus medido.
constexpr uint32_t kAppletClsidLow = 0x01000000u;
constexpr uint32_t kAppletClsidHigh = 0x01FFFFFFu;

// Um valor que casa aqui e quase certamente uma constante de ponto flutuante
// carregada para comparacao, nao um ClsId.
//
// Motivo medido: no .mod do 11839 (kh.mod) a varredura estrita devolveu, alem do
// ClsId, os doubles 0x3FE921FB (pi/4), 0x3FD33333 (0.3), 0x3FF921FB (pi/2) e
// 0x413921FB -- e a versao sem guarda escolheu 0x3FE921FB como ClsId. A faixa
// 0x3F0..0x420 e a dos expoentes IEEE-754 para magnitudes de ~1e-4 a ~1e5, que
// e onde mora constante numerica de codigo.
bool LooksLikeFloatingPointConstant(uint32_t value) {
  const uint32_t exponent = (value >> 20) & 0x7FFu;
  return exponent >= 0x3F0u && exponent <= 0x420u;
}

}  // namespace

std::vector<uint32_t> FindClsidCandidatesInMod(const std::vector<uint8_t>& mod,
                                              uint32_t base) {
  (void)base;  // base fixa em 0x00100000: e onde todo .mod do Zeebo e carregado.
  std::vector<uint32_t> in_range;
  std::vector<uint32_t> out_of_range;
  const size_t size = mod.size();
  auto push_unique = [](std::vector<uint32_t>& v, uint32_t value) {
    if (std::find(v.begin(), v.end(), value) == v.end()) v.push_back(value);
  };
  for (size_t off = 0; off + 4 <= size; off += 4) {
    uint32_t value = 0;
    int reg = 0;
    const uint32_t addr = 0x00100000u + static_cast<uint32_t>(off);
    if (!DecodeLdrPcLiteral(mod.data(), size, off, addr, &value, &reg)) continue;
    // Piso em 0x01000000: abaixo disso nao ha ClsId, so constante.
    if (value < kAppletClsidLow) continue;
    // O `cmp` com esse registrador precisa vir logo depois -- e o que
    // distingue um ClsId comparado de um literal qualquer carregado.
    bool compared = false;
    for (size_t probe = off + 4; probe < off + 4 + 12 * 4 && probe + 4 <= size; probe += 4) {
      int rn = 0, rm = 0;
      if (IsCmpReg(mod.data(), size, probe, &rn, &rm) && rm == reg) {
        compared = true;
        break;
      }
    }
    if (!compared) continue;
    if (value >= kAppletClsidLow && value <= kAppletClsidHigh) {
      push_unique(in_range, value);
    } else if (!LooksLikeFloatingPointConstant(value)) {
      // Fora da faixa BREW so entra se nao parecer constante de ponto flutuante.
      // A zenonia (0xBF2E2021) e desse grupo, e e o motivo de a faixa ser
      // larga -- mas o grupo vem DEPOIS: um candidato na faixa e sempre mais
      // provavel que um fora dela.
      push_unique(out_of_range, value);
    }
  }
  // Ordem: faixa BREW primeiro. Medido no 11839: sem esta ordem o primeiro
  // candidato era 0x3FE921FB (pi/4); com ela e 0x1026191, um ClsId plausivel.
  std::vector<uint32_t> found = in_range;
  for (uint32_t v : out_of_range) found.push_back(v);
  return found;
}

namespace {

// Parser do manifesto, tolerante de proposito: o arquivo e editado a mao pelo
// dono do projeto, e recusar por causa de espaco ou quebra de linha seria
// hostil. Aceita DUAS formas por jogo, e a antiga continua valendo:
//
//   "274214": 17308036
//   "274259": { "clsid": 17308016, "max_steps": 186486543,
//               "env": { "ZEEB_GL_SOFT": "1" } }
//
// A forma longa existe para o que a GUI precisa passar por jogo: orcamento de
// passos (que sem isto faz o cnk2 ser dado como morto) e flags ZEEB_* em geral.
std::string Unquote(const std::string& s) {
  std::string r = s;
  r.erase(std::remove_if(r.begin(), r.end(),
                         [](unsigned char ch) { return std::isspace(ch) != 0; }),
          r.end());
  if (r.size() >= 2 && r.front() == '"' && r.back() == '"') r = r.substr(1, r.size() - 2);
  return r;
}

// Le um valor escalar a partir de `pos`, parando em virgula ou fecha-chaves.
std::string ScalarAt(const std::string& body, size_t pos, size_t* out_end) {
  const size_t vstart = body.find_first_not_of(" \t\r\n", pos);
  if (vstart == std::string::npos) { *out_end = pos; return ""; }
  size_t vend = body.find_first_of(",}\n", vstart);
  if (vend == std::string::npos) vend = body.size();
  *out_end = vend;
  return Unquote(body.substr(vstart, vend - vstart));
}

// Le um objeto { "chave": "valor", ... } a partir da chave que o antecede.
std::map<std::string, std::string> ObjectOfScalars(const std::string& text, size_t from) {
  std::map<std::string, std::string> out;
  const size_t open = text.find('{', from);
  if (open == std::string::npos) return out;
  int depth = 0;
  size_t close = open;
  for (; close < text.size(); ++close) {
    if (text[close] == '{') ++depth;
    else if (text[close] == '}' && --depth == 0) break;
  }
  const std::string inner = text.substr(open + 1, close - open - 1);
  size_t p = 0;
  while ((p = inner.find('"', p)) != std::string::npos) {
    const size_t ke = inner.find('"', p + 1);
    if (ke == std::string::npos) break;
    const std::string key = inner.substr(p + 1, ke - p - 1);
    const size_t colon = inner.find(':', ke);
    if (colon == std::string::npos) break;
    size_t next = colon;
    const std::string val = ScalarAt(inner, colon + 1, &next);
    if (!key.empty() && !val.empty()) out[key] = val;
    p = next;
  }
  return out;
}

}  // namespace

std::map<std::string, GameConfig> LoadManifest(const std::string& path) {
  std::map<std::string, GameConfig> out;
  const std::vector<uint8_t> bytes = ReadFile(path);
  if (bytes.empty()) return out;
  const std::string text(bytes.begin(), bytes.end());
  const size_t pos = text.find("\"games\"");
  if (pos == std::string::npos) return out;
  const size_t open = text.find('{', pos);
  if (open == std::string::npos) return out;
  int depth = 0;
  size_t close = open;
  for (; close < text.size(); ++close) {
    if (text[close] == '{') ++depth;
    else if (text[close] == '}' && --depth == 0) break;
  }
  const std::string body = text.substr(open + 1, close - open - 1);
  size_t p = 0;
  while ((p = body.find('"', p)) != std::string::npos) {
    const size_t key_end = body.find('"', p + 1);
    if (key_end == std::string::npos) break;
    const std::string key = body.substr(p + 1, key_end - p - 1);
    const size_t colon = body.find(':', key_end);
    if (colon == std::string::npos) break;
    size_t vstart = body.find_first_not_of(" \t\r\n", colon + 1);
    if (vstart == std::string::npos) break;
    GameConfig cfg;
    if (body[vstart] == '{') {
      // Forma longa. `clsid` e obrigatorio; o resto e opcional.
      const size_t clsid_key = body.find("\"clsid\"", vstart);
      const size_t obj_end = body.find('}', vstart);
      if (clsid_key != std::string::npos && (obj_end == std::string::npos || clsid_key < obj_end)) {
        const size_t c = body.find(':', clsid_key);
        size_t next = c;
        cfg.clsid = static_cast<uint32_t>(std::strtoull(ScalarAt(body, c + 1, &next).c_str(), nullptr, 0));
      }
      const size_t ms_key = body.find("\"max_steps\"", vstart);
      if (ms_key != std::string::npos && (obj_end == std::string::npos || ms_key < obj_end)) {
        const size_t c = body.find(':', ms_key);
        size_t next = c;
        cfg.max_steps = std::strtoull(ScalarAt(body, c + 1, &next).c_str(), nullptr, 0);
      }
      const size_t env_key = body.find("\"env\"", vstart);
      if (env_key != std::string::npos && (obj_end == std::string::npos || env_key < obj_end)) {
        cfg.env = ObjectOfScalars(body, env_key);
      }
      p = (obj_end == std::string::npos) ? body.size() : obj_end;
    } else {
      size_t next = colon;
      cfg.clsid = static_cast<uint32_t>(std::strtoul(ScalarAt(body, colon + 1, &next).c_str(), nullptr, 0));
      p = next;
    }
    if (cfg.clsid != 0 || cfg.max_steps != 0 || !cfg.env.empty()) out[key] = cfg;
  }
  return out;
}

std::map<std::string, GameConfig> MergeManifests(const std::map<std::string, GameConfig>& base,
                                                 const std::map<std::string, GameConfig>& user) {
  std::map<std::string, GameConfig> out = base;
  for (const auto& kv : user) {
    // O usuario manda, mas so nos campos que ele realmente escreveu: um
    // manifesto de usuario que so corrige o clsid nao pode apagar o max_steps
    // medido que veio da camada de base.
    GameConfig merged = out[kv.first];
    if (kv.second.clsid != 0) merged.clsid = kv.second.clsid;
    if (kv.second.max_steps != 0) merged.max_steps = kv.second.max_steps;
    for (const auto& e : kv.second.env) merged.env[e.first] = e.second;
    out[kv.first] = merged;
  }
  return out;
}

ScanResult ScanNand(const ScanOptions& options) {
  ScanResult result;
  result.nand_root = options.nand_root;
  if (options.nand_root.empty()) {
    result.error = "diretorio NAND nao configurado";
    return result;
  }
  const fs::path mod_dir = fs::path(options.nand_root) / "mod";
  const fs::path mif_dir = fs::path(options.nand_root) / "mif";
  std::error_code ec;
  if (!fs::is_directory(mod_dir, ec)) {
    result.error = "nao ha '" + mod_dir.string() + "' (diretorio NAND invalido?)";
    return result;
  }
  result.root_exists = true;

  for (const fs::directory_entry& de : fs::directory_iterator(mod_dir, ec)) {
    if (!de.is_directory()) continue;
    ++result.folders_seen;
    const std::string folder = de.path().filename().string();

    // Um .mod e o que define um titulo. Pasta sem .mod nao entra na lista: ela
    // pode ser cache de asset ou sobra, e listar isso poluiria a biblioteca.
    std::string mod_path;
    for (const fs::directory_entry& f : fs::directory_iterator(de.path(), ec)) {
      if (f.is_regular_file() && f.path().extension() == ".mod") {
        mod_path = f.path().string();
        break;
      }
    }
    if (mod_path.empty()) continue;
    ++result.folders_with_mod;

    GameEntry e;
    e.folder = folder;
    e.mod_path = mod_path;

    // Assets opcionais, por nome convencionado. A ordem importa: "data.ggz" e
    // "sound.ggz" sao os nomes reais do corpus; os demais entram so se existirem.
    for (const fs::directory_entry& f : fs::directory_iterator(de.path(), ec)) {
      if (!f.is_regular_file()) continue;
      const std::string fn = f.path().filename().string();
      const std::string ext = f.path().extension().string();
      if (ext == ".ggz") {
        if (fn == "sound.ggz") e.sound_ggz = f.path().string();
        else if (e.data_ggz.empty()) e.data_ggz = f.path().string();
      } else if (ext == ".bar") {
        if (e.bar.empty()) e.bar = f.path().string();
      }
    }

    // Nome: .mif primeiro, pasta como ultimo recurso MARCADO como tal.
    const fs::path mif_path = mif_dir / (folder + ".mif");
    if (fs::is_regular_file(mif_path, ec)) {
      const std::vector<uint8_t> mif = ReadFile(mif_path.string());
      if (!mif.empty()) {
        // TOLERANTE de proposito: a versao estrita descarta as strings de titulo
        // que nao terminam em nulo (medido: "zenonia", "GOF", "VMGAME"), que sao
        // exatamente as que interessam aqui. Ver ExtractMifStringPrefixes.
        const std::vector<MifString> strings =
            ExtractMifStringPrefixes(mif.data(), mif.size());
        std::string stem = fs::path(mod_path).stem().string();
        const std::string name = NameFromMifStrings(strings, stem);
        if (!name.empty()) {
          e.name = name;
          e.name_source = NameSource::kMif;
        }
        e.clsid_candidates = ExtractMifClassIds(mif.data(), mif.size());
      }
    }
    if (!e.name.empty()) {
      e.name_source = NameSource::kMif;
    } else {
      // Sem nome no .mif: o basename do .mod e a proxima melhor fonte. Medido:
      // as 22 pastas sem nome no .mif deste NAND tem todas um basename legivel,
      // entao este degrau resolve o caso inteiro e "pasta" fica para o resto.
      const std::string stem = fs::path(mod_path).stem().string();
      if (!stem.empty()) {
        e.name = stem;
        e.name_source = NameSource::kModStem;
      } else {
        e.name = folder;
        e.name_source = NameSource::kFolder;
      }
    }

    // ClsId: manifesto > .mif > .mod > desconhecido (ordem do requisito RF-3).
    const auto it = options.manifest_clsids.find(folder);
    // max_steps e env valem MESMO quando o clsid veio do .mif: o que a GUI
    // precisa passar por jogo nao depende de onde o clsid foi descoberto.
    if (it != options.manifest_clsids.end()) {
      e.max_steps = it->second.max_steps;
      e.env = it->second.env;
    }
    if (it != options.manifest_clsids.end() && it->second.clsid != 0) {
      e.clsid = it->second.clsid;
      e.clsid_source = ClsidSource::kManifest;
    } else if (!e.clsid_candidates.empty()) {
      e.clsid = e.clsid_candidates.front();
      e.clsid_source = ClsidSource::kMif;
    } else {
      const std::vector<uint8_t> mod = ReadFile(mod_path);
      if (!mod.empty()) {
        const std::vector<uint32_t> from_mod = FindClsidCandidatesInMod(mod);
        if (!from_mod.empty()) {
          e.clsid_candidates = from_mod;
          e.clsid = from_mod.front();
          e.clsid_source = ClsidSource::kMod;
        }
      }
    }

    e.launchable = e.clsid != 0;
    if (!e.launchable) {
      // A UI precisa dizer POR QUE, nao so que nao da. Um "nao iniciavel" mudo
      // e o mesmo erro que, nesta sessao, fez 11 titulos serem dados como
      // mortos por ClsId errado sem que ninguem soubesse o motivo.
      e.status_reason = "ClsId desconhecido: nem o manifesto, nem o .mif, nem o despejo do .mod resolveram";
    }
    result.entries.push_back(std::move(e));
  }

  std::sort(result.entries.begin(), result.entries.end(),
            [](const GameEntry& a, const GameEntry& b) {
              const std::string an = ToLower(a.name), bn = ToLower(b.name);
              if (an != bn) return an < bn;
              return a.folder < b.folder;
            });
  return result;
}

std::vector<std::string> BuildLaunchArgs(const GameEntry& entry,
                                         const std::string& emulator_binary) {
  return BuildLaunchSpec(entry, emulator_binary).argv;
}

LaunchSpec BuildLaunchSpec(const GameEntry& entry, const std::string& emulator_binary) {
  LaunchSpec spec;
  if (!entry.launchable || entry.clsid == 0 || entry.mod_path.empty()) return spec;
  if (emulator_binary.empty()) return spec;
  spec.argv.push_back(emulator_binary);
  spec.argv.push_back(entry.mod_path);
  // Os dois slots de ggz sao posicionais no frontend: passar "-" quando nao ha.
  spec.argv.push_back(entry.data_ggz.empty() ? "-" : entry.data_ggz);
  spec.argv.push_back(entry.sound_ggz.empty() ? "-" : entry.sound_ggz);
  spec.argv.push_back(std::to_string(entry.clsid));
  // --bar e opcional e nomeado: so entra quando o titulo tem arquivo .bar.
  if (!entry.bar.empty()) {
    spec.argv.push_back("--bar");
    spec.argv.push_back(entry.bar);
  }
  // Orcamento de passos por jogo. MEDIDO: sem isto o cnk2 aborta com
  // "exceeded 64000000 steps without returning" e e dado como morto.
  if (entry.max_steps != 0) {
    spec.env["ZEEB_MAX_STEPS"] = std::to_string(entry.max_steps);
  }
  // Flags livres do manifesto. O que vier escrito aqui vence, para o dono do
  // projeto poder ajustar sem recompilar.
  for (const auto& kv : entry.env) spec.env[kv.first] = kv.second;
  return spec;
}

std::string DefaultUiConfigPath(const char* xdg_data_home, const char* home) {
  // Regra XDG: $XDG_DATA_HOME vence; ausente ou vazio, cai em ~/.local/share.
  if (xdg_data_home != nullptr && xdg_data_home[0] != '\0') {
    return std::string(xdg_data_home) + "/zeebulator/ui.json";
  }
  if (home != nullptr && home[0] != '\0') {
    return std::string(home) + "/.local/share/zeebulator/ui.json";
  }
  return {};
}

UiConfig LoadUiConfig(const std::string& path) {
  UiConfig cfg;
  const std::vector<uint8_t> bytes = ReadFile(path);
  if (bytes.empty()) return cfg;
  const std::string text(bytes.begin(), bytes.end());
  auto read_string = [&](const char* key) -> std::string {
    const size_t k = text.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return {};
    const size_t colon = text.find(':', k);
    if (colon == std::string::npos) return {};
    const size_t first = text.find('"', colon);
    if (first == std::string::npos) return {};
    const size_t last = text.find('"', first + 1);
    if (last == std::string::npos) return {};
    return text.substr(first + 1, last - first - 1);
  };
  auto read_int = [&](const char* key, int fallback) -> int {
    const size_t k = text.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return fallback;
    const size_t colon = text.find(':', k);
    if (colon == std::string::npos) return fallback;
    return static_cast<int>(std::strtol(text.c_str() + colon + 1, nullptr, 10));
  };
  cfg.nand_root = read_string("nand_root");
  cfg.scale = read_int("scale", cfg.scale);
  cfg.volume = read_int("volume", cfg.volume);
  cfg.audio_enabled = read_int("audio_enabled", cfg.audio_enabled ? 1 : 0) != 0;
  if (cfg.scale < 1) cfg.scale = 1;
  if (cfg.scale > 4) cfg.scale = 4;
  if (cfg.volume < 0) cfg.volume = 0;
  if (cfg.volume > 100) cfg.volume = 100;
  return cfg;
}

bool SaveUiConfig(const std::string& path, const UiConfig& config) {
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(path);
  if (!out) return false;
  out << "{\n"
      << "  \"nand_root\": \"" << config.nand_root << "\",\n"
      << "  \"scale\": " << config.scale << ",\n"
      << "  \"audio_enabled\": " << (config.audio_enabled ? 1 : 0) << ",\n"
      << "  \"volume\": " << config.volume << "\n"
      << "}\n";
  return out.good();
}

bool MatchesFilter(const GameEntry& entry, const std::string& filter) {
  if (filter.empty()) return true;
  const std::string needle = ToLower(filter);
  return ToLower(entry.name).find(needle) != std::string::npos ||
         ToLower(entry.folder).find(needle) != std::string::npos;
}

const char* ClsidSourceName(ClsidSource source) {
  switch (source) {
    case ClsidSource::kManifest: return "manifesto";
    case ClsidSource::kMif: return "mif";
    case ClsidSource::kMod: return "despejo do .mod";
    case ClsidSource::kUnknown: return "desconhecido";
  }
  return "desconhecido";
}

const char* NameSourceName(NameSource source) {
  switch (source) {
    case NameSource::kMif: return "mif";
    case NameSource::kModStem: return "nome do .mod";
    case NameSource::kFolder: return "pasta";
  }
  return "pasta";
}

}  // namespace zeebulator::gui
