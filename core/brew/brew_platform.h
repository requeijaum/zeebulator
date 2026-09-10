#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/brew/file_hle.h"
#include "core/brew/gl_hle.h"
#include "core/brew/hash_hle.h"
#include "core/brew/heap_hle.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/brew/media_hle.h"
#include "core/brew/mem_astream_hle.h"
#include "core/brew/mod_runtime.h"
#include "core/brew/thread_hle.h"
#include "core/brew/unzip_stream_hle.h"
#include "core/brew/virtual_filesystem.h"
#include "core/cpu/arm_core.h"

namespace zeebulator {

// The BREW platform every frontend needs, in one place.
//
// Why this exists: the whole AEE surface (IShell + every AEECLSID a real
// title creates) used to live inline inside tools/game_probe.cpp's 4000-line
// main(). That made the diagnostic probe the only build that could actually
// run a game: frontends/standalone registered exactly ONE class
// (AEECLSID_DISPLAY) and frontends/libretro registered none, so a title like
// Double Dragon could not even open a file there. This class owns the HLE
// objects and their ClassID registrations so probe and frontends share one
// platform instead of drifting apart.
//
// Address map is inherited verbatim from game_probe.cpp so guest-visible
// layout does not change: vtables/objects keep the exact addresses the
// corpus has been validated against.
class BrewPlatform {
 public:
  struct Config {
    int screen_width = 640;
    int screen_height = 480;
    // ModRuntime's bump heap. 64 MiB at 0x80300000, same as the probe.
    uint32_t heap_region = 0x80300000;
    uint32_t heap_size = 0x04000000;
    uint32_t context_address = 0x80280200;
    uint32_t module_base = 0x00100000;
    uint32_t helper_table_address = 0x80280000;
    // Directory the title was loaded from; replaces the probe's argv[1]
    // use so nothing here needs a command line.
    std::string game_dir;
  };

  BrewPlatform(IArmCore& cpu, HleRuntime& hle, VirtualFilesystem& vfs,
               IDisplayHle& display, GlBackend& gl_backend, Mixer& mixer,
               const Config& config);

  // Constructs every HLE object and registers its ClassID with IShell.
  // Must be called after the .mod is loaded (ModRuntime::Install writes the
  // helper-table pointer at module_base - 4).
  void Build();

  // The IShell object pointer a title's AEEMod_Load / CreateInstance takes.
  uint32_t shell_object() const { return shell_object_; }
  uint32_t display_object() const { return display_object_; }

  IShellHle& shell() { return shell_; }
  ModRuntime& mod_runtime() { return mod_runtime_; }
  FileHle& file() { return file_; }
  MediaHle& media() { return media_; }
  GlHle& gl() { return gl_; }

 private:
  void BuildDisplay();
  void BuildFileAndStreams();
  void BuildGraphics();
  void BuildRuntimeServices();
  void BuildAudio();

  IArmCore& cpu_;
  HleRuntime& hle_;
  VirtualFilesystem& vfs_;
  IDisplayHle& display_;
  GlBackend& gl_backend_;
  Mixer& mixer_;
  Config config_;

  IShellHle shell_;
  ModRuntime mod_runtime_;
  FileHle file_;
  MediaHle media_;
  GlHle gl_;
  HeapHle heap_;
  HashHle hash_;
  MemAStreamHle mem_astream_;
  UnzipStreamHle unzip_stream_;
  ThreadHle thread_;

  uint32_t shell_object_ = 0;
  uint32_t display_object_ = 0;
};

}  // namespace zeebulator
