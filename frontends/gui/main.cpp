// GUI do Zeebulator. Fase 1 do docs/GUI-DESIGN.md.
//
// Escopo desta fase: a BIBLIOTECA. O usuario escolhe o diretorio NAND, ve os
// titulos com a procedencia de cada ClsId, busca, e inicia.
//
// Iniciar ainda DELEGA ao frontend de emulacao existente, por processo filho.
// Isso e deliberado e esta previsto no design (Fase 1 -> Fase 2): extrair o
// carregador de tools/game_probe.cpp e uma refatoracao propria, e a interface
// ja nasce com a costura pronta em BuildLaunchArgs. O que NAO se faz e uma
// segunda implementacao de carregamento so para a janela ter o quadro dentro.

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "frontends/gui/game_library.h"
#include "imgui.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_sdl2.h"

#ifdef GUI_DEFAULT_MANIFEST
static const char* kDefaultManifest = GUI_DEFAULT_MANIFEST;
#else
static const char* kDefaultManifest = "";
#endif
#ifdef GUI_DEFAULT_EMULATOR
static const char* kDefaultEmulator = GUI_DEFAULT_EMULATOR;
#else
static const char* kDefaultEmulator = "";
#endif

namespace {

struct AppState {
  zeebulator::gui::UiConfig config;
  std::string config_path;
  zeebulator::gui::ScanResult library;
  std::string filter;
  std::map<std::string, uint32_t> user_manifest;
  int selected = -1;
  std::string nand_input;      // campo editavel da aba Configuracoes
  std::string status_message;  // ultima acao; a UI nunca fica muda
  pid_t running_child = -1;
  bool rescan_requested = false;
};

void Rescan(AppState& st) {
  zeebulator::gui::ScanOptions options;
  options.nand_root = st.config.nand_root;
  options.manifest_clsids = zeebulator::gui::MergeManifests(
      zeebulator::gui::LoadManifest(kDefaultManifest), st.user_manifest);
  st.library = zeebulator::gui::ScanNand(options);
  st.selected = -1;
  if (!st.library.error.empty()) {
    st.status_message = "erro: " + st.library.error;
  } else {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%zu titulos (%zu pastas, %zu com .mod)",
                  st.library.entries.size(), st.library.folders_seen,
                  st.library.folders_with_mod);
    st.status_message = buf;
  }
}

// Inicia o titulo selecionado. Fase 1: processo filho. O fork+exec e a unica
// forma que nao duplica o carregador, e o filho e registrado para poder ser
// esperado -- processo orfao e bug visivel (requisito RF-6).
void LaunchSelected(AppState& st) {
  if (st.selected < 0 || static_cast<size_t>(st.selected) >= st.library.entries.size()) return;
  const zeebulator::gui::GameEntry& entry = st.library.entries[static_cast<size_t>(st.selected)];
  if (!entry.launchable) {
    st.status_message = "nao iniciavel: " + entry.status_reason;
    return;
  }
  const std::vector<std::string> args = zeebulator::gui::BuildLaunchArgs(entry, kDefaultEmulator);
  if (args.empty()) {
    st.status_message = "nao consegui montar a linha de comando deste titulo";
    return;
  }
  if (st.running_child > 0) {
    st.status_message = "ja existe uma sessao rodando; pare antes de iniciar outra";
    return;
  }
  const pid_t pid = fork();
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    std::fprintf(stderr, "falha ao iniciar '%s': %s\n", argv[0], std::strerror(errno));
    _exit(127);
  }
  if (pid < 0) {
    st.status_message = std::string("fork falhou: ") + std::strerror(errno);
    return;
  }
  st.running_child = pid;
  st.status_message = "iniciado: " + entry.name;
}

// Colhe o filho quando ele termina, para nao deixar zumbi nem orfao.
void ReapChild(AppState& st) {
  if (st.running_child <= 0) return;
  int status = 0;
  const pid_t done = waitpid(st.running_child, &status, WNOHANG);
  if (done == st.running_child) {
    st.running_child = -1;
    st.status_message = (status == 0) ? "sessao encerrada" : "sessao terminou com erro";
  }
}

void SaveConfig(AppState& st) {
  if (st.config_path.empty()) return;
  if (!zeebulator::gui::SaveUiConfig(st.config_path, st.config)) {
    st.status_message = "aviso: nao consegui gravar as preferencias em " + st.config_path;
  }
}

}  // namespace

int main(int argc, char** argv) {
  AppState st;
  st.config_path = zeebulator::gui::DefaultUiConfigPath(std::getenv("XDG_DATA_HOME"),
                                                        std::getenv("HOME"));
  st.config = zeebulator::gui::LoadUiConfig(st.config_path);
  if (st.config.nand_root.empty()) {
    // Primeira execucao: usa o default do sistema se existir, senao pede na UI.
    st.config.nand_root = "/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand";
  }
  st.nand_input = st.config.nand_root;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
      st.config.nand_root = argv[++i];
      st.nand_input = st.config.nand_root;
    }
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* window = SDL_CreateWindow(
      "Zeebulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 680,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow falhou: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  // Navegacao por teclado LIGADA: emulador de verdade se opera sem mouse
  // (Dolphin, mGBA, RetroArch). Sem esta flag as setas nao movem a selecao, e
  // foi o que fez o primeiro teste de lancamento por teclado nao iniciar nada.
  {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  }
  ImGui_ImplSDL2_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL2_Init();

  Rescan(st);

  bool quit = false;
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (ev.type == SDL_QUIT) quit = true;
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
          ev.window.windowID == SDL_GetWindowID(window)) {
        quit = true;
      }
      // Atalhos fora do ImGui, para funcionarem com a lista focada.
      if (ev.type == SDL_KEYDOWN && !ImGui::GetIO().WantTextInput) {
        if (ev.key.keysym.sym == SDLK_F5) st.rescan_requested = true;
        if (ev.key.keysym.sym == SDLK_RETURN) LaunchSelected(st);
      }
    }
    ReapChild(st);
    if (st.rescan_requested) {
      st.rescan_requested = false;
      Rescan(st);
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // ---- Janela unica com abas. O design fixa isto: nada de janelas
    // flutuantes soltas, que e como uma UI de modo imediato vira bagunca.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Zeebulator", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar);

    if (st.running_child > 0) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Sessao em execucao (pid %d)",
                         static_cast<int>(st.running_child));
    } else {
      ImGui::TextDisabled("Nenhuma sessao em execucao");
    }
    ImGui::SameLine();
    ImGui::Text("| emulador: %s", kDefaultEmulator);
    ImGui::Separator();

    if (ImGui::BeginTabBar("abas")) {
      if (ImGui::BeginTabItem("Biblioteca")) {
        ImGui::Text("NAND: %s", st.library.nand_root.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Re-varrer (F5)")) Rescan(st);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        char filter_buf[128];
        std::snprintf(filter_buf, sizeof(filter_buf), "%s", st.filter.c_str());
        // A busca filtra a cada tecla: sem botao de aplicar, que e o padrao dos
        // emuladores de referencia (Dolphin) e evita um estado intermediario.
        if (ImGui::InputTextWithHint("##busca", "buscar por titulo ou pasta...", filter_buf,
                                     sizeof(filter_buf))) {
          st.filter = filter_buf;
        }
        ImGui::Spacing();

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("jogos", 6, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
          ImGui::TableSetupColumn("TITULO", ImGuiTableColumnFlags_WidthStretch, 0.30f);
          ImGui::TableSetupColumn("PASTA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
          ImGui::TableSetupColumn("ASSETS", ImGuiTableColumnFlags_WidthFixed, 90.0f);
          ImGui::TableSetupColumn("CLSID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
          ImGui::TableSetupColumn("ORIGEM", ImGuiTableColumnFlags_WidthFixed, 110.0f);
          ImGui::TableSetupColumn("ESTADO", ImGuiTableColumnFlags_WidthStretch, 0.30f);
          ImGui::TableHeadersRow();

          for (size_t i = 0; i < st.library.entries.size(); ++i) {
            const zeebulator::gui::GameEntry& e = st.library.entries[i];
            if (!zeebulator::gui::MatchesFilter(e, st.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool is_selected = (st.selected == static_cast<int>(i));
            // Duplo clique inicia, como em Dolphin/PCSX2.
            if (ImGui::Selectable(e.name.c_str(), is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
              st.selected = static_cast<int>(i);
              if (ImGui::IsMouseDoubleClicked(0)) LaunchSelected(st);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.folder.c_str());
            ImGui::TableSetColumnIndex(2);
            {
              std::string assets;
              if (!e.data_ggz.empty()) assets += "ggz";
              if (!e.sound_ggz.empty()) assets += assets.empty() ? "snd" : "+snd";
              if (!e.bar.empty()) assets += assets.empty() ? "bar" : "+bar";
              ImGui::TextUnformatted(assets.empty() ? "-" : assets.c_str());
            }
            ImGui::TableSetColumnIndex(3);
            if (e.clsid != 0) {
              ImGui::Text("0x%08X", e.clsid);
            } else {
              ImGui::TextDisabled("?");
            }
            ImGui::TableSetColumnIndex(4);
            {
              // A ORIGEM do ClsId fica visivel de proposito: e o que separa
              // "resolvi" de "chutei". Um valor do manifesto o usuario pode
              // auditar; um do despejo do .mod e heuristica.
              const zeebulator::gui::ClsidSource src = e.clsid_source;
              const ImVec4 color = (src == zeebulator::gui::ClsidSource::kManifest)
                                       ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                       : (src == zeebulator::gui::ClsidSource::kUnknown
                                              ? ImVec4(0.9f, 0.5f, 0.5f, 1.0f)
                                              : ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
              ImGui::TextColored(color, "%s", zeebulator::gui::ClsidSourceName(src));
            }
            ImGui::TableSetColumnIndex(5);
            if (e.launchable) {
              ImGui::TextUnformatted("iniciavel");
            } else {
              ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "%s", e.status_reason.c_str());
            }
          }
          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Configuracoes")) {
        ImGui::Text("Diretorio NAND");
        ImGui::SetNextItemWidth(700);
        char nand_buf[512];
        std::snprintf(nand_buf, sizeof(nand_buf), "%s", st.nand_input.c_str());
        if (ImGui::InputText("##nand", nand_buf, sizeof(nand_buf))) st.nand_input = nand_buf;
        ImGui::SameLine();
        if (ImGui::Button("Aplicar e re-varrer")) {
          st.config.nand_root = st.nand_input;
          SaveConfig(st);
          Rescan(st);
        }
        ImGui::TextDisabled("As preferencias ficam em %s", st.config_path.c_str());
        ImGui::TextDisabled("Nada e gravado no diretorio NAND: ele e midia do jogo.");
        ImGui::Separator();

        if (ImGui::SliderInt("Escala", &st.config.scale, 1, 4)) SaveConfig(st);
        if (ImGui::Checkbox("Audio", &st.config.audio_enabled)) SaveConfig(st);
        if (ImGui::SliderInt("Volume", &st.config.volume, 0, 100)) SaveConfig(st);
        ImGui::Separator();

        ImGui::TextUnformatted("Avancado");
        ImGui::TextWrapped(
            "Orcamento de passos por chamada: 64 milhoes no frontend de emulacao. "
            "Isso e um limite do EMULADOR, nao do jogo: um titulo que copia ou "
            "descomprime um asset grande pode consumir mais que isso numa unica "
            "chamada e ser abortado no meio. Medido: o CreateInstance do cnk2 "
            "consome 124 milhoes. Quando um titulo nao inicia, este numero e a "
            "primeira coisa a conferir.");
        ImGui::Separator();
        ImGui::TextDisabled("Manifesto do usuario: %s/zeebulator/games.json",
                            std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME")
                                                         : "~/.local/share");
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    // Barra de estado: a UI nunca fica muda sobre o que acabou de acontecer.
    ImGui::Separator();
    ImGui::TextUnformatted(st.status_message.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::TextDisabled("Enter inicia | F5 re-varre");

    ImGui::End();

    ImGui::Render();
    glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x),
               static_cast<int>(ImGui::GetIO().DisplaySize.y));
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  ImGui_ImplOpenGL2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
