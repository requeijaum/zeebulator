#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zeebulator::compat {

// Per-title compatibility & quirks layer.
//
// Why this exists: BREW games are not uniform. Each commercial title was
// built against the Zeebo/BREW SDK with its own resource layout, its own
// real AEECLSID (which is frequently NOT the MIF-advertised id nor the
// install-folder number -- see TitleQuirk::real_clsid notes), and its own
// set of engine quirks that the HLE has to satisfy to boot. Rather than
// scatter `if (title == ...)` branches through the harness, this registry
// centralizes the *verified facts* about each title we have actually run,
// keyed by the real class id the game's CreateInstance compares against.
//
// Clean-room discipline: every field here is an OBSERVED fact (measured
// by running the game under our own emulator / reading our own dumps),
// carrying a source note where non-obvious. Nothing is copied from
// Qualcomm/BREW/Infuse code. Untested titles are deliberately absent --
// we do not guess compatibility.

enum class AssetKind {
  kNone,     // .mod (+ .sig) only
  kGgzPair,  // data.ggz + sound.ggz (Double Dragon family)
  kBar,      // one or more .bar resource archives (Alien Breaker Deluxe)
  kPakz,     // .pakz / .pak / .pap / .vfs container
  kOther,    // present but not yet classified for the harness
};

// How far the title has been observed to run under our emulator.
enum class BootStatus {
  kUntested,          // in the corpus, not yet exercised
  kLoadFail,          // AEEMod_Load / loader rejects it
  kCreateInstanceFail,// module loads but CreateInstance EFAILED (clsid decoy?)
  kBoots,             // CreateInstance OK, first events handled, stops early
  kInGame,            // runs its own engine/tick loop, hits a known wall
  kPlayable,          // reaches interactive gameplay (render + audio)
};

struct TitleQuirk {
  // Real AEECLSID the game's CreateInstance actually compares against --
  // the value game_probe must be passed (in decimal). NOT necessarily the
  // MIF-advertised id: e.g. Alien Breaker Deluxe advertises 0x0103081d in
  // its MIF but truly compares against 0x0108e356 (found by tracing
  // CreateInstance). Double Dragon's MIF id 0x0102F789 happened to be real.
  uint32_t real_clsid = 0;

  // The MIF-advertised class id, when it differs from real_clsid (0 if
  // same/unknown). Kept so the "MIF scan is a decoy here" fact is explicit.
  uint32_t mif_clsid = 0;

  // Infuse install-folder number (e.g. "274754"). NEVER the class id, but
  // the stable on-disk key for locating assets.
  std::string folder;

  std::string display_name;   // human title, from MIF/observation
  std::string mod_filename;   // e.g. "ddragonz.mod"
  AssetKind assets = AssetKind::kNone;
  BootStatus status = BootStatus::kUntested;

  // Free-form, source-tagged notes about engine quirks that matter to the
  // HLE (media lifecycle, resource loading path, the current wall, ...).
  std::string notes;

  // Path to the evidence dossier (repo-relative), where the measured boot
  // trace / RE that justifies the above lives.
  std::string evidence;
};

// Returns the registry of titles we have verified facts for. Small and
// hand-curated on purpose; grows one measured title at a time.
const std::vector<TitleQuirk>& KnownTitles();

// Look up a title by its real AEECLSID (the value CreateInstance compares).
std::optional<TitleQuirk> FindByClsid(uint32_t real_clsid);

// Look up a title by its Infuse install-folder number.
std::optional<TitleQuirk> FindByFolder(const std::string& folder);

// Human-readable name for a BootStatus (for compat-list generation / logs).
const char* BootStatusName(BootStatus status);

}  // namespace zeebulator::compat
