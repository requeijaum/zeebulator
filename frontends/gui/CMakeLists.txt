# GUI do Zeebulator (Fase 1 do docs/GUI-DESIGN.md).
#
# Alvo separado de proposito: o resto do build nao depende do ImGui, entao uma
# falha em busca-lo nao quebra o emulador nem os testes.
option(ZEEBULATOR_BUILD_GUI "Compila a GUI (requer rede para buscar o Dear ImGui)" ON)

if(ZEEBULATOR_BUILD_GUI)
  include(FetchContent)
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5   # versao fixada: dependencia de UI nao pode seguir master
  )
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()

  # O repositorio do ImGui nao traz CMakeLists na raiz, entao as fontes entram
  # explicitamente. Apenas os backends que usamos: SDL2 para janela/entrada e
  # OpenGL2 fixo, que e o que o contexto existente do emulador ja oferece.
  add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
  )
  target_include_directories(imgui_backends PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(imgui_backends PUBLIC SDL2::SDL2 OpenGL::GL)

  add_executable(zeebulator_gui
    main.cpp
    game_library.cpp
    game_library.h
  )
  target_include_directories(zeebulator_gui PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(zeebulator_gui PRIVATE zeebulator_core imgui_backends SDL2::SDL2
                                                 OpenGL::GL)
  # Os mesmos caminhos que tools/library_dump usa, para as duas nao divergirem.
  target_compile_definitions(zeebulator_gui PRIVATE
    GUI_DEFAULT_MANIFEST="${PROJECT_SOURCE_DIR}/frontends/gui/default_games.json"
    GUI_DEFAULT_EMULATOR="${CMAKE_BINARY_DIR}/tools/zeebulator_game_probe")
endif()
