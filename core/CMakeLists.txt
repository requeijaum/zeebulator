configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/soundfont_path.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/soundfont_path.h
  @ONLY
)

add_library(zeebulator_core STATIC
  core.cpp
  core.h
  backend.h
  save_state.cpp
  save_state.h
  gl_texture_log.cpp
  gl_texture_log.h
  memory/memory.cpp
  memory/memory.h
  cpu/arm_core.h
  cpu/arm_interpreter.cpp
  cpu/arm_interpreter.h
  cpu/dynarmic_arm_core.cpp
  cpu/dynarmic_arm_core.h
  loader/bar.cpp
  loader/bar.h
  loader/aez.cpp
  loader/fufs.cpp
  loader/fufs.h
  loader/ggz.cpp
  loader/ggz.h
  loader/mif.cpp
  loader/mif.h
  loader/mod.cpp
  loader/mod.h
  loader/obm1.cpp
  loader/obm1.h
  loader/pkg.cpp
  loader/pkg.h
  loader/pakz.cpp
  loader/pakz.h
  loader/wav.cpp
  loader/wav.h
  loader/mp3.cpp
  loader/mp3.h
  loader/midi.cpp
  loader/midi.h
  loader/atitc.cpp
  loader/atitc.h
  loader/png.cpp
  loader/png.h
  loader/sar.cpp
  loader/sar.h
  loader/tga.cpp
  loader/tga.h
  loader/gif.cpp
  loader/gif.h
  audio/mixer.cpp
  audio/mixer.h
  audio/soundfont_synth.cpp
  audio/soundfont_synth.h
  brew/hle_runtime.cpp
  brew/hle_runtime.h
  brew/nid_table.cpp
  control/debug_hooks.cpp
  control/call_stack_tracer.cpp
  control/call_stack_tracer.h
  brew/nid_table.h
  brew/interface_object.cpp
  brew/interface_object.h
  brew/scaffold_object.cpp
  brew/scaffold_object.h
  brew/brew_resource_file.cpp
  brew/ishell.cpp
  brew/ishell.h
  brew/idisplay.cpp
  brew/idisplay.h
  brew/bitmap_hle.cpp
  brew/bitmap_hle.h
  brew/font5x7.cpp
  brew/font5x7.h
  brew/virtual_filesystem.cpp
  brew/virtual_filesystem.h
  brew/brew_platform.cpp
  brew/file_hle.cpp
  brew/file_hle.h
  brew/media_hle.cpp
  brew/media_hle.h
  brew/thread_hle.cpp
  brew/thread_hle.h
  brew/heap_hle.cpp
  brew/heap_hle.h
  brew/hash_hle.cpp
  brew/hash_hle.h
  brew/mem_astream_hle.cpp
  brew/mem_astream_hle.h
  brew/sql_hle.cpp
  brew/sql_hle.h
  brew/unzip_stream_hle.cpp
  brew/unzip_stream_hle.h
  brew/mod_runtime.cpp
  brew/mod_runtime.h
  brew/gl_types.h
  brew/gl_backend.h
  brew/gl_hle.cpp
  brew/gl_hle.h
  brew/soft_gl_backend.cpp
  brew/soft_gl_backend.h
  brew/hid_hle.cpp
  brew/hid_hle.h
  brew/compat/title_quirks.cpp
  brew/compat/title_quirks.h
)

target_include_directories(zeebulator_core PUBLIC ${PROJECT_SOURCE_DIR})
# zlib's CMakeLists predates modern usage-requirement propagation, so its
# generated zconf.h (in the binary dir) needs to be added explicitly.
target_include_directories(zeebulator_core PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(zeebulator_core PRIVATE zlibstatic liblzma sqlite3)
# dynarmic A32 JIT backend (Phase 9a): DynarmicArmCore's real JIT core. The
# `dynarmic` target propagates its own public include dir (src/), so
# #include "dynarmic/interface/A32/a32.h" resolves without extra wiring.
target_link_libraries(zeebulator_core PRIVATE dynarmic)
# liblzma's own CMakeLists doesn't propagate its public API header dir as
# a usage requirement (unlike a `find_package(LibLZMA)`-found install
# would via pkg-config), so add it explicitly, the same way zlib's own
# generated zconf.h needs to be above.
target_include_directories(zeebulator_core PRIVATE ${liblzma_SOURCE_DIR}/src/liblzma/api)
# TinySoundFont is header-only (no CMakeLists of its own to depend on);
# the generated/ dir holds this build's own configure_file output
# (soundfont_path.h, the real fetched .sf2's real absolute path).
target_include_directories(zeebulator_core PRIVATE
  ${tinysoundfont_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# minimp3 is a bundled single-header MP3 decoder (public domain / CC0), used
# by loader/mp3.cpp. Header-only: just needs its dir on the include path.
target_include_directories(zeebulator_core PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/minimp3)
