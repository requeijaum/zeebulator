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
      TitleQuirk{
          /*real_clsid=*/0x0108FF18u,  // 17366808 -- neighbourhood sweep of first-party band
          /*mif_clsid=*/0x01060000u,   // MIF value is a decoy
          /*folder=*/"279159",
          /*display_name=*/"Zeebo Peteca",
          /*mod_filename=*/"zeebopeteca.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "First-party Zeebo title. Real clsid recovered 2026-09-01 by "
          "neighbourhood sweep of the first-party band 0x0108ffxx (tennis "
          "0x0108eff9 / volley 0x0108ff15 / peteca 0x0108ff18 / footparty "
          "0x0108ff19 / ids 0x0108ff1a), verified through the real probe "
          "(CreateInstance accepted, reaches tick=1). MIF id 0x01060000 is a "
          "decoy.",
          /*evidence=*/"research/sources/2026-09-01_clsid-recovery-breadth.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x01087B72u,  // 17333106 -- Pac-Mania
          /*mif_clsid=*/0x01087B72u,
          /*folder=*/"276212",
          /*display_name=*/"Pac-Mania",
          /*mod_filename=*/"pacmania.mod",
          /*assets=*/AssetKind::kBar,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Namco Bandai port. Real clsid 0x01087b72 boots to event loop with "
          "mod_runtime static-base slot 0x50 wired. Calls ABD render scaffold "
          "0x800b1800 extensively each tick.",
          /*evidence=*/"research/sources/2026-09-02_clsid-static-recovery.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108AF6Cu,  // 17346412 -- Resident Evil 4 (bio4_brew)
          /*mif_clsid=*/0x0108AF6Cu,
          /*folder=*/"276675",
          /*display_name=*/"Resident Evil 4",
          /*mod_filename=*/"bio4_brew.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Capcom/Zeebo port. Real clsid 0x0108af6c boots to event loop with "
          "CreateInstance OK.",
          /*evidence=*/"research/sources/2026-09-02_clsid-static-recovery.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x010A2335u,  // 17441589 -- Activity Center
          /*mif_clsid=*/0x010A2335u,
          /*folder=*/"280634",
          /*display_name=*/"Activity Center",
          /*mod_filename=*/"activitycenter.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Zeebo title. Real clsid 0x010a2335 boots to event loop with "
          "CreateInstance OK.",
          /*evidence=*/"research/sources/2026-09-02_clsid-static-recovery.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x010A2337u,  // 17441591 -- Alice in Wonderland
          /*mif_clsid=*/0x010A2337u,
          /*folder=*/"280386",
          /*display_name=*/"Alice in Wonderland",
          /*mod_filename=*/"alice.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Zeebo title. Real clsid 0x010a2337 boots to event loop with "
          "CreateInstance OK (sister title of activitycenter).",
          /*evidence=*/"research/sources/2026-09-02_clsid-static-recovery.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF19u,  // 17366809 -- sits between peteca and ids
          /*mif_clsid=*/0x01060000u,   // MIF value is a decoy
          /*folder=*/"279380",
          /*display_name=*/"Foot Party",
          /*mod_filename=*/"footparty.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "First-party Zeebo title. Real clsid recovered 2026-09-01 by "
          "neighbourhood sweep; 0x0108ff19 sits exactly between peteca "
          "(0x0108ff18) and ids (0x0108ff1a), confirming the sequential "
          "first-party clsid allocation. Verified through the real probe "
          "(reaches tick=1). MIF id 0x01060000 is a decoy.",
          /*evidence=*/"research/sources/2026-09-01_clsid-recovery-breadth.md",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF16u,  // 17366806 -- Dodgeball
          /*mif_clsid=*/0x010C0C0Cu,   // MIF value is a decoy
          /*folder=*/"278738",
          /*display_name=*/"Zeebo Sports Queimada / Dodgeball",
          /*mod_filename=*/"dodgeball.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "First-party Zeebo Sports title. Real clsid 0x0108ff16 in the 0x0108ffxx band. "
          "Reaches event loop and tick execution.",
          /*evidence=*/"research/sources/scripts/hackathon_clsid.json",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF17u,  // 17366807 -- Fun Soccer
          /*mif_clsid=*/0x010C0C0Cu,   // MIF value is a decoy
          /*folder=*/"280647",
          /*display_name=*/"Fun Soccer",
          /*mod_filename=*/"funsoccer.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "First-party Zeebo title. Real clsid 0x0108ff17 in the 0x0108ffxx band. "
          "Reaches event loop and tick execution.",
          /*evidence=*/"research/sources/scripts/hackathon_clsid.json",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108EFF9u,  // 17362937 -- Zeebo Sports Tennis
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"277534",
          /*display_name=*/"Zeebo Sports Tennis",
          /*mod_filename=*/"zeebotennis.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Sports title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF15u,  // 17366805 -- Zeebo Sports Volley
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"278212",
          /*display_name=*/"Zeebo Sports Volei",
          /*mod_filename=*/"zeebovolley.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Sports title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF1Au,  // 17366810 -- Zeeboids
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"279382",
          /*display_name=*/"Zeeboids",
          /*mod_filename=*/"zeeboids.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF06u,  // 17366790 -- Zeebo Extreme Air Race
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"277285",
          /*display_name=*/"Zeebo Extreme Air Race",
          /*mod_filename=*/"AirRacez.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Extreme title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF07u,  // 17366791 -- Zeebo Extreme Baja
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"277727",
          /*display_name=*/"Zeebo Extreme Baja",
          /*mod_filename=*/"Bajaz.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Extreme title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF13u,  // 17366803 -- Zeebo Extreme Bóia Cross
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"278285",
          /*display_name=*/"Zeebo Extreme Boia Cross",
          /*mod_filename=*/"Boiaz.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Extreme title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108FF14u,  // 17366804 -- Zeebo Extreme Jetboard
          /*mif_clsid=*/0x01060000u,
          /*folder=*/"278283",
          /*display_name=*/"Zeebo Extreme Jetboard",
          /*mod_filename=*/"JetBoardz.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"First-party Zeebo Extreme title. Reaches event loop.",
          /*evidence=*/"research/sources/scripts/boot_matrix.py",
      },
      // --- Titles verified 2026-09-02 via MIF-tail CLSID recovery ------------
      // The real AEECLSID lives in the last 20 bytes of each title's .mif
      // (folder.mif), at byte offset len-20 as a little-endian uint32. This
      // was confirmed against every already-known title (pacmania 0x01087b72,
      // RE4 0x0108af6c, quake 0x01087a3c, activitycenter 0x010a2335, ...) and
      // then used to recover CLSIDs for the rest of the corpus, each verified
      // through the real probe (CreateInstance accepted + reaches the event
      // loop with no unhandled instruction). See research/sources/scripts/
      // clsid_validate.sh.
      TitleQuirk{
          /*real_clsid=*/0x01087B73u,  // 17333107 -- Ridge Racer
          /*mif_clsid=*/0x01087B73u,
          /*folder=*/"276152",
          /*display_name=*/"Ridge Racer",
          /*mod_filename=*/"ridgeracer.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Namco port. CLSID recovered from MIF tail; CreateInstance OK, "
          "reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x0108C0BBu,  // 17350843 -- Rolimã (Zeebo Extreme Rolimã)
          /*mif_clsid=*/0x0108C0BBu,
          /*folder=*/"276809",
          /*display_name=*/"Zeebo Extreme Rolima",
          /*mod_filename=*/"Rolimaz.mod",
          /*assets=*/AssetKind::kNone,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "First-party Zeebo Extreme title. CLSID from MIF tail; reaches "
          "event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x01099CD6u,  // 17407190 -- Peggle
          /*mif_clsid=*/0x01099CD6u,
          /*folder=*/"278962",
          /*display_name=*/"Peggle",
          /*mod_filename=*/"peggle.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "PopCap port. CLSID from MIF tail; CreateInstance OK, reaches event "
          "loop. Peggle per-tick loop already studied (TASKS.md Phase 8).",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924DEu,  // 17376478 -- Caveman Ninja / Joe & Mac
          /*mif_clsid=*/0x010924DEu,
          /*folder=*/"278986",
          /*display_name=*/"Caveman Ninja",
          /*mod_filename=*/"cninja.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/
          "Data East / G-mode arcade port. CLSID from MIF tail; reaches event "
          "loop (also in the regression audit).",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924E1u,  // 17376481 -- Spin Master
          /*mif_clsid=*/0x010924E1u,
          /*folder=*/"278987",
          /*display_name=*/"Spin Master",
          /*mod_filename=*/"spinmast.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924E2u,  // 17376482 -- Street Hoop
          /*mif_clsid=*/0x010924E2u,
          /*folder=*/"278988",
          /*display_name=*/"Street Hoop",
          /*mod_filename=*/"strhoop.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010963A5u,  // 17392549 -- (folder 279036 game.mod)
          /*mif_clsid=*/0x010963A5u,
          /*folder=*/"279036",
          /*display_name=*/"Game 279036",
          /*mod_filename=*/"game.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"CLSID from MIF tail; CreateInstance OK, reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924E3u,  // 17376483 -- Super Baseball 2020 (supbtime)
          /*mif_clsid=*/0x010924E3u,
          /*folder=*/"279125",
          /*display_name=*/"Super Baseball / SuperBTime",
          /*mod_filename=*/"supbtime.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924DDu,  // 17376477 -- Karnov's Revenge
          /*mif_clsid=*/0x010924DDu,
          /*folder=*/"279126",
          /*display_name=*/"Karnov's Revenge",
          /*mod_filename=*/"karnovr.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924E4u,  // 17376484 -- Wizard Fire
          /*mif_clsid=*/0x010924E4u,
          /*folder=*/"279173",
          /*display_name=*/"Wizard Fire",
          /*mod_filename=*/"wizdfire.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924E0u,  // 17376480 -- Magical Drop 3
          /*mif_clsid=*/0x010924E0u,
          /*folder=*/"279200",
          /*display_name=*/"Magical Drop 3",
          /*mod_filename=*/"magdrop3.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
      },
      TitleQuirk{
          /*real_clsid=*/0x010924DFu,  // 17376479 -- Dark Seal
          /*mif_clsid=*/0x010924DFu,
          /*folder=*/"279233",
          /*display_name=*/"Dark Seal",
          /*mod_filename=*/"darkseal.mod",
          /*assets=*/AssetKind::kOther,
          /*status=*/BootStatus::kBoots,
          /*notes=*/"G-mode arcade port. CLSID from MIF tail; reaches event loop.",
          /*evidence=*/"research/sources/scripts/clsid_validate.sh",
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
