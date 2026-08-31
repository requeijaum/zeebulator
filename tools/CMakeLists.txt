add_executable(zeebulator_cpu_demo cpu_demo.cpp)
target_link_libraries(zeebulator_cpu_demo PRIVATE zeebulator_core)

add_executable(zeebulator_ggz_inspector ggz_inspector.cpp)
target_link_libraries(zeebulator_ggz_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_bar_inspector bar_inspector.cpp)
target_link_libraries(zeebulator_bar_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_mif_inspector mif_inspector.cpp)
target_link_libraries(zeebulator_mif_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_obm1_inspector obm1_inspector.cpp)
target_link_libraries(zeebulator_obm1_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_pkg_inspector pkg_inspector.cpp)
target_link_libraries(zeebulator_pkg_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_pakz_inspector pakz_inspector.cpp)
target_link_libraries(zeebulator_pakz_inspector PRIVATE zeebulator_core)

add_executable(zeebulator_mod_probe mod_probe.cpp)
target_link_libraries(zeebulator_mod_probe PRIVATE zeebulator_core)

if(ZEEBULATOR_BUILD_STANDALONE)
  add_executable(zeebulator_game_probe
    game_probe.cpp
    ${PROJECT_SOURCE_DIR}/frontends/standalone/sdl2_unified_backend.cpp
    ${PROJECT_SOURCE_DIR}/frontends/standalone/letterbox.cpp
    ${PROJECT_SOURCE_DIR}/frontends/standalone/zpad_edges.cpp
  )
  target_include_directories(zeebulator_game_probe PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(zeebulator_game_probe PRIVATE zeebulator_core SDL2::SDL2 SDL2::SDL2main
                                                       OpenGL::GL)
endif()
