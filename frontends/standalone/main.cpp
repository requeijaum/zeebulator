// Standalone dev/debug frontend. Currently boots the bundled hello_brew
// M0 demo app (see tests/fixtures/hello_brew/) through the real BREW
// app-lifecycle contract and shows the result in an actual SDL2 window
// -- this is PRD.md's M0 milestone. Loading arbitrary real games is
// later work (needs the .mod entry-point-discovery and GGZ/MIF wiring
// from later phases); for now this frontend exists to prove the whole
// pipeline (CPU core -> HLE -> Backend -> window) actually works.

#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/audio/mixer.h"
#include "core/brew/gl_hle.h"
#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/brew/media_hle.h"
#include "core/brew/virtual_filesystem.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"
#include "frontends/standalone/sdl2_backend.h"
#include "frontends/standalone/sdl2_gl_backend.h"
#include "tests/fixtures/hello_brew/entry_offset.h"
#include "tests/fixtures/hello_gl/entry_offset.h"

int main(int argc, char** argv) {
  const char* fixture_path = argc > 1 ? argv[1] : HELLO_BREW_FIXTURE_PATH;

  std::ifstream in(fixture_path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", fixture_path);
    return 1;
  }
  std::vector<uint8_t> mod_data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  constexpr int kWidth = 640;
  constexpr int kHeight = 480;  // Zeebo's native output resolution.
  // Matches Mixer's fixed output rate below -- also the real sample rate
  // of every actual Double Dragon audio asset (see TASKS.md Phase 6).
  constexpr int kAudioSampleRate = 22050;
  SDL_Window* window =
      SDL_CreateWindow("Zeebulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                        kWidth, kHeight, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  zeebulator::ArmInterpreter cpu;
  zeebulator::HleRuntime hle(cpu, 0xF0000000, 0x10000);
  zeebulator::Sdl2Backend backend(renderer, kWidth, kHeight, kAudioSampleRate);
  zeebulator::IDisplayHle display(backend, kWidth, kHeight);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);

  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);
  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle, kWidth, kHeight);
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_obj);
  uint32_t shell =
      shell_hle.Build(/*vtable=*/0x80000000, /*object=*/0x80001000);

  uint32_t entry = kBase + kHelloBrewAeeModLoadOffset;

  // Drive the real app lifecycle: AEEMod_Load -> IModule::CreateInstance
  // -> HandleEvent(EVT_APP_START). See TASKS.md Phase 3 for the contract.
  constexpr uint32_t kPpModAddr = 0x00090000;
  hle.CallArmFunction(entry, /*pIShell=*/shell, /*ph=*/0, /*ppMod=*/kPpModAddr);
  uint32_t module_ptr = cpu.GetMemory().Read32(kPpModAddr);

  uint32_t module_vtable = cpu.GetMemory().Read32(module_ptr);
  uint32_t create_instance_fn = cpu.GetMemory().Read32(module_vtable + 2 * 4);

  constexpr uint32_t kPpObjAddr = 0x00090010;
  constexpr uint32_t kClsId = 0x1234;
  hle.CallArmFunction(create_instance_fn, module_ptr, shell, kClsId, kPpObjAddr);
  uint32_t handle_event_fn = cpu.GetMemory().Read32(kPpObjAddr);

  constexpr uint32_t kAppStartAddr = 0x00090020;
  auto& mem = cpu.GetMemory();
  mem.Write32(kAppStartAddr + 0, 0);            // error
  mem.Write32(kAppStartAddr + 4, kClsId);       // clsApp
  mem.Write32(kAppStartAddr + 8, display_obj);  // pDisplay
  mem.Write32(kAppStartAddr + 12, 0);
  mem.Write32(kAppStartAddr + 16, 0);
  mem.Write32(kAppStartAddr + 20, 0);
  mem.Write32(kAppStartAddr + 24, 0);

  constexpr uint32_t kTestEvtAppStart = 1;  // matches hello_brew.c's own constant
  hle.CallArmFunction(handle_event_fn, /*pMe=*/0, kTestEvtAppStart, /*wParam=*/0,
                       kAppStartAddr);

  std::printf("Zeebulator: hello_brew booted -- window should show a white block\n");

  // --- Phase 5 GL demo: real compiled ARM code, not hand-driven calls ---
  // Separate window, separate real GL context, and (unlike the
  // hello_brew section above) a separate CPU/memory instance entirely:
  // proves the IGL/IEGL HLE vtables reach a real host OpenGL context end
  // to end, through real compiled ARM code exercising the vtable
  // dispatch itself -- see tests/fixtures/hello_gl/ and TASKS.md Phase
  // 5's "validate against SDK sample apps" item.
  //
  // Why a separate CPU instance, not just a different load address in
  // the shared one (as first attempted): hello_gl.bin (like hello_brew.c
  // before it) turns out not to be truly position-independent --
  // `arm-none-eabi-gcc` without `-fPIC`/ROPI flags bakes `&g_module`'s
  // *absolute* link-time address (0x001013e8) into a literal pool rather
  // than computing it PC-relative, confirmed via objdump (`ldr r2, [pc,
  // #20]` loading a fixed `.word 0x001013e8`). That only ever worked
  // before because every existing fixture happened to be loaded at
  // exactly the address it was linked for (0x00100000) -- this demo is
  // the first thing in the project to actually attempt loading a second,
  // independent `.mod` alongside another, which surfaced it. A real
  // BREW-compiled `.mod` (RVCT `armcc --apcs /ropi`) doesn't have this
  // problem; it's specific to our own gcc-built test-fixture convention,
  // not a real ABI gap -- worth revisiting if a future fixture needs
  // true load-address independence, but the easy, correct fix here is to
  // just give this demo its own CPU/memory space and load hello_gl.bin
  // at the same 0x00100000 base it was linked for.
  std::ifstream gl_in(HELLO_GL_FIXTURE_PATH, std::ios::binary);
  std::vector<uint8_t> gl_mod_data((std::istreambuf_iterator<char>(gl_in)),
                                    std::istreambuf_iterator<char>());

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_Window* gl_window =
      SDL_CreateWindow("Zeebulator - hello_gl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                        kWidth, kHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);

  zeebulator::ArmInterpreter gl_cpu;
  zeebulator::HleRuntime gl_hle_runtime(gl_cpu, 0xF0000000, 0x10000);
  zeebulator::Sdl2GlBackend gl_backend(gl_window);
  zeebulator::GlHle gl_hle(gl_backend);
  uint32_t gl_obj = gl_hle.BuildGl(gl_cpu.GetMemory(), gl_hle_runtime, /*vtable=*/0x80000000,
                                    /*object=*/0x80001000);
  uint32_t egl_obj = gl_hle.BuildEgl(gl_cpu.GetMemory(), gl_hle_runtime, /*vtable=*/0x80002000,
                                      /*object=*/0x80003000);

  constexpr uint32_t kGlBase = 0x00100000;  // hello_gl.bin's own link base
  zeebulator::LoadMod(gl_cpu, gl_mod_data, kGlBase);
  uint32_t gl_entry = kGlBase + kHelloGlAeeModLoadOffset;

  constexpr uint32_t kGlPpModAddr = 0x00090000;
  gl_hle_runtime.CallArmFunction(gl_entry, /*pIShell=*/0, /*ph=*/0, /*ppMod=*/kGlPpModAddr);
  auto& gl_mem = gl_cpu.GetMemory();
  uint32_t gl_module_ptr = gl_mem.Read32(kGlPpModAddr);
  uint32_t gl_module_vtable = gl_mem.Read32(gl_module_ptr);
  uint32_t gl_create_instance_fn = gl_mem.Read32(gl_module_vtable + 2 * 4);

  constexpr uint32_t kGlPpObjAddr = 0x00090010;
  gl_hle_runtime.CallArmFunction(gl_create_instance_fn, gl_module_ptr, /*pIShell=*/0,
                                  /*ClsId=*/0x5678, kGlPpObjAddr);
  uint32_t gl_handle_event_fn = gl_mem.Read32(kGlPpObjAddr);

  // GlDemoParams { IGL *pIGL; IEGL *pIEGL; } -- see hello_gl.c.
  constexpr uint32_t kGlParamsAddr = 0x00090020;
  gl_mem.Write32(kGlParamsAddr + 0, gl_obj);
  gl_mem.Write32(kGlParamsAddr + 4, egl_obj);

  constexpr uint32_t kTestEvtGlDemo = 2;  // matches hello_gl.c's own constant
  gl_hle_runtime.CallArmFunction(gl_handle_event_fn, /*pMe=*/0, kTestEvtGlDemo, /*wParam=*/0,
                                  kGlParamsAddr);

  std::printf(
      "Zeebulator: hello_gl booted -- second window should show a real "
      "red/green/blue triangle\n");

  // --- Phase 6 audio demo -------------------------------------------
  // Real IMedia HLE, driven directly (like the Phase 5 GL smoke-test
  // increment, before hello_gl.bin existed to validate it against real
  // compiled code) rather than through a compiled test app -- proves the
  // WAV/MIDI decode -> IMedia HLE -> Mixer -> real SDL2 audio device path
  // works end to end for both real target-game codecs. test_tone.wav and
  // test_tune.mid are our own synthesized/composed content (see their
  // READMEs), loaded into a VirtualFilesystem exactly the way a
  // GGZ-backed one would be.
  zeebulator::VirtualFilesystem audio_vfs;
  auto load_fixture = [&](const char* path, const std::string& vfs_name) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    audio_vfs.AddFile(vfs_name, std::move(data));
  };
  load_fixture(TEST_TONE_FIXTURE_PATH, "test_tone.wav");
  load_fixture(TEST_TUNE_FIXTURE_PATH, "test_tune.mid");

  zeebulator::Mixer mixer(kAudioSampleRate);
  constexpr uint32_t kMediaVtable = 0x80008000;
  zeebulator::MediaHle media_hle(cpu.GetMemory(), hle, audio_vfs, mixer, /*object_region=*/
                                  0x00092000);
  media_hle.Build(kMediaVtable);
  constexpr uint32_t kParmMediaData = 1;
  uint32_t set_media_parm_fn = mem.Read32(kMediaVtable + 4 * 4);  // slot 4 = SetMediaParm
  uint32_t play_fn = mem.Read32(kMediaVtable + 6 * 4);            // slot 6 = Play

  // AEEMediaData { AEECLSID clsData; void *pData; uint32 dwSize; }
  uint32_t scratch = 0x00093000;
  auto play_media = [&](const std::string& vfs_name) {
    uint32_t media_data_addr = scratch;
    uint32_t name_addr = scratch + 0x100;
    scratch += 0x200;
    for (size_t i = 0; i < vfs_name.size(); ++i) {
      mem.Write8(name_addr + static_cast<uint32_t>(i), static_cast<uint8_t>(vfs_name[i]));
    }
    mem.Write8(name_addr + static_cast<uint32_t>(vfs_name.size()), 0);
    mem.Write32(media_data_addr + 0, 0);  // MMD_FILE_NAME
    mem.Write32(media_data_addr + 4, name_addr);
    mem.Write32(media_data_addr + 8, 0);

    uint32_t media_obj = media_hle.CreateMediaObject();
    hle.CallArmFunction(set_media_parm_fn, media_obj, kParmMediaData, media_data_addr, 0);
    hle.CallArmFunction(play_fn, media_obj);
  };
  play_media("test_tone.wav");
  play_media("test_tune.mid");

  std::printf(
      "Zeebulator: playing a real 440Hz test tone and a 4-note MIDI tune "
      "through SDL2 audio\n");

  bool running = true;
  SDL_Event event;
  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
    }
    // One mixer tick per ~16ms frame -- see Mixer's class doc for why
    // this simple per-tick pull (rather than a real-time audio callback
    // thread) is an acceptable, documented simplification for now.
    mixer.Mix(backend, static_cast<size_t>(kAudioSampleRate * 16 / 1000));
    SDL_Delay(16);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_DestroyWindow(gl_window);
  SDL_Quit();
  return 0;
}
