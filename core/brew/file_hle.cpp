#include "core/brew/file_hle.h"

#include <algorithm>

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }
void StubFailed(IArmCore& core) { core.SetRegister(kR0, 1); }  // AEE_EFAILED-ish

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace

FileHle::FileHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs,
                  uint32_t file_object_region_start)
    : memory_(memory), hle_(hle), vfs_(vfs), next_object_address_(file_object_region_start) {}

void FileHle::WriteFileInfo(uint32_t dest_addr, const std::string& name, uint32_t size) {
  // Matches AEEFileInfo: { char attrib; uint32 dwCreationDate;
  // uint32 dwSize; char szName[64]; } with standard ARM struct
  // alignment (3 bytes padding after the 1-byte attrib field).
  memory_.Write8(dest_addr, 0);       // attrib = _FA_NORMAL
  memory_.Write32(dest_addr + 4, 0);  // dwCreationDate: unknown, not tracked
  memory_.Write32(dest_addr + 8, size);
  size_t n = std::min(name.size(), static_cast<size_t>(63));
  for (size_t i = 0; i < n; ++i) {
    memory_.Write8(dest_addr + 12 + static_cast<uint32_t>(i),
                    static_cast<uint8_t>(name[i]));
  }
  memory_.Write8(dest_addr + 12 + static_cast<uint32_t>(n), 0);
}

uint32_t FileHle::AllocateFileObject(const std::string& name, const std::vector<uint8_t>* data,
                                      std::vector<uint8_t>* mutable_data) {
  uint32_t obj_addr = next_object_address_;
  next_object_address_ += 4;
  memory_.Write32(obj_addr, file_vtable_address_);
  open_files_[obj_addr] = OpenFile{name, data, mutable_data, 0};
  return obj_addr;
}

void FileHle::OpenFileImpl(IArmCore& core) {
  // IFile* OpenFile(IFileMgr* piname, const char* pszFile, OpenFileMode mode)
  // Real OpenFileMode bits (confirmed against AEEFile.h): _OFM_READ=1,
  // _OFM_READWRITE=2, _OFM_CREATE=4, _OFM_APPEND=8.
  constexpr uint32_t kOfmCreate = 0x0004;
  std::string name = ReadCString(memory_, core.GetRegister(kR1));
  uint32_t mode = core.GetRegister(kR2);

  uint32_t handle = 0;
  auto writable_it = writable_files_.find(name);
  if (writable_it != writable_files_.end()) {
    handle = AllocateFileObject(name, &writable_it->second, &writable_it->second);
  } else if (const std::vector<uint8_t>* data = vfs_.Find(name)) {
    handle = AllocateFileObject(name, data);
  } else if ((mode & kOfmCreate) != 0) {
    auto [inserted, _] = writable_files_.emplace(name, std::vector<uint8_t>{});
    handle = AllocateFileObject(name, &inserted->second, &inserted->second);
  }
  if (handle != 0) last_opened_handle_ = handle;
  core.SetRegister(kR0, handle);
}

void FileHle::FileMgrGetInfoImpl(IArmCore& core) {
  // int GetInfo(IFileMgr* piname, const char* pszName, FileInfo* pInfo)
  std::string name = ReadCString(memory_, core.GetRegister(kR1));
  auto writable_it = writable_files_.find(name);
  if (writable_it != writable_files_.end()) {
    WriteFileInfo(core.GetRegister(kR2), name, static_cast<uint32_t>(writable_it->second.size()));
    core.SetRegister(kR0, 0);
    return;
  }
  const std::vector<uint8_t>* data = vfs_.Find(name);
  if (!data) {
    core.SetRegister(kR0, 1);
    return;
  }
  WriteFileInfo(core.GetRegister(kR2), name, static_cast<uint32_t>(data->size()));
  core.SetRegister(kR0, 0);
}

void FileHle::TestImpl(IArmCore& core) {
  // int Test(IFileMgr* piname, const char* pszName)
  std::string name = ReadCString(memory_, core.GetRegister(kR1));
  bool exists = vfs_.Exists(name) || writable_files_.count(name) != 0;
  core.SetRegister(kR0, exists ? 0u : 1u);
}

void FileHle::GetFreeSpaceImpl(IArmCore& core) {
  // uint32 GetFreeSpace(IFileMgr* piname, uint32* pdwTotal) -- returns
  // free bytes directly, optionally also writing total capacity.
  // Reports a plausible simulated user-data quota (1 MiB); not a
  // measured real device value -- confirmed real disassembly
  // (PHASE8_LOG.md) shows Double Dragon's save routine treating 0
  // (the previous blind-Stub behavior) as "storage unusable" and
  // aborting, so this must be a believable nonzero amount, not just
  // "not zero".
  constexpr uint32_t kSimulatedFreeSpaceBytes = 1024 * 1024;
  uint32_t total_addr = core.GetRegister(kR1);
  if (total_addr != 0) {
    memory_.Write32(total_addr, kSimulatedFreeSpaceBytes);
  }
  core.SetRegister(kR0, kSimulatedFreeSpaceBytes);
}

void FileHle::EnumInitImpl(IArmCore& core) {
  // int EnumInit(IFileMgr* piname, const char* pszDir, boolean bDirs)
  // pszDir/bDirs ignored -- the VFS is flat, so there's only ever one
  // "directory" to enumerate.
  enum_cursor_ = 0;
  core.SetRegister(kR0, 0);
}

void FileHle::EnumNextImpl(IArmCore& core) {
  // boolean EnumNext(IFileMgr* piname, FileInfo* pInfo)
  const auto& names = vfs_.Names();
  if (enum_cursor_ >= names.size()) {
    core.SetRegister(kR0, 0);  // FALSE: no more entries
    return;
  }
  const std::string& name = names[enum_cursor_++];
  const std::vector<uint8_t>* data = vfs_.Find(name);
  WriteFileInfo(core.GetRegister(kR1), name, static_cast<uint32_t>(data->size()));
  core.SetRegister(kR0, 1);  // TRUE
}

void FileHle::ReadFromHandle(IArmCore& core, uint32_t handle) {
  auto it = open_files_.find(handle);
  if (it == open_files_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }
  OpenFile& f = it->second;
  uint32_t dest = core.GetRegister(kR1);
  uint32_t want = core.GetRegister(kR2);
  uint32_t remaining = static_cast<uint32_t>(f.data->size()) - f.position;
  uint32_t n = std::min(want, remaining);
  for (uint32_t i = 0; i < n; ++i) {
    memory_.Write8(dest + i, (*f.data)[f.position + i]);
  }
  f.position += n;
  core.SetRegister(kR0, n);
}

void FileHle::ReadImpl(IArmCore& core) {
  // int32 Read(IFile* po, void* pDest, uint32 nWant)
  ReadFromHandle(core, core.GetRegister(kR0));
}

void FileHle::WriteImpl(IArmCore& core) {
  // int32 Write(IFile* po, const void* pSrc, uint32 nWant)
  auto it = open_files_.find(core.GetRegister(kR0));
  if (it == open_files_.end() || it->second.mutable_data == nullptr) {
    core.SetRegister(kR0, static_cast<uint32_t>(-1));  // EFAILED-ish: read-only or unknown handle
    return;
  }
  OpenFile& f = it->second;
  uint32_t src = core.GetRegister(kR1);
  uint32_t want = core.GetRegister(kR2);
  if (f.position + want > f.mutable_data->size()) {
    f.mutable_data->resize(f.position + want);
  }
  for (uint32_t i = 0; i < want; ++i) {
    (*f.mutable_data)[f.position + i] = memory_.Read8(src + i);
  }
  f.position += want;
  dirty_ = true;
  core.SetRegister(kR0, want);
}

void FileHle::FileGetInfoImpl(IArmCore& core) {
  // int GetInfo(IFile* pIFile, FileInfo* pInfo)
  auto it = open_files_.find(core.GetRegister(kR0));
  if (it == open_files_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  const OpenFile& f = it->second;
  WriteFileInfo(core.GetRegister(kR1), f.name, static_cast<uint32_t>(f.data->size()));
  core.SetRegister(kR0, 0);
}

void FileHle::SeekImpl(IArmCore& core) {
  // int32 Seek(IFile* pIFile, FileSeekType seek, int32 moveDistance)
  // FileSeekType: _SEEK_START=0, _SEEK_END=1, _SEEK_CURRENT=2 (confirmed
  // against the real AEEFile.h enum). Real return value (also confirmed
  // against AEEFile.h's own documented contract -- NOT the resulting
  // position, a wrong assumption this class shipped with until real
  // disassembly of Double Dragon caught it, see PHASE8_LOG.md): plain
  // AEE_SUCCESS(0)/AEE_EFAILED(1), except the specific special case of
  // _SEEK_CURRENT with moveDistance==0 ("tell"), which returns the
  // current position. Real semantics: a read-only file fails the seek
  // if the target is outside [0, size]; a writable file only fails if
  // the target is negative, and seeking past EOF extends the file.
  constexpr uint32_t kSeekStart = 0;
  constexpr uint32_t kSeekCurrent = 2;
  constexpr uint32_t kAeeSuccess = 0;
  constexpr uint32_t kAeeEfailed = 1;
  auto it = open_files_.find(core.GetRegister(kR0));
  if (it == open_files_.end()) {
    core.SetRegister(kR0, kAeeEfailed);
    return;
  }
  OpenFile& f = it->second;
  uint32_t seek_type = core.GetRegister(kR1);
  auto move_distance = static_cast<int32_t>(core.GetRegister(kR2));

  if (seek_type == kSeekCurrent && move_distance == 0) {
    core.SetRegister(kR0, f.position);  // tell operation
    return;
  }

  int64_t base;
  switch (seek_type) {
    case kSeekStart: base = 0; break;
    default: base = static_cast<int64_t>(f.position); break;  // _SEEK_CURRENT
    case 1: base = static_cast<int64_t>(f.data->size()); break;  // _SEEK_END
  }
  int64_t new_pos = base + move_distance;

  if (f.mutable_data != nullptr) {
    if (new_pos < 0) {
      core.SetRegister(kR0, kAeeEfailed);
      return;
    }
    if (new_pos > static_cast<int64_t>(f.mutable_data->size())) {
      f.mutable_data->resize(static_cast<size_t>(new_pos));
    }
  } else if (new_pos < 0 || new_pos > static_cast<int64_t>(f.data->size())) {
    core.SetRegister(kR0, kAeeEfailed);
    return;
  }
  f.position = static_cast<uint32_t>(new_pos);
  core.SetRegister(kR0, kAeeSuccess);
}

uint32_t FileHle::Build(uint32_t file_mgr_vtable_address, uint32_t file_mgr_object_address,
                         uint32_t file_vtable_address) {
  file_vtable_address_ = file_vtable_address;

  // Shared IFile vtable: every OpenFile call creates a fresh object
  // header pointing at this SAME vtable; only the header address
  // differs per open file, since ReadImpl/SeekImpl/FileGetInfoImpl look
  // up per-file state by "po" (R0) at dispatch time, not by vtable
  // identity.
  std::vector<HleRuntime::HleFunction> file_methods = {
      Stub,                                          // 0  AddRef
      Stub,                                          // 1  Release
      Stub,                                          // 2  Readable
      [this](IArmCore& c) { ReadImpl(c); },           // 3  Read
      Stub,                                          // 4  Cancel
      [this](IArmCore& c) { WriteImpl(c); },          // 5  Write
      [this](IArmCore& c) { FileGetInfoImpl(c); },    // 6  GetInfo
      [this](IArmCore& c) { SeekImpl(c); },           // 7  Seek
      StubFailed,                                    // 8  Truncate (not implemented)
  };
  for (size_t i = 0; i < file_methods.size(); ++i) {
    uint32_t sentinel = hle_.Register(file_methods[i]);
    memory_.Write32(file_vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }

  std::vector<HleRuntime::HleFunction> mgr_methods = {
      Stub,                                            // 0  AddRef
      Stub,                                            // 1  Release
      [this](IArmCore& c) { OpenFileImpl(c); },         // 2  OpenFile
      [this](IArmCore& c) { FileMgrGetInfoImpl(c); },   // 3  GetInfo
      StubFailed,                                      // 4  Remove (read-only)
      StubFailed,                                      // 5  MkDir (read-only)
      StubFailed,                                      // 6  RmDir (read-only)
      [this](IArmCore& c) { TestImpl(c); },             // 7  Test
      [this](IArmCore& c) { GetFreeSpaceImpl(c); },     // 8  GetFreeSpace
      Stub,                                            // 9  GetLastError (unused)
      [this](IArmCore& c) { EnumInitImpl(c); },         // 10 EnumInit
      [this](IArmCore& c) { EnumNextImpl(c); },         // 11 EnumNext
      StubFailed,                                      // 12 Rename (read-only)
  };
  return BuildInterfaceObject(memory_, hle_, file_mgr_vtable_address,
                               file_mgr_object_address, mgr_methods);
}

namespace {

bool WriteU32(std::ostream& out, uint32_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return out.good();
}

bool ReadU32(std::istream& in, uint32_t& v) {
  in.read(reinterpret_cast<char*>(&v), sizeof(v));
  return in.good();
}

}  // namespace

bool FileHle::Serialize(std::ostream& out) const {
  if (!WriteU32(out, static_cast<uint32_t>(writable_files_.size()))) return false;
  for (const auto& [name, data] : writable_files_) {
    if (!WriteU32(out, static_cast<uint32_t>(name.size()))) return false;
    out.write(name.data(), static_cast<std::streamsize>(name.size()));
    if (!out.good()) return false;
    if (!WriteU32(out, static_cast<uint32_t>(data.size()))) return false;
    if (!data.empty()) {
      out.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
      if (!out.good()) return false;
    }
  }
  dirty_ = false;
  return true;
}

bool FileHle::Deserialize(std::istream& in) {
  uint32_t count = 0;
  if (!ReadU32(in, count)) return false;

  std::unordered_map<std::string, std::vector<uint8_t>> loaded;
  loaded.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t name_len = 0;
    if (!ReadU32(in, name_len)) return false;
    std::string name(name_len, '\0');
    if (name_len != 0) {
      in.read(name.data(), name_len);
      if (!in.good()) return false;
    }
    uint32_t data_len = 0;
    if (!ReadU32(in, data_len)) return false;
    std::vector<uint8_t> data(data_len);
    if (data_len != 0) {
      in.read(reinterpret_cast<char*>(data.data()), data_len);
      if (!in.good()) return false;
    }
    loaded.emplace(std::move(name), std::move(data));
  }

  // Open files reference writable_files_ entries by address
  // (mutable_data), so swapping the whole map out from under any
  // currently-open handle would leave a dangling pointer -- not a real
  // concern in practice (this is only ever called once, before the
  // guest has opened anything), but guard it explicitly rather than
  // silently risk it.
  if (!open_files_.empty()) return false;
  writable_files_ = std::move(loaded);
  dirty_ = false;
  return true;
}

uint32_t FileHle::BuildLastOpenedFileProxy(uint32_t vtable_address, uint32_t object_address) {
  // Sized to cover every real IFile slot (see file_methods in Build()),
  // even though only Read (slot 3) is expected to be called on this
  // one -- see the class doc comment for why.
  std::vector<HleRuntime::HleFunction> methods(9, Stub);
  methods[3] = [this](IArmCore& core) { ReadFromHandle(core, last_opened_handle_); };
  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
