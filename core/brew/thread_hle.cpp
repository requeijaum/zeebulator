#include "core/brew/thread_hle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kEAlready = 26;  // AEEError.h
constexpr uint32_t kENoMemory = 2;   // AEEError.h
constexpr uint32_t kMinThreadStack = 64 * 1024;
constexpr uint32_t kCallbackSize = 28;
}  // namespace

ThreadHle::ThreadHle(Memory& memory, HleRuntime& hle,
                     std::function<uint32_t(uint32_t)> malloc_fn,
                     std::function<void(uint32_t)> free_fn)
    : memory_(memory), hle_(hle), malloc_fn_(std::move(malloc_fn)), free_fn_(std::move(free_fn)) {}

uint32_t ThreadHle::CreateThreadObject() {
  uint32_t obj_size = 16;
  uint32_t thread_obj = malloc_fn_(obj_size);
  if (thread_obj == 0) return 0;

  ThreadState state;
  state.thread_obj = thread_obj;
  threads_[thread_obj] = state;

  std::vector<HleRuntime::HleFunction> methods(12);

  // 0: AddRef
  methods[0] = [this, thread_obj](IArmCore& core) {
    auto it = threads_.find(thread_obj);
    if (it == threads_.end()) { core.SetRegister(kR0, 0); return; }
    if (it->second.ref_count != 0xffffffffu) ++it->second.ref_count;
    core.SetRegister(kR0, it->second.ref_count);
  };

  // 1: Release. O codigo antigo liberava o objeto em TODA chamada e ainda o
  // liberava de novo se a entrada ja tivesse sido apagada.
  methods[1] = [this, thread_obj](IArmCore& core) {
    auto it = threads_.find(thread_obj);
    if (it == threads_.end()) { core.SetRegister(kR0, 0); return; }
    if (it->second.ref_count > 1) {
      core.SetRegister(kR0, --it->second.ref_count);
      return;
    }
    const ThreadState dying = it->second;
    if (dying.resume_cb != 0) {
      resume_callbacks_.erase(dying.resume_cb);
      if (free_fn_) free_fn_(dying.resume_cb);
    }
    pending_threads_.erase(std::remove(pending_threads_.begin(), pending_threads_.end(), thread_obj),
                           pending_threads_.end());
    if (dying.stack_base != 0 && free_fn_) free_fn_(dying.stack_base);
    threads_.erase(it);
    if (free_fn_) {
      if (dying.vtable_addr != 0) free_fn_(dying.vtable_addr);
      free_fn_(thread_obj);
    }
    core.SetRegister(kR0, 0);
  };

  // 2: QueryInterface adquire outra referencia.
  methods[2] = [this, thread_obj](IArmCore& core) {
    uint32_t out_ptr = core.GetRegister(kR2);
    auto it = threads_.find(thread_obj);
    if (out_ptr == 0 || it == threads_.end()) {
      core.SetRegister(kR0, 14);  // EBADPARM
      return;
    }
    if (it->second.ref_count != 0xffffffffu) ++it->second.ref_count;
    core.GetMemory().Write32(out_ptr, thread_obj);
    core.SetRegister(kR0, kSuccess);
  };

  // 3: Malloc
  methods[3] = [this](IArmCore& core) {
    uint32_t sz = core.GetRegister(kR1);
    uint32_t ptr = malloc_fn_ ? malloc_fn_(sz) : 0;
    core.SetRegister(kR0, ptr);
  };

  // 4: Free
  methods[4] = [this](IArmCore& core) {
    uint32_t ptr = core.GetRegister(kR1);
    if (free_fn_ && ptr != 0) free_fn_(ptr);
    core.SetRegister(kR0, kSuccess);
  };

  // 5: HoldRsc
  methods[5] = [](IArmCore& core) {
    core.SetRegister(kR0, kSuccess);
  };

  // 6: ReleaseRsc
  methods[6] = [](IArmCore& core) {
    core.SetRegister(kR0, kSuccess);
  };

  // 7: Start(IThread* po, int nStackSz, PFNTHREAD pfStart, void *pvStart)
  methods[7] = [this, thread_obj](IArmCore& core) {
    StartImpl(core, thread_obj);
  };

  // 8: Exit(IThread* po, int nRv)
  methods[8] = [this, thread_obj](IArmCore& core) {
    ExitImpl(core, thread_obj);
  };

  // 9: Join(IThread* po, AEECallback *pcb, int *pnRv)
  methods[9] = [this, thread_obj](IArmCore& core) {
    JoinImpl(core, thread_obj);
  };

  // 10: Suspend(IThread* po)
  methods[10] = [this, thread_obj](IArmCore& core) {
    SuspendImpl(core, thread_obj);
  };

  // 11: GetResumeCBK(IThread* po)
  methods[11] = [this, thread_obj](IArmCore& core) {
    GetResumeCBKImpl(core, thread_obj);
  };

  // Build the vtable in memory
  uint32_t vtable_addr = malloc_fn_(static_cast<uint32_t>(methods.size() * 4));
  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t trap_addr = hle_.Register(methods[i]);
    memory_.Write32(vtable_addr + static_cast<uint32_t>(i * 4), trap_addr);
  }
  memory_.Write32(thread_obj, vtable_addr);
  threads_[thread_obj].vtable_addr = vtable_addr;

  return thread_obj;
}

void ThreadHle::StartImpl(IArmCore& core, uint32_t this_ptr) {
  auto it = threads_.find(this_ptr);
  if (it == threads_.end()) {
    core.SetRegister(kR0, kENoMemory);
    return;
  }
  ThreadState& state = it->second;
  if (state.started) {
    core.SetRegister(kR0, kEAlready);
    return;
  }

  uint32_t stack_size = core.GetRegister(kR1);
  uint32_t pf_start = core.GetRegister(kR2);
  uint32_t pv_start = core.GetRegister(kR3);

  std::printf("[thread_hle] Start called: this=0x%08x stack_sz=%u pf_start=0x%08x pv_start=0x%08x\n",
              this_ptr, stack_size, pf_start, pv_start);

  uint32_t real_stack_sz = std::max(stack_size, kMinThreadStack);
  uint32_t stack_base = malloc_fn_ ? malloc_fn_(real_stack_sz) : 0;
  if (stack_base == 0) {
    core.SetRegister(kR0, kENoMemory);
    return;
  }

  // ARM AAPCS requires 8-byte aligned stack pointer at public interfaces
  uint32_t sp_top = (stack_base + real_stack_sz) & ~7u;

  state.started = true;
  state.stack_base = stack_base;
  state.stack_size = real_stack_sz;
  state.resume_pc = pf_start;

  // Initialize context registers:
  // r0 = IThread* this_ptr
  // r1 = pvStart
  // r13 (SP) = top of stack
  std::memset(state.context, 0, sizeof(state.context));
  state.context[0] = this_ptr;
  state.context[1] = pv_start;
  state.context[13] = sp_top;

  pending_threads_.push_back(this_ptr);
  core.SetRegister(kR0, kSuccess);
}

void ThreadHle::ExitImpl(IArmCore& core, uint32_t this_ptr) {
  uint32_t exit_code = core.GetRegister(kR1);
  FinishThread(this_ptr, exit_code);
  // Return control: set return address to trap_base so current execution exits
  core.SetRegister(kLR, hle_.trap_base());
  core.SetRegister(kR0, kSuccess);
}

void ThreadHle::JoinImpl(IArmCore& core, uint32_t /*this_ptr*/) {
  // Join is rarely used in simple loading loops, return success
  core.SetRegister(kR0, kSuccess);
}

void ThreadHle::SuspendImpl(IArmCore& core, uint32_t this_ptr) {
  auto it = threads_.find(this_ptr);
  if (it != threads_.end()) {
    // Save current thread context (R0-R12, SP)
    for (int i = 0; i <= 12; ++i) {
      it->second.context[i] = core.GetRegister(i);
    }
    it->second.context[13] = core.GetRegister(kSP);
    it->second.resume_pc = core.GetRegister(kLR);
    it->second.suspended = true;
  }
  core.SetRegister(kR0, kSuccess);
  if (yield_fn_) {
    yield_fn_();
  }
}

void ThreadHle::GetResumeCBKImpl(IArmCore& core, uint32_t this_ptr) {
  auto it = threads_.find(this_ptr);
  if (it == threads_.end()) {
    core.SetRegister(kR0, 0);
    return;
  }

  if (it->second.resume_cb != 0) {
    core.SetRegister(kR0, it->second.resume_cb);
    return;
  }

  uint32_t cb = malloc_fn_ ? malloc_fn_(kCallbackSize) : 0;
  if (cb != 0) {
    for (uint32_t off = 0; off < kCallbackSize; off += 4) {
      memory_.Write32(cb + off, 0);
    }
    it->second.resume_cb = cb;
    resume_callbacks_[cb] = this_ptr;
  }
  core.SetRegister(kR0, cb);
}

bool ThreadHle::IsResumeCallback(uint32_t pcb) const {
  return resume_callbacks_.find(pcb) != resume_callbacks_.end();
}

bool ThreadHle::ResumeByCallback(uint32_t pcb) {
  auto it = resume_callbacks_.find(pcb);
  if (it == resume_callbacks_.end()) return false;

  uint32_t thread_obj = it->second;
  std::printf("[thread_hle] ResumeByCallback called: pcb=0x%08x thread=0x%08x\n", pcb, thread_obj);
  if (std::find(pending_threads_.begin(), pending_threads_.end(), thread_obj) ==
      pending_threads_.end()) {
    pending_threads_.push_back(thread_obj);
  }
  return true;
}

std::vector<uint32_t> ThreadHle::TakePendingThreads() {
  std::vector<uint32_t> pending;
  pending.swap(pending_threads_);
  return pending;
}

void ThreadHle::EnqueueThread(uint32_t thread_obj) {
  if (std::find(pending_threads_.begin(), pending_threads_.end(), thread_obj) ==
      pending_threads_.end()) {
    pending_threads_.push_back(thread_obj);
  }
}

ThreadState* ThreadHle::GetThreadState(uint32_t thread_obj) {
  auto it = threads_.find(thread_obj);
  if (it != threads_.end()) return &it->second;
  return nullptr;
}

const ThreadState* ThreadHle::GetThreadState(uint32_t thread_obj) const {
  auto it = threads_.find(thread_obj);
  if (it != threads_.end()) return &it->second;
  return nullptr;
}

void ThreadHle::SuspendCurrentThread(uint32_t thread_obj, const uint32_t regs[14],
                                    uint32_t resume_pc) {
  auto it = threads_.find(thread_obj);
  if (it != threads_.end()) {
    std::memcpy(it->second.context, regs, sizeof(it->second.context));
    it->second.resume_pc = resume_pc;
    it->second.suspended = true;
  }
}

void ThreadHle::FinishThread(uint32_t thread_obj, uint32_t /*exit_code*/) {
  auto it = threads_.find(thread_obj);
  if (it != threads_.end()) {
    it->second.finished = true;
    it->second.suspended = false;
  }
}

}  // namespace zeebulator
