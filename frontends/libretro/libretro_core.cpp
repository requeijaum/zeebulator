#include "core/core.h"

// Real libretro API entry point — the rest of the retro_* surface
// (retro_load_game, retro_run, environment callbacks, ...) lands in
// TASKS.md Phase 9. This just proves the shared-library target builds and
// links against zeebulator_core.
extern "C" unsigned retro_api_version() { return 1; }
