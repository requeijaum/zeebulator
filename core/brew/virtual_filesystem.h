#pragma once

#include <cstdint>
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

 private:
  std::unordered_map<std::string, std::vector<uint8_t>> files_;
  std::vector<std::string> names_;
};

// Populates a VirtualFilesystem by decompressing every entry in a
// parsed GgzArchive. Eager (decompresses everything up front) --
// correctness-first per Design Principle 4; revisit lazily-decompressing
// on first access if a real game's GGZ archive makes that matter.
VirtualFilesystem BuildVirtualFilesystemFromGgz(const GgzArchive& archive);

}  // namespace zeebulator
