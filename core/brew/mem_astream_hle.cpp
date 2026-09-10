#include "core/brew/mem_astream_hle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

}  // namespace

MemAStreamHle::MemAStreamHle(Memory& memory, HleRuntime& hle,
                             uint32_t stream_object_region_start)
    : memory_(memory), hle_(hle), next_object_address_(stream_object_region_start) {}

uint32_t MemAStreamHle::Build(uint32_t vtable_address) {
  vtable_address_ = vtable_address;

  std::vector<HleRuntime::HleFunction> methods = {
      [this](IArmCore& c) { AddRef(c); },      // 0 AddRef
      [this](IArmCore& c) { Release(c); },     // 1 Release
      [this](IArmCore& c) { Readable(c); },    // 2 Readable
      [this](IArmCore& c) { Read(c); },        // 3 Read
      [this](IArmCore& c) { Cancel(c); },      // 4 Cancel
      [this](IArmCore& c) { Set(c); },         // 5 Set
      [this](IArmCore& c) { SetEx(c); },       // 6 SetEx
  };

  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle_.Register(methods[i]);
    memory_.Write32(vtable_address_ + static_cast<uint32_t>(i) * 4, sentinel);
  }
  return vtable_address_;
}

uint32_t MemAStreamHle::AllocateStream() {
  uint32_t obj_addr = next_object_address_;
  next_object_address_ += 4;
  memory_.Write32(obj_addr, vtable_address_);
  streams_[obj_addr] = StreamState{};
  return obj_addr;
}

void MemAStreamHle::AddRef(IArmCore& core) {
  uint32_t this_obj = core.GetRegister(kR0);
  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    it->second.ref_count++;
    core.SetRegister(kR0, it->second.ref_count);
  } else {
    core.SetRegister(kR0, 1);
  }
}

void MemAStreamHle::Release(IArmCore& core) {
  uint32_t this_obj = core.GetRegister(kR0);
  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    if (it->second.ref_count > 0) {
      it->second.ref_count--;
    }
    uint32_t rem = it->second.ref_count;
    if (rem == 0) {
      streams_.erase(it);
    }
    core.SetRegister(kR0, rem);
  } else {
    core.SetRegister(kR0, 0);
  }
}

void MemAStreamHle::Readable(IArmCore& core) {
  // void Readable(IAStream * po, PFNNOTIFY pfn, void * pUser);
  // Memory buffer is immediately available in host memory.
  // If a notify callback is passed, invoke it synchronously or acknowledge ready.
  uint32_t pfn = core.GetRegister(kR1);
  uint32_t puser = core.GetRegister(kR2);
  if (pfn != 0) {
    hle_.CallArmFunction(pfn, puser);
  }
  core.SetRegister(kR0, 0);
}

void MemAStreamHle::Read(IArmCore& core) {
  // int32 Read(IAStream * po, void * pDest, uint32 nWant);
  uint32_t this_obj = core.GetRegister(kR0);
  uint32_t pdest = core.GetRegister(kR1);
  uint32_t nwant = core.GetRegister(kR2);

  auto it = streams_.find(this_obj);
  if (it == streams_.end()) {
    core.SetRegister(kR0, static_cast<uint32_t>(-1));  // error
    return;
  }

  StreamState& s = it->second;
  uint32_t avail = (s.size > s.position) ? (s.size - s.position) : 0;
  uint32_t actual = std::min(nwant, avail);

  for (uint32_t i = 0; i < actual; ++i) {
    uint8_t byte_val = memory_.Read8(s.buffer + s.position + i);
    memory_.Write8(pdest + i, byte_val);
  }
  s.position += actual;
  core.SetRegister(kR0, actual);
}

void MemAStreamHle::Cancel(IArmCore& core) {
  core.SetRegister(kR0, 0);  // SUCCESS
}

void MemAStreamHle::Set(IArmCore& core) {
  // void Set(IMemAStream * po, byte * pBuff, uint32 dwSize, uint32 dwOffset, boolean bSysMem);
  uint32_t this_obj = core.GetRegister(kR0);
  uint32_t pbuff = core.GetRegister(kR1);
  uint32_t dwsize = core.GetRegister(kR2);
  uint32_t dwoffset = core.GetRegister(kR3);

  auto it = streams_.find(this_obj);
  if (it != streams_.end()) {
    it->second.buffer = pbuff;
    it->second.size = dwsize;
    it->second.position = std::min(dwoffset, dwsize);
  }
  core.SetRegister(kR0, 0);
}

void MemAStreamHle::SetEx(IArmCore& core) {
  // void SetEx(IMemAStream * po, byte * pBuff, uint32 dwSize, uint32 nOffset, PFNNOTIFY pUserFreeFn, void *pUserFeeData);
  Set(core);
}

}  // namespace zeebulator
