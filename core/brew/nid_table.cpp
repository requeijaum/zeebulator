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
    case 0x01001002u: return "AEECLSID_HEAP";
    case 0x01002001u: return "AEECLSID_GRAPHICS";
    case 0x0100100cu: return "AEECLSID_MEMASTREAM";
    case 0x0100100fu: return "AEECLSID_LICENSE";
    case 0x01001056u: return "AEECLSID_SOUND";
    case 0x01041207u: return "AEECLSID_SIGNAL_CB_FACTORY";
    case 0x01005511u: return "AEECLSID_MEDIAPCM";
    case 0x01001017u: return "AEECLSID_THREAD";
    case 0x0101eb0bu: return "AEEIID_FORCEFEED";
    case 0x01026e23u: return "AEECLSID_PNGDECODER";
    case 0x01030766u: return "AEECLSID_PNGDECODER_BREW";
    case 0x01004004u: return "AEECLSID_PNG";
    case 0x01005000u: return "AEECLSID_WEB";
    case 0x01001015u: return "AEECLSID_MD5";
    case 0x0102cce1u: return "AEECLSID_CIPHER_FACTORY";

    // --- ABD (Alien Breaker Deluxe) rendering-engine scaffold: the
    //     200-slot object 0x800b1800 the tick-9 wall cycles on. Naming it
    //     is the whole point of the NID table for Phase 9d. ---
    case 0x0103d8ecu: return "AEECLSID_QEGL";

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
