#include "core/brew/nid_table.h"

#include <cstdio>

namespace zeebulator {

const char* KnownClassName(uint32_t clsid) {
  switch (clsid) {
    // --- Core BREW system interfaces (AEECLSID_* public identifiers) ---
    case 0x01001001u: return "AEECLSID_DISPLAY";
    case 0x010127d4u: return "AEECLSID_DISPLAY1";   // 2nd display, aliases DISPLAY
    case 0x01001003u: return "AEECLSID_FILEMGR";
    case 0x01001014u: return "AEECLSID_FILE";       // last-opened-file proxy
    case 0x01005500u: return "AEECLSID_MEDIA";
    case 0x01005501u: return "AEECLSID_MEDIAMIDI";  // DD BGM
    case 0x0100550au: return "AEECLSID_MEDIAADPCM"; // DD SFX
    case 0x01014bc3u: return "AEECLSID_GL";         // OpenGL ES
    case 0x01014bc4u: return "AEECLSID_EGL";
    case 0x0106c411u: return "AEECLSID_HID";        // input

    // --- Interfaces this project resolved by tracing real CreateInstance
    //     call sites / disassembly but whose public AEECLSID_* symbol is
    //     not yet pinned. Named descriptively, keyed by the real id the
    //     game compares against (see tools/game_probe.cpp for evidence). ---
    case 0x01002001u: return "ZEEBO_UNIDENTIFIED_0x01002001";
    case 0x0100100cu: return "ZEEBO_UNIDENTIFIED_0x0100100c";
    case 0x01041207u: return "ZEEBO_UNIDENTIFIED_0x01041207";
    case 0x01005511u: return "ZEEBO_UNIDENTIFIED_0x01005511";
    case 0x01001017u: return "ZEEBO_UNIDENTIFIED_0x01001017";
    case 0x0101eb0bu: return "ZEEBO_SELFPROP_STUB_0x0101eb0b";
    case 0x01030766u: return "ZEEBO_UNIDENTIFIED_0x01030766";

    // --- ABD (Alien Breaker Deluxe) rendering-engine scaffold: the
    //     200-slot object 0x800b1800 the tick-9 wall cycles on. Naming it
    //     is the whole point of the NID table for Phase 9d. ---
    case 0x0103d8ecu: return "ABD_RENDER_SCAFFOLD";

    // --- Applet class ids (a game's own IApplet, from CreateInstance) ---
    case 0x0102f789u: return "APP_DOUBLE_DRAGON";
    case 0x0108e356u: return "APP_ALIEN_BREAKER_DELUXE";
    case 0x0108ff18u: return "APP_ZEEBO_PETECA";
    case 0x0108ff19u: return "APP_FOOT_PARTY";
    case 0x0108eff9u: return "APP_ZEEBO_TENNIS";
    case 0x0108ff15u: return "APP_ZEEBO_VOLLEY";
    case 0x0108ff1au: return "APP_ZEEBO_IDS";
    case 0x0108ff06u: return "APP_AIR_RACEZ";
    case 0x0108ff07u: return "APP_BAJAZ";
    case 0x0108ff13u: return "APP_BOIAZ";
    case 0x0108ff14u: return "APP_JETBOARDZ";

    default: return nullptr;
  }
}

std::string DescribeClsid(uint32_t clsid) {
  char buf[64];
  const char* name = KnownClassName(clsid);
  std::snprintf(buf, sizeof(buf), "%s (0x%08x)", name ? name : "UNKNOWN", clsid);
  return std::string(buf);
}

}  // namespace zeebulator
