#include "core/brew/compat/title_quirks.h"

namespace zeebulator::compat {

const std::vector<TitleQuirk>& KnownTitles() {
  // Only titles we have ACTUALLY RUN under this emulator appear here.
  // Each row is measured, not guessed; `evidence` points at the dossier.
  static const std::vector<TitleQuirk> kTitles = {
      TitleQuirk{
          /*real_clsid=*/0x0102F789u,  // 16971657
          /*mif_clsid=*/0x0102F789u,   // same -- MIF id happened to be real
          /*folder=*/"274754",
          /*display_name=*/"Double Dragon",
          /*mod_filename=*/"ddragonz.mod",
          /*assets=*/AssetKind::kGgzPair,
          /*status=*/BootStatus::kPlayable,
          /*notes=*/
          "Full BREW lifecycle -> splash 31 FPS -> gameplay (combat + HUD) "
          "-> audio via GM soundfont. Media-lifecycle NULL-wander fixed "
          "host-side (media-binding guard region 0x80200000-0x80300000; "
          "Play=vtable[6] at helper 0x11d04c; Release-clear at 0x11f424 "
          "suppressed). Final audio tick is input-gated -> headless cannot "
          "reach it; interactive validation pending (Rafael).",
          /*evidence=*/"research/sources/2026-08-31_dd-media-interface-contract.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108E356u,  // 17359702 -- traced from CreateInstance
          /*mif_clsid=*/0x0103081Du,   // 16975901 -- MIF value is a DECOY
          /*folder=*/"279369",
          /*display_name=*/"Alien Breaker Deluxe",
          /*mod_filename=*/"abd.mod",
          /*assets=*/AssetKind::kBar,  // data.bar (9.16 MB), no ggz
          /*status=*/BootStatus::kInGame,
          /*notes=*/
          "AEEMod_Load OK -> CreateInstance OK (applet 0x80300024) -> "
          "EVT_APP_START/RESUME -> event loop -> ticks 0-9 (748 HLE calls) "
          "-> runaway timer callback exceeds the 5M-step budget. Wall "
          "localized 2026-09-01: tight 4-trap cycle "
          "0xf0000ecc->0xf0000ff4->0xf0000fd8->0xf0000f20 on the "
          "self-propagating stub object 0x800b1800 (all slots return 0), "
          "iterator cursor r2 frozen at 0x80310a7c; loop body abd.mod "
          "~0x115ae0/0x115d38. Real clsid found by tracing CreateInstance "
          "(cmp r0,r1 vs literal 0x0108e356 at abd.mod +0x00100790), NOT "
          "the MIF id.",
          /*evidence=*/"research/sources/2026-09-01_abd-bringup.md",
      },
  };
  return kTitles;
}

std::optional<TitleQuirk> FindByClsid(uint32_t real_clsid) {
  for (const auto& t : KnownTitles()) {
    if (t.real_clsid == real_clsid) return t;
  }
  return std::nullopt;
}

std::optional<TitleQuirk> FindByFolder(const std::string& folder) {
  for (const auto& t : KnownTitles()) {
    if (t.folder == folder) return t;
  }
  return std::nullopt;
}

const char* BootStatusName(BootStatus status) {
  switch (status) {
    case BootStatus::kUntested:           return "untested";
    case BootStatus::kLoadFail:           return "load-fail";
    case BootStatus::kCreateInstanceFail: return "createinstance-fail";
    case BootStatus::kBoots:              return "boots";
    case BootStatus::kInGame:             return "in-game";
    case BootStatus::kPlayable:           return "playable";
  }
  return "unknown";
}

}  // namespace zeebulator::compat
