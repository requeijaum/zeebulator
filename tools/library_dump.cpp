// Lista a biblioteca de jogos de um diretorio NAND. Existe para exercitar a
// mesma camada que a GUI usa, sem abrir janela -- a GUI nao pode ser o unico
// caminho para medir a descoberta (requisito RNF-4).
//
// uso: zeebulator_library <raiz_nand> [--manifest <games.json>] [--filtro texto]

#include <cstdio>
#include <map>
#include <cstring>
#include <string>

#include "frontends/gui/game_library.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "uso: %s <raiz_nand> [--manifest <games.json>] [--filtro texto]\n", argv[0]);
    return 2;
  }
  zeebulator::gui::ScanOptions options;
  options.nand_root = argv[1];
  std::string filter;
  std::map<std::string, uint32_t> user_manifest;
  bool use_default = true;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
      user_manifest = zeebulator::gui::LoadManifest(argv[++i]);
    } else if (std::strcmp(argv[i], "--sem-default") == 0) {
      use_default = false;
    } else if (std::strcmp(argv[i], "--filtro") == 0 && i + 1 < argc) {
      filter = argv[++i];
    }
  }

  // Camada base embarcada (default_games.json) + a do usuario por cima.
  std::map<std::string, uint32_t> base_manifest;
  if (use_default) {
    base_manifest = zeebulator::gui::LoadManifest(std::string(GUI_DEFAULT_MANIFEST));
  }
  options.manifest_clsids = zeebulator::gui::MergeManifests(base_manifest, user_manifest);

  const zeebulator::gui::ScanResult r = zeebulator::gui::ScanNand(options);
  if (!r.error.empty()) {
    std::fprintf(stderr, "erro: %s\n", r.error.c_str());
    return 1;
  }
  std::printf("NAND: %s\n", r.nand_root.c_str());
  std::printf("pastas em mod/: %zu | com .mod: %zu | no manifesto: %zu\n\n",
              r.folders_seen, r.folders_with_mod, options.manifest_clsids.size());
  std::printf("%-26s %-8s %-7s %-11s %-14s %s\n", "TITULO", "PASTA", "ASSETS",
              "CLSID", "ORIGEM", "ESTADO");
  size_t shown = 0, launchable = 0;
  for (const zeebulator::gui::GameEntry& e : r.entries) {
    if (!zeebulator::gui::MatchesFilter(e, filter)) continue;
    ++shown;
    if (e.launchable) ++launchable;
    std::string assets;
    if (!e.data_ggz.empty()) assets += "ggz";
    if (!e.sound_ggz.empty()) assets += assets.empty() ? "snd" : "+snd";
    if (!e.bar.empty()) assets += assets.empty() ? "bar" : "+bar";
    if (assets.empty()) assets = "-";
    char clsid[32];
    if (e.clsid != 0) std::snprintf(clsid, sizeof(clsid), "0x%08X", e.clsid);
    else std::snprintf(clsid, sizeof(clsid), "?");
    std::printf("%-26.26s %-8.8s %-7.7s %-11s %-14s %s\n", e.name.c_str(), e.folder.c_str(),
                assets.c_str(), clsid, zeebulator::gui::ClsidSourceName(e.clsid_source),
                e.launchable ? "iniciavel" : e.status_reason.c_str());
  }
  std::printf("\nmostrados: %zu | iniciaveis: %zu\n", shown, launchable);
  return 0;
}
