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

// Nome do titulo a partir do .mif. O .mif guarda strings UTF-16LE com BOM, e o
// extrator ja existente devolve todas em ordem de arquivo. Convencao medida nos
// MIFs desta NAND: a primeira string costuma ser a versao ("1.0.696") e a
// segunda o nome do publisher ("Zeebo"). Entao a escolha do nome NAO pode ser
// "a primeira": filtra-se por candidato que nao pareca versao e nao seja um
// nome de plataforma conhecido.
std::string NameFromMifStrings(const std::vector<MifString>& strings) {
  static const char* const kNeverTitle[] = {"zeebo", "qualcomm", "brew"};
  auto looks_like_version = [](const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
      if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') continue;
      return false;
    }
    return true;
  };
  for (const MifString& s : strings) {
    if (s.text.empty()) continue;
    if (looks_like_version(s.text)) continue;
    bool blocked = false;
    for (const char* bad : kNeverTitle) {
      if (ToLower(s.text) == bad) { blocked = true; break; }
    }
    if (blocked) continue;
    return s.text;
  }
  return {};
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

std::map<std::string, uint32_t> LoadManifest(const std::string& path) {
  std::map<std::string, uint32_t> out;
  const std::vector<uint8_t> bytes = ReadFile(path);
  if (bytes.empty()) return out;
  std::string text(bytes.begin(), bytes.end());
  // Parser minimo e deliberadamente tolerante: o arquivo e editado a mao pelo
  // usuario, e recusar por causa de espaco ou quebra de linha seria hostil.
  size_t pos = text.find("\"games\"");
  if (pos == std::string::npos) return out;
  pos = text.find('{', pos);
  if (pos == std::string::npos) return out;
  size_t end = text.find('}', pos);
  const std::string body = text.substr(pos + 1, (end == std::string::npos ? text.size() : end) - pos - 1);
  size_t p = 0;
  while ((p = body.find('"', p)) != std::string::npos) {
    const size_t key_end = body.find('"', p + 1);
    if (key_end == std::string::npos) break;
    const std::string key = body.substr(p + 1, key_end - p - 1);
    const size_t colon = body.find(':', key_end);
    if (colon == std::string::npos) break;
    size_t vstart = body.find_first_not_of(" \t\r\n", colon + 1);
    if (vstart == std::string::npos) break;
    size_t vend = body.find_first_of(",}\n", vstart);
    if (vend == std::string::npos) vend = body.size();
    std::string raw = body.substr(vstart, vend - vstart);
    raw.erase(std::remove_if(raw.begin(), raw.end(),
                             [](unsigned char c) { return std::isspace(c) != 0; }),
              raw.end());
    if (!raw.empty() && raw.front() == '"' && raw.back() == '"') raw = raw.substr(1, raw.size() - 2);
    if (!raw.empty()) {
      const uint32_t value = static_cast<uint32_t>(std::strtoul(raw.c_str(), nullptr, 0));
      if (value != 0) out[key] = value;
    }
    p = vend;
  }
  return out;
}

std::map<std::string, uint32_t> MergeManifests(const std::map<std::string, uint32_t>& base,
                                               const std::map<std::string, uint32_t>& user) {
  std::map<std::string, uint32_t> out = base;
  for (const auto& kv : user) out[kv.first] = kv.second;  // o usuario manda
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
        const std::vector<MifString> strings = ExtractMifStrings(mif.data(), mif.size());
        const std::string name = NameFromMifStrings(strings);
        if (!name.empty()) {
          e.name = name;
          e.name_source = NameSource::kMif;
        }
        e.clsid_candidates = ExtractMifClassIds(mif.data(), mif.size());
      }
    }
    if (e.name.empty()) {
      e.name = folder;
      e.name_source = NameSource::kFolder;
    }

    // ClsId: manifesto > .mif > .mod > desconhecido (ordem do requisito RF-3).
    const auto it = options.manifest_clsids.find(folder);
    if (it != options.manifest_clsids.end() && it->second != 0) {
      e.clsid = it->second;
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
  std::vector<std::string> args;
  if (!entry.launchable || entry.clsid == 0 || entry.mod_path.empty()) return args;
  if (emulator_binary.empty()) return args;
  args.push_back(emulator_binary);
  args.push_back(entry.mod_path);
  // Os dois slots de ggz sao posicionais no frontend: passar "-" quando nao ha.
  args.push_back(entry.data_ggz.empty() ? "-" : entry.data_ggz);
  args.push_back(entry.sound_ggz.empty() ? "-" : entry.sound_ggz);
  args.push_back(std::to_string(entry.clsid));
  // --bar e opcional e nomeado: so entra quando o titulo tem arquivo .bar.
  if (!entry.bar.empty()) {
    args.push_back("--bar");
    args.push_back(entry.bar);
  }
  return args;
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
    case NameSource::kFolder: return "pasta";
  }
  return "pasta";
}

}  // namespace zeebulator::gui
