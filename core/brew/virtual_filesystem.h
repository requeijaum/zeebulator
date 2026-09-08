#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/loader/ggz.h"

namespace zeebulator {

// In-memory virtual filesystem exposed to the IFile/IFileMgr HLE
// implementations. Populated from a loaded GGZ archive's decompressed
// contents -- games never get access to the real host filesystem (see
// ARCHITECTURE.md 3.4). Flat namespace (no directories), matching how
// GGZ entries are named.
class VirtualFilesystem {
 public:
  void AddFile(std::string name, std::vector<uint8_t> data);

  bool Exists(const std::string& name) const;

  // Returns nullptr if `name` isn't present.
  const std::vector<uint8_t>* Find(const std::string& name) const;

  // Names in insertion order, for IFileMgr's directory-enumeration
  // methods (EnumInit/EnumNext).
  const std::vector<std::string>& Names() const { return names_; }

  // Lazy loose-asset resolver (2026-09-08). Installed by the harness
  // (game_probe) to load a LOOSE sibling file from the host FS ON DEMAND --
  // only when the guest actually opens a name that misses every in-VFS
  // lookup. This replaces the old eager "register every sibling at boot"
  // pass (commit 86463ac), which polluted the flat namespace + basename
  // fallback and silently regressed titles like Double Dragon that share a
  // folder with unrelated loose files. The resolver receives the requested
  // basename; on a hit it fills `out` and returns true, and Find() caches
  // the bytes so subsequent opens hit the map directly. Games still never
  // touch the host FS themselves -- the resolver is a harness-level hook,
  // exactly like the code it replaces.
  using MissResolver =
      std::function<bool(const std::string& basename, std::vector<uint8_t>& out)>;
  void SetMissResolver(MissResolver resolver) { miss_resolver_ = std::move(resolver); }

 private:
  // Mutable so the const Find() can memoize a lazily-resolved loose asset.
  mutable std::unordered_map<std::string, std::vector<uint8_t>> files_;
  mutable std::vector<std::string> names_;
  MissResolver miss_resolver_;
};

// Populates a VirtualFilesystem by decompressing every entry in a
// parsed GgzArchive. Eager (decompresses everything up front) --
// correctness-first per Design Principle 4; revisit lazily-decompressing
// on first access if a real game's GGZ archive makes that matter.
VirtualFilesystem BuildVirtualFilesystemFromGgz(const GgzArchive& archive);

}  // namespace zeebulator
