add_library(zeebulator_libretro SHARED libretro_core.cpp)
target_link_libraries(zeebulator_libretro PRIVATE zeebulator_core)

# Matches standard libretro core naming (e.g. zeebulator_libretro.so/.dylib/.dll),
# no "lib" prefix, so RetroArch's core loader recognizes it.
set_target_properties(zeebulator_libretro PROPERTIES
  PREFIX ""
  OUTPUT_NAME "zeebulator_libretro"
)
