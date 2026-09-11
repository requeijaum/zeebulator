#include "core/brew/ishell.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include "core/brew/interface_object.h"
#include "core/brew/thread_hle.h"
#include "core/brew/nid_table.h"
#include "core/brew/stub_trace.h"

namespace zeebulator {

namespace {

// Generic stub: sets a zero/failure-ish return value and does nothing
// else. Safe default for any BREW method call whose behavior isn't
// implemented yet -- real games that hit one of these will need it
// filled in with real behavior at that point.
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace

IShellHle::IShellHle(Memory& memory, HleRuntime& hle, int screen_width, int screen_height)
    : memory_(memory), hle_(hle), screen_width_(screen_width), screen_height_(screen_height) {}

void IShellHle::RegisterInstance(uint32_t cls_id, uint32_t object_ptr) {
  instances_[cls_id] = object_ptr;
}

void IShellHle::RegisterFactory(uint32_t cls_id, std::function<uint32_t()> factory) {
  factories_[cls_id] = std::move(factory);
}

void IShellHle::RegisterResourceFile(const std::string& name, std::vector<uint8_t> data) {
  resource_files_.emplace(name, BarArchive::Parse(std::move(data)));
}

void IShellHle::CreateInstanceImpl(IArmCore& core) {
  // int CreateInstance(IShell *po, AEECLSID cls, void **ppo)
  uint32_t cls_id = core.GetRegister(kR1);
  uint32_t ppobj = core.GetRegister(kR2);
  // Phase 9c unknown-interface logger (env-gated, non-perturbing: stderr only,
  // no new HLE state / no traps registered). Set ZEEB_LOG_CREATEINSTANCE=1 to
  // surface every clsid a title requests and whether it was satisfied.
  const bool log_ci = std::getenv("ZEEB_LOG_CREATEINSTANCE") != nullptr;
  auto factory_it = factories_.find(cls_id);
  if (factory_it != factories_.end()) {
    if (log_ci) std::fprintf(stderr, "[createinstance] cls=%s -> factory OK\n", DescribeClsid(cls_id).c_str());
    memory_.Write32(ppobj, factory_it->second());
    core.SetRegister(kR0, 0);  // SUCCESS
    return;
  }
  auto it = instances_.find(cls_id);
  if (it == instances_.end()) {
    if (log_ci) std::fprintf(stderr, "[createinstance] cls=%s -> UNKNOWN (ECLASSNOTSUPPORT)\n", DescribeClsid(cls_id).c_str());
    if (ppobj != 0) memory_.Write32(ppobj, 0);
    core.SetRegister(kR0, 20);  // ECLASSNOTSUPPORT (Qualcomm standard: 20)
    return;
  }
  if (log_ci) std::fprintf(stderr, "[createinstance] cls=%s -> instance OK\n", DescribeClsid(cls_id).c_str());
  memory_.Write32(ppobj, it->second);
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::GetDeviceInfoImpl(IArmCore& core) {
  // void GetDeviceInfo(IShell *po, AEEDeviceInfo *pdi)
  // po is R0 (unused, matches "this"), pdi R1.
  // Full AEEDeviceInfo mapping based on official BREW SDK and zeebx (machine.rs:2690):
  // Offset 0..15: 8 uint16 fields (cxScreen, cyScreen, cxAltScreen, cyAltScreen, cxScrollBar, wEncoding, wMenuTextScroll, wColorDepth)
  // Offset 24: dwRAM (64MB)
  // If requested wStructSize >= 64, fill extended fields (dwNetLinger, dwSleepDefer, wMaxPath, dwPlatformID)
  uint32_t pdi = core.GetRegister(kR1);
  if (pdi != 0) {
    uint16_t requested = memory_.Read16(pdi + 44);

    // Zero up to dwLang (44 bytes)
    for (uint32_t off = 0; off < 44; off += 4) {
      memory_.Write32(pdi + off, 0);
    }

    memory_.Write16(pdi + 0, static_cast<uint16_t>(screen_width_));   // cxScreen
    memory_.Write16(pdi + 2, static_cast<uint16_t>(screen_height_));  // cyScreen
    memory_.Write16(pdi + 4, 0);                                       // cxAltScreen
    memory_.Write16(pdi + 6, 0);                                       // cyAltScreen
    memory_.Write16(pdi + 8, 8);                                       // cxScrollBar
    memory_.Write16(pdi + 10, 3);                                      // wEncoding (AEE_ENC_ISOLATIN1)
    memory_.Write16(pdi + 12, 0);                                      // wMenuTextScroll
    memory_.Write16(pdi + 14, 16);                                     // wColorDepth (16-bit 565)

    memory_.Write32(pdi + 24, 64 * 1024 * 1024);                      // dwRAM (64MB heap)

    if (requested >= 64) {
      memory_.Write16(pdi + 44, 64);  // wStructSize
      memory_.Write32(pdi + 48, 0);   // dwNetLinger
      memory_.Write32(pdi + 52, 0);   // dwSleepDefer
      memory_.Write16(pdi + 56, 64);  // wMaxPath (AEE_MAX_FILE_NAME)
      memory_.Write32(pdi + 60, 0);   // dwPlatformID
    }
  }
  core.SetRegister(kR0, 0);
}

void IShellHle::ScheduleTimer(uint32_t ms, uint32_t callback, uint32_t user_data,
                               std::optional<uint32_t> r0_override) {
  // Re-registering the same (callback, user_data) pair reschedules it
  // rather than creating a duplicate -- matches the real self-rearming
  // timer pattern (see class doc comment) where the callback calls
  // SetTimer again with the same identity every time it fires.
  for (auto& timer : timers_) {
    if (timer.callback == callback && timer.user_data == user_data) {
      timer.remaining_ms = ms;
      timer.r0_override = r0_override;
      return;
    }
  }
  timers_.push_back(PendingTimer{ms, callback, user_data, r0_override});
  if (std::getenv("ZEEB_LOG_TIMER")) {
    std::fprintf(stderr, "[timer] schedule ms=%u cb=0x%08x data=0x%08x (total=%zu)\n",
                 ms, callback, user_data, timers_.size());
  }
}

void IShellHle::SetTimerImpl(IArmCore& core) {
  // int SetTimer(IShell *ps, uint32 dwCount, PFNNOTIFY pfnNotify, void *pUser)
  uint32_t ms = core.GetRegister(kR1);
  uint32_t callback = core.GetRegister(kR2);
  uint32_t user_data = core.GetRegister(kR3);

  // If callback == user_data, this is an AEECallback* struct:
  // pfnNotify is at offset 16 (+0x10), pNotifyData is at offset 20 (+0x14).
  if (callback != 0 && callback == user_data) {
    uint32_t pfn = memory_.Read32(callback + 16);
    uint32_t data = memory_.Read32(callback + 20);
    ScheduleTimer(ms, pfn, data);
  } else {
    ScheduleTimer(ms, callback, user_data);
  }
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::CancelTimerImpl(IArmCore& core) {
  // int CancelTimer(IShell *ps, PFNNOTIFY pfnNotify, void *pUser)
  uint32_t callback = core.GetRegister(kR1);
  uint32_t user_data = core.GetRegister(kR2);
  if (callback != 0 && callback == user_data) {
    uint32_t pfn = memory_.Read32(callback + 16);
    uint32_t data = memory_.Read32(callback + 20);
    callback = pfn;
    user_data = data;
  }
  // BREW SDK specification (and zeebx machine.rs:2182):
  // When pfnNotify is null, cancel ALL timers associated with this context (user_data).
  bool erased = false;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (it->user_data == user_data && (callback == 0 || it->callback == callback)) {
      it = timers_.erase(it);
      erased = true;
    } else {
      ++it;
    }
  }
  core.SetRegister(kR0, erased ? 0 : 1);
}

void IShellHle::SendEventImpl(IArmCore& core) {
  // int ISHELL_SendEvent(IShell *po, AEECLSID cls, AEEEvent evt, uint16 wParam, uint32 dwParam)
  uint32_t a1 = core.GetRegister(kR1);
  uint32_t a2 = core.GetRegister(kR2);
  uint32_t a3 = core.GetRegister(kR3);
  uint32_t sp = core.GetRegister(kSP);
  uint32_t s0 = memory_.Read32(sp);
  uint32_t s1 = memory_.Read32(sp + 4);

  uint32_t cls = a1;
  uint32_t evt = a2;
  uint16_t w = static_cast<uint16_t>(a3);
  uint32_t dw = s0;
  if (a2 == applet_clsid_ || a2 == 0x01070798) {
    cls = a2;
    evt = a3;
    w = static_cast<uint16_t>(s0);
    dw = s1;
  }
  // Evento 0x7b0a da Z-Wheel
  if (evt == 0x7b0a) {
    if (w == 4) {  // PrefsDB
      if (dw != 0 && applet_ptr_ != 0) {
        memory_.Write32(dw, applet_ptr_ + 0x2ef8);
      }
      core.SetRegister(kR0, 1);  // TRUE = tratado
      return;
    }
    if (w == 1 || w == 0xa) {  // Lang
      if (dw != 0) {
        memory_.Write32(dw, 538997872);  // "pt  "
      }
      core.SetRegister(kR0, 1);  // TRUE = tratado
      return;
    }
  }
  core.SetRegister(kR0, 0);  // 0 = nao tratado
}

void IShellHle::ResumeImpl(IArmCore& core) {
  // int ISHELL_Resume(IShell *ps, AEECallback *pCallback)
  // Invokes or queues callback immediately (ms=0), or schedules a cooperative thread.
  uint32_t pcb = core.GetRegister(kR1);
  if (pcb != 0) {
    if (thread_hle_ != nullptr && thread_hle_->IsResumeCallback(pcb)) {
      thread_hle_->ResumeByCallback(pcb);
      core.SetRegister(kR0, 0);  // SUCCESS
      return;
    }
    // AEECallback layout: pfnNotify at +16, pNotifyData at +20 (or +4/+8 depending on struct variant)
    // If it's a direct callback function pointer:
    uint32_t pfn = memory_.Read32(pcb + 16);
    uint32_t data = memory_.Read32(pcb + 20);
    if (pfn != 0) {
      ScheduleTimer(0, pfn, data);
    } else {
      // Direct notification callback or alternative layout
      ScheduleTimer(0, pcb, 0);
    }
  }
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::SetLoadResObjectReturn(uint32_t object_ptr) {
  load_res_object_obj_ = object_ptr;
}

void IShellHle::LoadResObjectImpl(IArmCore& core) {
  // ISHELL_LoadResObject: only the injected return-object is honored by
  // default (no real resource decode here). Quake's EVT_APP_START calls slot
  // 19 with r1 = a `fs:/...` path string and immediately treats the RETURN
  // value as an object whose vtable[10] it calls; the stub's prior r0=0 made
  // it bx 0. Returning the harness-injected object keeps that dereference
  // valid (slot 10 = safe no-op) while real resource loading stays out of
  // scope for this fix. Any future decode belongs in game_probe.cpp-style
  // OpenFile plumbing, not here.
  core.SetRegister(kR0, load_res_object_obj_);
}

void IShellHle::LoadResDataImpl(IArmCore& core) {
  // void * ISHELL_LoadResData(IShell * po, const char * pszResFile, uint16 nResID, ResType nType)
  // Real calling convention (Qualcomm BREW SDK AEE.h):
  //   r0 = pIShell
  //   r1 = pszResFile
  //   r2 = nResID (uint16)
  //   r3 = nType (ResType)
  // Returns pointer to allocated resource buffer, or NULL on error.
  std::string filename = ReadCString(memory_, core.GetRegister(kR1));
  uint32_t id = core.GetRegister(kR2);
  uint32_t type = core.GetRegister(kR3);

  auto file_it = resource_files_.find(filename);
  if (file_it == resource_files_.end()) {
    if (std::getenv("ZEEB_LOG_FILE")) {
      std::fprintf(stderr, "[res] LoadResData('%s', id=0x%x, type=0x%x) -> NO FILE REGISTERED\n",
                   filename.c_str(), id, type);
    }
    core.SetRegister(kR0, 0);
    return;
  }
  const BarEntry* entry =
      file_it->second.Find(static_cast<uint16_t>(type), static_cast<uint16_t>(id));
  if (entry == nullptr) {
    if (std::getenv("ZEEB_LOG_FILE")) {
      std::fprintf(stderr, "[res] LoadResData('%s', id=0x%x, type=0x%x) -> NO DIR ENTRY\n",
                   filename.c_str(), id, type);
    }
    core.SetRegister(kR0, 0);
    return;
  }
  std::vector<uint8_t> data = file_it->second.Extract(*entry);
  uint32_t ptr = malloc_fn_ ? malloc_fn_(static_cast<uint32_t>(data.size() + 4)) : 0;
  if (ptr != 0) {
    for (size_t i = 0; i < data.size(); ++i) {
      memory_.Write8(ptr + static_cast<uint32_t>(i), data[i]);
    }
  }
  if (std::getenv("ZEEB_LOG_FILE")) {
    std::fprintf(stderr, "[res] LoadResData('%s', id=0x%x, type=0x%x) -> OK size=%zu ptr=0x%08x\n",
                 filename.c_str(), id, type, data.size(), ptr);
  }
  core.SetRegister(kR0, ptr);
}

void IShellHle::LoadResDataExImpl(IArmCore& core) {
  // AEEResult LoadResDataEx(IShell *pIShell, const char *pszResFile,
  //   uint16 wResID, AEERESTYPE resType, void *pBuffer, uint32 *pnLen)
  // Real calling convention and the real `(void*)-1` "size only" buffer
  // sentinel confirmed against a real Peggle call site -- see this
  // class's own doc comment.
  std::string filename = ReadCString(memory_, core.GetRegister(kR1));
  uint32_t id = core.GetRegister(kR2);
  uint32_t type = core.GetRegister(kR3);
  uint32_t buffer = HleRuntime::ReadStackArg(core, 0);
  uint32_t len_addr = HleRuntime::ReadStackArg(core, 1);

  auto file_it = resource_files_.find(filename);
  if (file_it == resource_files_.end()) {
    if (std::getenv("ZEEB_LOG_FILE")) {
      std::fprintf(stderr, "[res] LoadResDataEx('%s', id=0x%x, type=0x%x) -> NO FILE REGISTERED\n",
                   filename.c_str(), id, type);
    }
    core.SetRegister(kR0, 1);  // EFAILED-ish: unregistered resource file
    return;
  }
  const BarEntry* entry =
      file_it->second.Find(static_cast<uint16_t>(type), static_cast<uint16_t>(id));
  if (entry == nullptr) {
    if (std::getenv("ZEEB_LOG_FILE")) {
      std::fprintf(stderr, "[res] LoadResDataEx('%s', id=0x%x, type=0x%x) -> NO DIR ENTRY\n",
                   filename.c_str(), id, type);
    }
    core.SetRegister(kR0, 1);  // EFAILED-ish: no directory entry for this (type, id)
    return;
  }
  if (std::getenv("ZEEB_LOG_FILE")) {
    std::fprintf(stderr, "[res] LoadResDataEx('%s', id=0x%x, type=0x%x) -> OK size=%u\n",
                 filename.c_str(), id, type, entry->size);
  }

  constexpr uint32_t kSizeOnlySentinel = 0xFFFFFFFF;
  if (buffer == kSizeOnlySentinel) {
    if (len_addr != 0) memory_.Write32(len_addr, entry->size);
    core.SetRegister(kR0, 0);  // SUCCESS
    return;
  }

  std::vector<uint8_t> data = file_it->second.Extract(*entry);
  uint32_t dest_buffer = buffer;
  bool caller_allocated = (dest_buffer != 0);
  if (!caller_allocated) {
    // If pBuffer is NULL, allocate buffer in guest memory and return pointer
    dest_buffer = malloc_fn_ ? malloc_fn_(static_cast<uint32_t>(data.size() + 4)) : 0;
  }

  if (dest_buffer != 0) {
    for (uint32_t i = 0; i < data.size(); ++i) {
      memory_.Write8(dest_buffer + i, data[i]);
    }
  }
  if (len_addr != 0) memory_.Write32(len_addr, entry->size);
  // When caller supplied buffer, return 0 (AEE_SUCCESS); when allocated on demand, return pointer.
  core.SetRegister(kR0, caller_allocated ? 0 : dest_buffer);
}

void IShellHle::GetHandlerImpl(IArmCore& core) {
  // AEECLSID GetHandler(IShell *ps, AEECLSID cls, const char *pszMIME)
  // Maps MIME types to their corresponding Qualcomm BREW handler ClassIDs.
  // Standard MIME registry per Qualcomm AEEClassIDs.h and zeebx machine.rs:233.
  constexpr uint32_t kAeeClsidPng = 0x01004004;
  constexpr uint32_t kAeeClsidWinBmp = 0x01004001;
  constexpr uint32_t kAeeClsidGif = 0x01004003;
  constexpr uint32_t kAeeClsidJpeg = 0x01004005;
  constexpr uint32_t kAeeClsidMediaMidi = 0x01005501;
  constexpr uint32_t kAeeClsidMediaMp3 = 0x01005502;
  constexpr uint32_t kAeeClsidMediaAdpcm = 0x0100550a;
  constexpr uint32_t kAeeClsidMediaPcm = 0x01005511;
  constexpr uint32_t kAudioMediaCls = 0x01005500;

  uint32_t cls = core.GetRegister(kR1);
  uint32_t mime_addr = core.GetRegister(kR2);

  if (mime_addr != 0) {
    std::string mime;
    for (uint32_t i = 0; i < 64; ++i) {
      char c = static_cast<char>(memory_.Read8(mime_addr + i));
      if (c == '\0') break;
      mime.push_back(c);
    }
    if (mime == "image/png") {
      core.SetRegister(kR0, kAeeClsidPng);
      return;
    }
    if (mime == "image/bmp" || mime == "image/x-ms-bmp") {
      core.SetRegister(kR0, kAeeClsidWinBmp);
      return;
    }
    if (mime == "image/jpeg") {
      core.SetRegister(kR0, kAeeClsidJpeg);
      return;
    }
    if (mime == "image/gif") {
      core.SetRegister(kR0, kAeeClsidGif);
      return;
    }
    if (mime == "audio/mid" || mime == "audio/midi") {
      core.SetRegister(kR0, kAeeClsidMediaMidi);
      return;
    }
    if (mime == "audio/mpeg" || mime == "audio/mp3") {
      core.SetRegister(kR0, kAeeClsidMediaMp3);
      return;
    }
    if (mime == "audio/wav" || mime == "audio/x-wav") {
      core.SetRegister(kR0, kAeeClsidMediaPcm);
      return;
    }
    if (mime == "audio/vnd.qcelp") {
      core.SetRegister(kR0, kAeeClsidMediaAdpcm);
      return;
    }
  }

  core.SetRegister(kR0, cls == kAudioMediaCls ? cls : 0);
}

void IShellHle::DetectTypeImpl(IArmCore& core) {
  // int DetectType(IShell *po, const void *cpBuf, uint32 *pdwSize, const char *cpszName, const char **pcpszMIME)
  // Canonical BREW SDK 4.0.2 / zeebx machine.rs:2209.
  uint32_t buf_addr = core.GetRegister(kR1);
  uint32_t size_ptr = core.GetRegister(kR2);
  uint32_t name_ptr = core.GetRegister(kR3);
  uint32_t sp = core.GetRegister(kSP);
  uint32_t mime_ptr = memory_.Read32(sp);

  constexpr uint32_t kDetectTypeBytes = 16;
  constexpr uint32_t kEneedMore = 35;  // ENEEDMORE from AEEError.h
  constexpr uint32_t kEnoType = 34;    // ENOTYPE from AEEError.h

  // Sem dados e sem nome, a pergunta é: "de quantos bytes você precisa?"
  if (buf_addr == 0 && name_ptr == 0) {
    if (size_ptr != 0) {
      memory_.Write32(size_ptr, kDetectTypeBytes);
    }
    core.SetRegister(kR0, kEneedMore);
    return;
  }

  uint32_t available = (size_ptr != 0) ? memory_.Read32(size_ptr) : 0;
  uint32_t read_len = std::min(available, kDetectTypeBytes);
  std::vector<uint8_t> bytes(read_len);
  for (uint32_t i = 0; i < read_len; ++i) {
    bytes[i] = memory_.Read8(buf_addr + i);
  }
  std::string name = (name_ptr != 0) ? ReadCString(memory_, name_ptr) : "";

  auto starts = [&](const uint8_t* magic, size_t len) {
    if (bytes.size() < len) return false;
    return std::memcmp(bytes.data(), magic, len) == 0;
  };

  const char* detected_mime = nullptr;
  const uint8_t png_magic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  const uint8_t jpg_magic[] = {0xff, 0xd8, 0xff};
  const uint8_t midi_magic[] = {'M', 'T', 'h', 'd'};
  const uint8_t id3_magic[] = {'I', 'D', '3'};
  const uint8_t amr_magic[] = {'#', '!', 'A', 'M', 'R'};

  if (starts(png_magic, sizeof(png_magic))) {
    detected_mime = "image/png";
  } else if (starts(jpg_magic, sizeof(jpg_magic))) {
    detected_mime = "image/jpeg";
  } else if (bytes.size() >= 6 && (std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0)) {
    detected_mime = "image/gif";
  } else if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
    detected_mime = "image/bmp";
  } else if (starts(midi_magic, sizeof(midi_magic))) {
    detected_mime = "audio/mid";
  } else if (starts(id3_magic, sizeof(id3_magic)) || (bytes.size() >= 2 && bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0)) {
    detected_mime = "audio/mpeg";
  } else if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
    detected_mime = "audio/wav";
  } else if (starts(amr_magic, sizeof(amr_magic))) {
    detected_mime = "audio/amr";
  } else if (!name.empty()) {
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
      std::string ext = name.substr(dot + 1);
      for (char& c : ext) c = static_cast<char>(std::tolower(c));
      if (ext == "png") detected_mime = "image/png";
      else if (ext == "jpg" || ext == "jpeg") detected_mime = "image/jpeg";
      else if (ext == "gif") detected_mime = "image/gif";
      else if (ext == "bmp") detected_mime = "image/bmp";
      else if (ext == "mid" || ext == "midi") detected_mime = "audio/mid";
      else if (ext == "mp3") detected_mime = "audio/mpeg";
      else if (ext == "wav") detected_mime = "audio/wav";
      else if (ext == "amr") detected_mime = "audio/amr";
      else if (ext == "qcp") detected_mime = "audio/vnd.qcelp";
      else if (ext == "txt") detected_mime = "text/plain";
    }
  }

  if (detected_mime != nullptr) {
    // Escreve string estática/internada na memória guest se mime_ptr for fornecido
    if (mime_ptr != 0) {
      // Aloca buffer permanente para a string MIME se não existir
      auto it = interned_mimes_.find(detected_mime);
      uint32_t str_addr = 0;
      if (it != interned_mimes_.end()) {
        str_addr = it->second;
      } else {
        str_addr = next_mime_addr_;
        size_t slen = std::strlen(detected_mime);
        next_mime_addr_ += static_cast<uint32_t>((slen + 4) & ~3u);
        for (size_t i = 0; i <= slen; ++i) {
          memory_.Write8(str_addr + static_cast<uint32_t>(i), static_cast<uint8_t>(detected_mime[i]));
        }
        interned_mimes_[detected_mime] = str_addr;
      }
      memory_.Write32(mime_ptr, str_addr);
    }
    core.SetRegister(kR0, 0);  // SUCCESS
  } else {
    core.SetRegister(kR0, kEnoType);
  }
}

void IShellHle::GetClassItemIdImpl(IArmCore& core) {
  // uint32 GetClassItemID(IShell *po, AEECLSID cls)
  uint32_t cls = core.GetRegister(kR1);
  if (applet_clsid_ != 0 && cls != applet_clsid_) {
    core.SetRegister(kR0, 0);
    return;
  }
  core.SetRegister(kR0, item_id_);
}

void IShellHle::GetDeviceInfoExImpl(IArmCore& core) {
  // int GetDeviceInfoEx(IShell *po, AEEDeviceItem nItem, void *pBuff, int *pnSize)
  // Per Qualcomm AEEDeviceItems.h and zeebx machine.rs:3510.
  // pnSize is in/out: holds buffer capacity on entry, required size on exit.
  // When pBuff is NULL, the caller is querying the required buffer size.
  constexpr uint32_t kDeviceItemImei = 28;
  constexpr uint32_t kEunsupported = 20;
  constexpr uint32_t kEbadParm = 2;
  constexpr const char* kImei = "350000000000006"; // 15-digit Luhn-valid synthetic IMEI
  constexpr uint32_t kImeiLen = 16; // 15 digits + null terminator

  uint32_t item = core.GetRegister(kR1);
  uint32_t buffer = core.GetRegister(kR2);
  uint32_t size_ptr = core.GetRegister(kR3);

  if (size_ptr == 0) {
    core.SetRegister(kR0, kEbadParm);
    return;
  }

  constexpr uint32_t kDeviceItemChipId = 1;   // AEE_DEVICEITEM_CHIP_ID
  constexpr uint32_t kDeviceItemMobileId = 2; // AEE_DEVICEITEM_MOBILE_ID (IMSI)
  constexpr const char* kChipId = "MSM7201A";
  constexpr uint32_t kChipIdLen = 9;
  constexpr const char* kMobileId = "724050000000001"; // 15-digit Brazilian Claro IMSI
  constexpr uint32_t kMobileIdLen = 16;

  const char* str_val = nullptr;
  uint32_t str_len = 0;

  if (item == kDeviceItemImei) {
    str_val = kImei;
    str_len = kImeiLen;
  } else if (item == kDeviceItemMobileId) {
    str_val = kMobileId;
    str_len = kMobileIdLen;
  } else if (item == kDeviceItemChipId) {
    str_val = kChipId;
    str_len = kChipIdLen;
  }

  if (str_val != nullptr) {
    uint32_t capacity = memory_.Read32(size_ptr);
    memory_.Write32(size_ptr, str_len);
    if (buffer != 0) {
      uint32_t to_copy = std::min(capacity, str_len);
      for (uint32_t i = 0; i < to_copy; ++i) {
        memory_.Write8(buffer + i, static_cast<uint8_t>(str_val[i]));
      }
    }
    core.SetRegister(kR0, 0); // SUCCESS
  } else {
    core.SetRegister(kR0, kEunsupported);
  }
}

std::vector<IShellHle::ExpiredTimer> IShellHle::Tick(uint32_t elapsed_ms) {
  std::vector<ExpiredTimer> expired;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (elapsed_ms >= it->remaining_ms) {
      expired.push_back(ExpiredTimer{it->callback, it->user_data, it->r0_override});
      it = timers_.erase(it);
    } else {
      it->remaining_ms -= elapsed_ms;
      ++it;
    }
  }
  return expired;
}

uint32_t IShellHle::Build(uint32_t vtable_address, uint32_t object_address) {
  // Order matches AEEIShell.h's INHERIT_IShell macro exactly (verified
  // directly against real Qualcomm source -- see TASKS.md Phase 3), up
  // through the pre-BREW-MP slot count (40 IShell-specific methods,
  // which is what a 2009-era Zeebo/BREW 4.x IShell should have --
  // BREW MP's later-appended slots, e.g. RegisterSystemCallback onward,
  // are deliberately not included since Zeebo predates that rebrand).
  std::vector<HleRuntime::HleFunction> methods = {
      LoggedStub("IShell", 0, "AddRef"),                                            // 0 AddRef
      LoggedStub("IShell", 1, "Release"),                                            // 1 Release
      [this](IArmCore& c) { CreateInstanceImpl(c); },   // 2  CreateInstance
      LoggedStub("IShell", 3, "QueryClass"),  // 3 QueryClass
      [this](IArmCore& c) { GetDeviceInfoImpl(c); },  // 4  GetDeviceInfo
      LoggedStub("IShell", 5, "StartApplet"),  // 5 StartApplet
      LoggedStub("IShell", 6, "CloseApplet"),  // 6 CloseApplet
      LoggedStub("IShell", 7, "CanStartApplet"),  // 7 CanStartApplet
      LoggedStub("IShell", 8, "ActiveApplet"),  // 8 ActiveApplet
      LoggedStub("IShell", 9, "EnumAppletInit"),  // 9 EnumAppletInit
      LoggedStub("IShell", 10, "EnumNextApplet"),  // 10 EnumNextApplet
      [this](IArmCore& c) { SetTimerImpl(c); },     // 11 SetTimer
      [this](IArmCore& c) { CancelTimerImpl(c); },  // 12 CancelTimer
      LoggedStub("IShell", 13, "GetTimerExpiration"),  // 13 GetTimerExpiration
      LoggedStub("IShell", 14, "CreateDialog"),  // 14 CreateDialog
      LoggedStub("IShell", 15, "GetActiveDialog"),  // 15 GetActiveDialog
      LoggedStub("IShell", 16, "EndDialog"),  // 16 EndDialog
      LoggedStub("IShell", 17, "LoadResString"),  // 17 LoadResString
      [this](IArmCore& c) { LoadResDataImpl(c); },    // 18 LoadResData
      [this](IArmCore& c) { LoadResObjectImpl(c); },  // 19 LoadResObject
      LoggedStub("IShell", 20, "FreeResData"),  // 20 FreeResData
      [this](IArmCore& c) { SendEventImpl(c); },  // 21 SendEvent
      LoggedStub("IShell", 22, "Beep"),  // 22 Beep
      LoggedStub("IShell", 23, "GetPrefs"),  // 23 GetPrefs
      LoggedStub("IShell", 24, "SetPrefs"),  // 24 SetPrefs
      LoggedStub("IShell", 25, "GetItemStyle"),  // 25 GetItemStyle
      LoggedStub("IShell", 26, "Prompt"),  // 26 Prompt
      LoggedStub("IShell", 27, "MessageBox"),  // 27 MessageBox
      LoggedStub("IShell", 28, "MessageBoxText"),  // 28 MessageBoxText
      LoggedStub("IShell", 29, "SetAlarm"),  // 29 SetAlarm
      LoggedStub("IShell", 30, "CancelAlarm"),  // 30 CancelAlarm
      LoggedStub("IShell", 31, "AlarmsActive"),  // 31 AlarmsActive
      [this](IArmCore& c) { GetHandlerImpl(c); },  // 32 GetHandler
      LoggedStub("IShell", 33, "RegisterHandler"),  // 33 RegisterHandler
      LoggedStub("IShell", 34, "RegisterNotify"),  // 34 RegisterNotify
      LoggedStub("IShell", 35, "Notify"),                                           // 35 Notify
      [this](IArmCore& c) { ResumeImpl(c); },         // 36 Resume
      LoggedStub("IShell", 37, "ForceExit"),                                           // 37 ForceExit
      LoggedStub("IShell", 38, "GetPosition"),  // 38 GetPosition
      LoggedStub("IShell", 39, "CheckPrivLevel"),  // 39 CheckPrivLevel
      LoggedStub("IShell", 40, "IsValidResource"),  // 40 IsValidResource
      [this](IArmCore& c) { LoadResDataExImpl(c); },  // 41 LoadResDataEx
      // Slots below this point are NOT verified against any real header --
      // unlike 0-41 above, they're only known to exist at all because real
      // Double Dragon disassembly (`ddragonz.mod` offset 0x10a234) showed a
      // genuine call through vtable offset 0xac (slot 43), one word past
      // what the "40 IShell-specific methods" pre-BREW-MP count above
      // accounted for -- either that count was of an incomplete real
      // header, or Zeebo's own BREW variant extends classic IShell by a
      // couple of slots the same way this project has already found it
      // doing for other interfaces. Extended with safe, generously-sized
      // headroom (matching this project's established precedent, e.g. the
      // HID device scaffold) rather than pinned exactly to slot 43, so the
      // next real call into this range doesn't reproduce the same
      // undersized-vtable crash.
      LoggedStub("IShell", 42, "RegisterSystemCallback"),  // 42 RegisterSystemCallback
      // Slot 43: DetectType
      // int DetectType(IShell *po, const void *cpBuf, uint32 *pdwSize, const char *cpszName, const char **pcpszMIME)
      // Confirmed by BREW SDK 4.0.2 headers and zeebx oracle (machine.rs:2209 / aee_slots.rs:55).
      // Replaces the historical alternating 35/0 heuristic for Alien Breaker Deluxe with canonical MIME detection.
      [this](IArmCore& c) { DetectTypeImpl(c); },
      [this](IArmCore& c) { GetDeviceInfoExImpl(c); },  // 44 GetDeviceInfoEx
      [this](IArmCore& c) { GetClassItemIdImpl(c); },  // 45 GetClassItemID
      LoggedStub("IShell", 46, "Obsolete"),  // 46 Obsolete
      LoggedStub("IShell", 47, "GetProperty"),  // 47 GetProperty
      LoggedStub("IShell", 48, "SetProperty"),  // 48 SetProperty
      LoggedStub("IShell", 49, "RegisterEvent"),  // 49 RegisterEvent
      LoggedStub("IShell", 50, "Reset"),  // 50 Reset
      LoggedStub("IShell", 51, "AppIsInGroup"),  // 51 AppIsInGroup
  };
  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
