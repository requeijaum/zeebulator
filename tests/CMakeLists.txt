include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# Keep gtest's runtime library selection consistent with the rest of the
# project on Windows instead of forcing its own default.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(zeebulator_tests
  core_test.cpp
  memory_test.cpp
  cpu_test.cpp
  thumb_test.cpp
  bar_test.cpp
  ggz_test.cpp
  mif_test.cpp
  mod_test.cpp
  obm1_test.cpp
  pkg_test.cpp
  pakz_test.cpp
  hle_runtime_test.cpp
  brew_test.cpp
  brew_lifecycle_test.cpp
  virtual_filesystem_test.cpp
  file_hle_test.cpp
  gl_hle_test.cpp
  hid_hle_test.cpp
  gl_lifecycle_test.cpp
  wav_test.cpp
  mixer_test.cpp
  media_hle_test.cpp
  midi_test.cpp
  soundfont_synth_test.cpp
  mod_runtime_test.cpp
  font5x7_test.cpp
  scaffold_object_test.cpp
  atitc_test.cpp
  png_test.cpp
  letterbox_test.cpp
  ${PROJECT_SOURCE_DIR}/frontends/standalone/letterbox.cpp
  save_state_test.cpp
  gl_texture_log_test.cpp
  zpad_edges_test.cpp
  ${PROJECT_SOURCE_DIR}/frontends/standalone/zpad_edges.cpp
)
target_include_directories(zeebulator_tests PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(zeebulator_tests PRIVATE zeebulator_core GTest::gtest_main zlibstatic
                                                liblzma)
target_include_directories(zeebulator_tests PRIVATE ${liblzma_SOURCE_DIR}/src/liblzma/api)
target_compile_definitions(zeebulator_tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(GoogleTest)
gtest_discover_tests(zeebulator_tests)
