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
  return files_.find(name) != files_.end();
}

const std::vector<uint8_t>* VirtualFilesystem::Find(const std::string& name) const {
  auto it = files_.find(name);
  return it == files_.end() ? nullptr : &it->second;
}

VirtualFilesystem BuildVirtualFilesystemFromGgz(const GgzArchive& archive) {
  VirtualFilesystem vfs;
  for (const auto& entry : archive.Entries()) {
    vfs.AddFile(entry.name, archive.Extract(entry));
  }
  return vfs;
}

}  // namespace zeebulator
