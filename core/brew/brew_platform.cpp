#include "core/brew/brew_platform.h"

namespace zeebulator {

BrewPlatform::BrewPlatform(IArmCore& cpu, HleRuntime& hle, VirtualFilesystem& vfs,
                           IDisplayHle& display, GlBackend& gl_backend, Mixer& mixer,
                           const Config& config)
    : cpu_(cpu),
      hle_(hle),
      vfs_(vfs),
      display_(display),
      gl_backend_(gl_backend),
      mixer_(mixer),
      config_(config),
      shell_(cpu.GetMemory(), hle, config.screen_width, config.screen_height),
      mod_runtime_(cpu.GetMemory(), hle, config.heap_region, config.heap_size,
                   config.context_address),
      file_(cpu.GetMemory(), hle, vfs, /*file_object_region_start=*/0x80100000),
      media_(cpu.GetMemory(), hle, vfs, mixer, /*object_region_start=*/0x80200000),
      gl_(gl_backend),
      heap_(cpu.GetMemory(), hle,
            [this](uint32_t sz) { return mod_runtime_.Allocate(sz); },
            [this](uint32_t ptr, uint32_t sz) { return mod_runtime_.Reallocate(ptr, sz); },
            nullptr,
            [this]() { return mod_runtime_.GetHeapAvailBytes(); },
            [this]() { return mod_runtime_.GetHeapUsedBytes(); }),
      hash_(cpu.GetMemory(), hle,
            [this](uint32_t sz) { return mod_runtime_.Allocate(sz); }),
      mem_astream_(cpu.GetMemory(), hle, /*stream_object_region_start=*/0x80085000),
      unzip_stream_(cpu.GetMemory(), hle, /*object_region_start=*/0x80087000),
      thread_(cpu.GetMemory(), hle,
              [this](uint32_t sz) { return mod_runtime_.Allocate(sz); }, nullptr) {}

void BrewPlatform::Build() {
  // Helper table first: the module's own bootstrap reads it from
  // module_base - 4 before anything else runs.
  mod_runtime_.Install(config_.module_base, config_.helper_table_address);

  BuildDisplay();
  BuildFileAndStreams();
  BuildGraphics();
  BuildRuntimeServices();
  BuildAudio();

  shell_object_ = shell_.Build(/*vtable=*/0x80000000, /*object=*/0x80001000);
  mod_runtime_.SetShellInstance(shell_object_);
  mod_runtime_.SetDisplayInstance(display_object_);
}

void BrewPlatform::BuildDisplay() {
  display_object_ =
      display_.Build(cpu_.GetMemory(), hle_, /*vtable=*/0x80002000, /*object=*/0x80003000);
  shell_.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_object_);
  shell_.RegisterInstance(/*AEECLSID_DISPLAY1=*/0x010127d4, display_object_);
}

void BrewPlatform::BuildFileAndStreams() {
  uint32_t file_mgr_obj = file_.Build(/*file_mgr_vtable=*/0x80004000,
                                      /*file_mgr_object=*/0x80005000,
                                      /*file_vtable=*/0x80006000);
  shell_.RegisterInstance(/*AEECLSID_FILEMGR=*/0x01001003, file_mgr_obj);

  mem_astream_.Build(/*vtable=*/0x80084000);
  shell_.RegisterFactory(MemAStreamHle::kClsidMemAStream,
                         [this]() { return mem_astream_.AllocateStream(); });

  unzip_stream_.Build(/*vtable=*/0x80086000);
  shell_.RegisterFactory(UnzipStreamHle::kClsidUnzipStream,
                         [this]() { return unzip_stream_.AllocateStream(); });
}

void BrewPlatform::BuildGraphics() {
  // Legacy AEEGL.h interfaces (AEECLSID_GL/AEECLSID_EGL) plus the unified
  // Qualcomm class. Crash Nitro Kart 2 asks for the legacy pair directly.
  uint32_t gl_obj = gl_.BuildGl(cpu_.GetMemory(), hle_, /*vtable=*/0x80007000,
                                /*object=*/0x80008000);
  gl_.SetGlObject(gl_obj);
  shell_.RegisterInstance(/*AEECLSID_GL=*/0x01014bc3, gl_obj);

  uint32_t egl_obj = gl_.BuildEgl(cpu_.GetMemory(), hle_, /*vtable=*/0x80009000,
                                  /*object=*/0x8000A000);
  gl_.SetEglObject(egl_obj);
  shell_.RegisterInstance(/*AEECLSID_EGL=*/0x01014bc4, egl_obj);
  shell_.RegisterInstance(/*AEECLSID_QEGL=*/0x0103d8ec, egl_obj);

  uint32_t gles11_obj = gl_.BuildGles11(cpu_.GetMemory(), hle_, /*vtable=*/0x80088000,
                                        /*object=*/0x80089000);
  gl_.SetGles11Object(gles11_obj);
}

void BrewPlatform::BuildRuntimeServices() {
  shell_.SetThreadHle(&thread_);
  shell_.RegisterFactory(/*AEECLSID_THREAD=*/0x01001017,
                         [this]() { return thread_.CreateThreadObject(); });

  uint32_t heap_obj = heap_.Build(/*vtable=*/0x80080000, /*object=*/0x80081000);
  shell_.RegisterInstance(HeapHle::kClsidHeap, heap_obj);

  uint32_t hash_obj = hash_.Build(/*vtable=*/0x80082000, /*object=*/0x80083000);
  shell_.RegisterInstance(HashHle::kClsidMd5, hash_obj);
}

void BrewPlatform::BuildAudio() {
  media_.Build(/*vtable=*/0x8000B000);
  // A fresh IMedia per sound: real code creates one per activated clip
  // rather than sharing a single instance (see media_hle.h).
  shell_.RegisterFactory(/*AEECLSID_MEDIA=*/0x01005500,
                         [this]() { return media_.CreateMediaObject(); });
  shell_.RegisterFactory(0x0100550a, [this]() { return media_.CreateMediaObject(); });
  shell_.RegisterFactory(0x01005501, [this]() { return media_.CreateMediaObject(); });

  // Media-interface binding guard: matches MediaHle's object_region so a
  // game's Release-clear cannot unbind a live HLE-owned interface.
  cpu_.GetMemory().SetMediaBindingGuardRegion(0x80200000, 0x80300000);
}

}  // namespace zeebulator
