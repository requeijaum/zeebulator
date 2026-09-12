#include "core/brew/unzip_stream_hle.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace zeebulator {

namespace {

bool InflateData(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out) {
  constexpr size_t kMaxCompressed = 64u * 1024u * 1024u;
  constexpr size_t kMaxExpanded = 128u * 1024u * 1024u;
  if (src == nullptr || src_len == 0 || src_len > kMaxCompressed) return false;

  auto try_inflate = [&](int window_bits) -> bool {
    z_stream strm{};
    if (inflateInit2(&strm, window_bits) != Z_OK) return false;

    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(src_len);

    std::vector<uint8_t> buffer;
    const size_t initial = std::min(kMaxExpanded, src_len * 4 + 1024);
    try { buffer.resize(initial); } catch (const std::exception&) {
      inflateEnd(&strm);
      return false;
    }

    for (;;) {
      strm.next_out = reinterpret_cast<Bytef*>(buffer.data() + strm.total_out);
      strm.avail_out = static_cast<uInt>(buffer.size() - strm.total_out);

      int ret = inflate(&strm, Z_NO_FLUSH);
      if (ret == Z_STREAM_END) {
        buffer.resize(strm.total_out);
        inflateEnd(&strm);
        out = std::move(buffer);
        return true;
      }
      if (ret != Z_OK) {
        inflateEnd(&strm);
        return false;
      }
      if (strm.avail_out == 0) {
        if (buffer.size() >= kMaxExpanded) {
          inflateEnd(&strm);
          return false;
        }
        const size_t next = std::min(kMaxExpanded, buffer.size() * 2);
        try { buffer.resize(next); } catch (const std::exception&) {
          inflateEnd(&strm);
          return false;
        }
      }
    }
  };

  // Try auto-detect (zlib or gzip header) first (15 + 32)
  if (try_inflate(15 + 32)) return true;
  // Fall back to raw deflate (-15)
  if (try_inflate(-15)) return true;
  // Fall back to standard zlib (15)
  return try_inflate(15);
}

}  // namespace

UnzipStreamHle::UnzipStreamHle(Memory& memory, HleRuntime& hle,
                               uint32_t object_region_start,
                               std::function<uint32_t(uint32_t)> read_source_fn)
    : memory_(memory), hle_(hle), next_object_address_(object_region_start) {}

uint32_t UnzipStreamHle::Build(uint32_t vtable_address) {
  vtable_address_ = vtable_address;

  std::vector<HleRuntime::HleFunction> methods = {
      [this](IArmCore& c) { AddRef(c); },      // 0 AddRef
      [this](IArmCore& c) { Release(c); },     // 1 Release
      [this](IArmCore& c) { Readable(c); },    // 2 Readable
      [this](IArmCore& c) { Read(c); },        // 3 Read
      [this](IArmCore& c) { Cancel(c); },      // 4 Cancel
      [this](IArmCore& c) { SetStream(c); },   // 5 SetStream
  };

  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle_.Register(methods[i]);
    memory_.Write32(vtable_address_ + static_cast<uint32_t>(i) * 4, sentinel);
  }
  return vtable_address_;
}

uint32_t UnzipStreamHle::AllocateStream() {
  uint32_t obj_addr = next_object_address_;
  next_object_address_ += 4;
  memory_.Write32(obj_addr, vtable_address_);
  streams_[obj_addr] = UnzipState{};
  return obj_addr;
}

void UnzipStreamHle::AddRef(IArmCore& core) {
  uint32_t this_obj = core.GetRegister(kR0);
  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    it->second.ref_count++;
    core.SetRegister(kR0, it->second.ref_count);
  } else {
    core.SetRegister(kR0, 1);
  }
}

void UnzipStreamHle::Release(IArmCore& core) {
  uint32_t this_obj = core.GetRegister(kR0);
  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    if (it->second.ref_count > 0) it->second.ref_count--;
    uint32_t rem = it->second.ref_count;
    if (rem == 0) {
      streams_.erase(it);
      pending_readable_.erase(
          std::remove_if(pending_readable_.begin(), pending_readable_.end(),
                         [this_obj](const PendingReadable& p) { return p.stream == this_obj; }),
          pending_readable_.end());
    }
    core.SetRegister(kR0, rem);
  } else {
    core.SetRegister(kR0, 0);
  }
}

void UnzipStreamHle::Readable(IArmCore& core) {
  // void Readable(IAStream * po, PFNNOTIFY pfn, void * pUser);
  uint32_t pfn = core.GetRegister(kR1);
  uint32_t puser = core.GetRegister(kR2);
  if (pfn != 0) {
    pending_readable_.push_back({core.GetRegister(kR0), pfn, puser});
  }
  core.SetRegister(kR0, 0);
}

void UnzipStreamHle::Tick() {
  if (pending_readable_.empty()) return;
  std::vector<PendingReadable> deferred;
  deferred.swap(pending_readable_);
  for (const PendingReadable& notify : deferred) {
    if (streams_.find(notify.stream) != streams_.end())
      hle_.CallArmFunction(notify.fn, notify.user);
  }
}

bool UnzipStreamHle::Expand(UnzipState& state) {
  if (state.expanded) return true;
  if (state.expand_attempted) return false;
  state.expand_attempted = true;
  if (state.source_stream == 0) return false;

  std::vector<uint8_t> compressed;
  if (drainer_) {
    compressed = drainer_(state.source_stream);
  } else {
    // Default fallback: call source_stream's Read method (slot 3)
    uint32_t vtable = memory_.Read32(state.source_stream);
    uint32_t read_slot = memory_.Read32(vtable + 3 * 4);
    if (read_slot != 0) {
      constexpr uint32_t kChunk = 4096;
      uint32_t temp_buf = 0x00095000;
      for (;;) {
        const uint32_t n = hle_.CallArmFunctionPreservingContext(
            read_slot, state.source_stream, temp_buf, kChunk);
        if (n == 0 || n == static_cast<uint32_t>(-1)) break;
        if (n > kChunk || compressed.size() + n > 64u * 1024u * 1024u) return false;
        for (uint32_t i = 0; i < n; ++i) compressed.push_back(memory_.Read8(temp_buf + i));
        if (n < kChunk) break;
      }
    }
  }

  if (compressed.empty()) return false;
  state.expanded = InflateData(compressed.data(), compressed.size(), state.uncompressed);
  return state.expanded;
}

void UnzipStreamHle::Read(IArmCore& core) {
  // int32 Read(IAStream * po, void * pDest, uint32 nWant);
  uint32_t this_obj = core.GetRegister(kR0);
  uint32_t pdest = core.GetRegister(kR1);
  uint32_t nwant = core.GetRegister(kR2);

  auto it = streams_.find(this_obj);
  if (it == streams_.end()) {
    core.SetRegister(kR0, static_cast<uint32_t>(-1));
    return;
  }

  UnzipState& s = it->second;
  if (!s.expanded && !Expand(s)) {
    core.SetRegister(kR0, static_cast<uint32_t>(-1));
    return;
  }

  uint32_t avail = (s.uncompressed.size() > s.position)
                       ? static_cast<uint32_t>(s.uncompressed.size() - s.position)
                       : 0;
  uint32_t actual = std::min(nwant, avail);

  for (uint32_t i = 0; i < actual; ++i) {
    memory_.Write8(pdest + i, s.uncompressed[s.position + i]);
  }
  s.position += actual;
  core.SetRegister(kR0, actual);
}

void UnzipStreamHle::Cancel(IArmCore& core) {
  const uint32_t stream = core.GetRegister(kR0);
  const uint32_t fn = core.GetRegister(kR1);
  const uint32_t user = core.GetRegister(kR2);
  pending_readable_.erase(
      std::remove_if(pending_readable_.begin(), pending_readable_.end(),
                     [=](const PendingReadable& p) {
                       return p.stream == stream && (fn == 0 || (p.fn == fn && p.user == user));
                     }),
      pending_readable_.end());
  core.SetRegister(kR0, 0);
}

void UnzipStreamHle::SetStream(IArmCore& core) {
  // void SetStream(IUnzipAStream * po, IAStream * pIAStream);
  uint32_t this_obj = core.GetRegister(kR0);
  uint32_t source = core.GetRegister(kR1);

  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    it->second.source_stream = source;
    it->second.expanded = false;
    it->second.expand_attempted = false;
    it->second.uncompressed.clear();
    it->second.position = 0;
  }
  core.SetRegister(kR0, 0);
}

}  // namespace zeebulator
