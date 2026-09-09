#include "core/brew/virtual_filesystem.h"

#include <utility>

namespace zeebulator {

void VirtualFilesystem::AddFile(std::string name, std::vector<uint8_t> data) {
  if (files_.find(name) == files_.end()) {
    names_.push_back(name);
  }
  files_[std::move(name)] = std::move(data);
}

bool VirtualFilesystem::Exists(const std::string& name) const {
  return Find(name) != nullptr;
}

const std::vector<uint8_t>* VirtualFilesystem::Find(const std::string& name) const {
  auto it = files_.find(name);
  if (it != files_.end()) return &it->second;
  // Path-normalization fallback (2026-09-02): real games build resource paths
  // with printf into a buffer that often carries a leading "./" or "fs:/" and
  // collapses to doubled slashes (e.g. Rolimaz opens ".//pak0.pakz" for a file
  // registered as "pak0.pakz"). Registrations use the archive's own basename,
  // so retry with a canonical form: strip a leading "fs:/" / "./" and collapse
  // repeated '/'. If that still misses, fall back to a basename match (the
  // segment after the last '/'), which resolves any remaining directory prefix
  // a game prepends to a flat resource name.
  auto canon = [](std::string s) {
    // normalize Windows-style backslashes to '/' first (real MAME/arcade ports
    // like the Data East titles open ".\\baddudes.zip" / "roms\\baddudes.zip")
    for (auto& ch : s) if (ch == '\\') ch = '/';
    // strip a leading fs:/ scheme
    if (s.rfind("fs:/", 0) == 0) s.erase(0, 4);
    // collapse "./" segments and doubled slashes
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '/' && !out.empty() && out.back() == '/') continue;  // skip dup '/'
      if (s[i] == '.' && i + 1 < s.size() && s[i + 1] == '/' &&
          (out.empty() || out.back() == '/')) {
        ++i;  // skip "./" (the '/' is consumed by the loop's ++i)
        continue;
      }
      out.push_back(s[i]);
    }
    // strip any leading '/'
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
  };
  std::string c = canon(name);
  it = files_.find(c);
  if (it != files_.end()) return &it->second;
  // basename fallback: match the tail segment against each registered file's
  // own basename (case-insensitive, as BREW filesystem is case-insensitive).
  auto base_of = [](const std::string& s) {
    size_t p = s.find_last_of('/');
    return p == std::string::npos ? s : s.substr(p + 1);
  };
  auto iequals = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return true;
  };
  std::string want = base_of(c);
  if (!want.empty()) {
    // Exact match first
    for (const auto& n : names_) {
      if (base_of(n) == want) {
        auto jt = files_.find(n);
        if (jt != files_.end()) return &jt->second;
      }
    }
    // Case-insensitive match across registered names
    for (const auto& n : names_) {
      if (iequals(base_of(n), want) || iequals(n, c)) {
        auto jt = files_.find(n);
        if (jt != files_.end()) return &jt->second;
      }
    }
  }
  // Lazy loose-asset fallback (2026-09-08): everything registered has missed.
  // Ask the harness-installed resolver to fetch this loose sibling from the
  // host FS ON DEMAND. Try the canonical relative path first (`c`, e.g.
  // "zeeboiddata/version.txt" -- some titles, like Zeeboids, ship a per-game
  // asset subfolder next to the .mod and open files inside it by that
  // relative path), then fall back to a bare basename match (`want`) for the
  // flat-namespace case the resolver originally targeted. On a hit, memoize
  // under the resolved key so repeat opens resolve directly. Only names the
  // guest actually requests get registered -- unlike the old eager pass, this
  // never pollutes the namespace for titles that don't ask for the file.
  if (miss_resolver_) {
    std::vector<uint8_t> loaded;
    if (!c.empty() && miss_resolver_(c, loaded)) {
      if (files_.find(c) == files_.end()) names_.push_back(c);
      auto& slot = files_[c];
      slot = std::move(loaded);
      return &slot;
    }
    if (!want.empty() && miss_resolver_(want, loaded)) {
      if (files_.find(want) == files_.end()) names_.push_back(want);
      auto& slot = files_[want];
      slot = std::move(loaded);
      return &slot;
    }
  }
  return nullptr;
}

VirtualFilesystem BuildVirtualFilesystemFromGgz(const GgzArchive& archive) {
  VirtualFilesystem vfs;
  for (const auto& entry : archive.Entries()) {
    vfs.AddFile(entry.name, archive.Extract(entry));
  }
  return vfs;
}

}  // namespace zeebulator
