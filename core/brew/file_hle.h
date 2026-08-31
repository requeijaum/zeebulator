#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/brew/virtual_filesystem.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IFile/IFileMgr HLE backed by a VirtualFilesystem (itself backed by a
// loaded GGZ archive's contents). Vtable slot orders verified directly
// against real Qualcomm AEEFile.h -- see TASKS.md Phase 4:
//   IFile inherits IAStream: AddRef, Release, Readable, Read, Cancel,
//   then Write, GetInfo, Seek, Truncate.
//   IFileMgr: AddRef, Release, OpenFile, GetInfo, Remove, MkDir, RmDir,
//   Test, GetFreeSpace, GetLastError, EnumInit, EnumNext, Rename.
//
// The GGZ-backed content is read-only (Remove/MkDir/RmDir/Rename all
// return an error rather than silently succeeding), but OpenFile
// supports real save-game style read/write/create against a separate,
// in-memory "user data" store (see writable_files_) -- confirmed real
// disassembly of Double Dragon's own save/load routine (`ddragonz.mod`
// offset `0x9f3c`, called from `0x237c4`, see PHASE8_LOG.md) does
// exactly `IFILEMGR_Test("./udata/ddz.sav")`, then on failure
// `IFILEMGR_GetFreeSpace()` (checked against a minimum), then
// `IFILEMGR_OpenFile(..., _OFM_CREATE)` -- a real "load save, or create
// a fresh one" pattern this class needs to actually satisfy, not just
// avoid crashing on. See Serialize/Deserialize for real persistence of
// that in-memory store past this process's own lifetime -- without it,
// every cold relaunch starts from a genuinely empty store, which is
// exactly the real, live-reported "the game doesn't save" bug this was
// chasing (confirmed: Double Dragon's own save routine faithfully
// wrote its unlock/progress flags into `writable_files_` every time,
// they just never survived process exit). The filesystem is flat (GGZ
// has no directory
// structure), so EnumInit ignores its directory argument and always
// enumerates everything (only the read-only GGZ-backed files, not
// writable ones -- nothing exercised so far enumerates a save
// directory).
//
// IFile and IFileMgr are implemented together here (not one-file-per-
// interface like IShell/IDisplay) because they're a tightly coupled
// pair in practice: IFileMgr::OpenFile is what creates IFile instances,
// and both need access to the same open-file state.
class FileHle {
 public:
  // `file_object_region_start` is a bump-allocated address range this
  // class owns for newly-opened IFile object headers (4 bytes each) --
  // must not overlap any other memory region the caller is using.
  FileHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs,
          uint32_t file_object_region_start);

  // Builds the shared IFile vtable (used by every opened file) and the
  // IFileMgr object. Returns the IFileMgr* value the app should receive.
  uint32_t Build(uint32_t file_mgr_vtable_address, uint32_t file_mgr_object_address,
                 uint32_t file_vtable_address);

  // Builds a proxy IFile-shaped object whose Read (slot 3) always
  // forwards to whichever file this OpenFileImpl most recently
  // returned, re-resolved on every call rather than fixed at build
  // time. Real disassembly of Double Dragon (PHASE8_LOG.md) shows a
  // second, separate real BREW class (0x01001014, created once via
  // ISHELL_CreateInstance alongside AEECLSID_FILEMGR and never
  // reassigned anywhere in the traced code -- confirmed with a live
  // memory watchpoint across a full run) whose own Read is called
  // immediately after the game's own resource-loading routine opens
  // and seeks the real file it wants via a completely different
  // object (IFileMgr). No real header or sample in this repo's bundled
  // materials names class 0x01001014 (see PHASE8_LOG.md for what was
  // searched and ruled out), so this is a deliberate, evidence-
  // grounded educated implementation -- not a confirmed-correct one --
  // of the one behavior every piece of real evidence points at: acting
  // as "whatever file is current" without ever being told so
  // explicitly through its own storage.
  uint32_t BuildLastOpenedFileProxy(uint32_t vtable_address, uint32_t object_address);

  // Persists `writable_files_` -- real save-game data (see class doc
  // comment) -- so it survives past this process, not just this class's
  // own in-memory lifetime. Confirmed the real, live-reproduced bug this
  // fixes: without ever writing this back to a real host file, every
  // cold relaunch starts from a genuinely empty writable_files_, so
  // real save-game progress (Double Dragon's own "./udata/ddz.sav")
  // silently vanishes the moment the process exits -- indistinguishable
  // from the game "not saving" at all, which is exactly the real,
  // reported symptom this was chasing. Deliberately a separate
  // mechanism from core/save_state.h's own CPU/memory snapshot (which
  // captures a single live moment to resume from) -- this instead
  // mirrors what a real device's own persistent flash storage does:
  // survives across every cold launch, save-state loads and all,
  // regardless of which moment a save state itself was taken at.
  //
  // Format: uint32 LE entry count, then per entry: uint32 LE name
  // length, the name's own raw bytes, uint32 LE data length, the data's
  // own raw bytes.
  bool Serialize(std::ostream& out) const;
  // Returns false (leaving `writable_files_` untouched) on a malformed
  // stream -- matches core/save_state.h's own "don't half-apply a
  // corrupt load" convention. An absent/empty file (a first-ever launch,
  // no save yet) is the caller's own concern, not this function's --
  // see tools/game_probe.cpp for how a missing file is tolerated there.
  bool Deserialize(std::istream& in);

  // True once `writable_files_` has changed since the last successful
  // Serialize() call (or construction, if never serialized) -- lets a
  // caller persist real save-game writes to a real host file only when
  // there's actually something new, rather than rewriting an unchanged
  // file every single tick regardless.
  bool HasUnsavedWrites() const { return dirty_; }

 private:
  struct OpenFile {
    std::string name;
    const std::vector<uint8_t>* data;  // current bytes, for Read/Seek/GetInfo/size
    std::vector<uint8_t>* mutable_data = nullptr;  // non-null only for writable_files_ entries
    uint32_t position = 0;
  };

  // IFileMgr methods.
  void OpenFileImpl(IArmCore& core);
  void FileMgrGetInfoImpl(IArmCore& core);
  void TestImpl(IArmCore& core);
  void GetFreeSpaceImpl(IArmCore& core);
  void EnumInitImpl(IArmCore& core);
  void EnumNextImpl(IArmCore& core);

  // IFile methods (shared vtable; instance looked up by "po", i.e. R0).
  void ReadImpl(IArmCore& core);
  void WriteImpl(IArmCore& core);
  void FileGetInfoImpl(IArmCore& core);
  void SeekImpl(IArmCore& core);

  // Shared by ReadImpl (handle = R0, the real "po") and the last-
  // opened-file proxy's Read (handle = last_opened_handle_, ignoring
  // its own "po" entirely).
  void ReadFromHandle(IArmCore& core, uint32_t handle);

  void WriteFileInfo(uint32_t dest_addr, const std::string& name, uint32_t size);
  uint32_t AllocateFileObject(const std::string& name, const std::vector<uint8_t>* data,
                               std::vector<uint8_t>* mutable_data = nullptr);

  Memory& memory_;
  HleRuntime& hle_;
  const VirtualFilesystem& vfs_;
  uint32_t file_vtable_address_ = 0;
  uint32_t next_object_address_;
  size_t enum_cursor_ = 0;
  std::unordered_map<uint32_t, OpenFile> open_files_;
  // Runtime-created/written files (e.g. save games) -- separate from
  // the read-only GGZ-backed vfs_. Optionally persisted past this
  // process via Serialize/Deserialize -- see their own doc comments.
  std::unordered_map<std::string, std::vector<uint8_t>> writable_files_;
  // The handle OpenFileImpl most recently returned successfully (0 if
  // none yet) -- see BuildLastOpenedFileProxy.
  uint32_t last_opened_handle_ = 0;
  // See HasUnsavedWrites()'s own doc comment. mutable: Serialize() is
  // logically read-only (writable_files_ itself never changes), but
  // still needs to clear this once it's successfully persisted the
  // current state.
  mutable bool dirty_ = false;
};

}  // namespace zeebulator
