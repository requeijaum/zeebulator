#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Class ID for BREW IThread interface (AEECLSID_CORE + 23 = 0x01001017)
constexpr uint32_t kAeeClsidThread = 0x01001017;

// Represents the state of an active or suspended cooperative IThread.
// Mirrors the clean-room architecture documented from the BREW SDK / zeebx oracle:
// 14 integer registers (R0-R12, SP), resume_pc, stack allocation, and resume callback.
struct ThreadState {
  uint32_t thread_obj = 0;
  bool started = false;
  bool finished = false;
  bool suspended = false;
  uint32_t stack_base = 0;
  uint32_t stack_size = 0;
  uint32_t resume_pc = 0;
  uint32_t resume_cb = 0;
  uint32_t context[14] = {0}; // R0..R12, SP
};

class ThreadHle {
 public:
  ThreadHle(Memory& memory, HleRuntime& hle, std::function<uint32_t(uint32_t)> malloc_fn,
            std::function<void(uint32_t)> free_fn);

  // Builds and returns a new IThread interface object in guest memory.
  uint32_t CreateThreadObject();

  // Checks if a given pointer is registered as the resume callback of any thread.
  bool IsResumeCallback(uint32_t pcb) const;

  // Schedules the thread associated with `pcb` to run on the next cooperative dispatch.
  bool ResumeByCallback(uint32_t pcb);

  // Queries pending threads.
  bool HasPendingThreads() const { return !pending_threads_.empty(); }

  // Enqueues a thread to run on the next cooperative dispatch pass.
  void EnqueueThread(uint32_t thread_obj);

  // Returns list of pending threads to run, clearing the pending list.
  std::vector<uint32_t> TakePendingThreads();

  // Thread state accessors
  ThreadState* GetThreadState(uint32_t thread_obj);
  const ThreadState* GetThreadState(uint32_t thread_obj) const;

  // Suspends the currently running thread: saves registers R0-R12, SP and resume_pc.
  void SuspendCurrentThread(uint32_t thread_obj, const uint32_t regs[14], uint32_t resume_pc);

  // Marks thread finished with exit code.
  void FinishThread(uint32_t thread_obj, uint32_t exit_code);

 private:
  // IThread vtable methods (AEEThread.h: 12 methods)
  // 0: AddRef, 1: Release, 2: QueryInterface
  // 3: Malloc, 4: Free, 5: HoldRsc, 6: ReleaseRsc
  // 7: Start, 8: Exit, 9: Join, 10: Suspend, 11: GetResumeCBK
  void StartImpl(IArmCore& core, uint32_t this_ptr);
  void ExitImpl(IArmCore& core, uint32_t this_ptr);
  void JoinImpl(IArmCore& core, uint32_t this_ptr);
  void SuspendImpl(IArmCore& core, uint32_t this_ptr);
  void GetResumeCBKImpl(IArmCore& core, uint32_t this_ptr);

  Memory& memory_;
  HleRuntime& hle_;
  std::function<uint32_t(uint32_t)> malloc_fn_;
  std::function<void(uint32_t)> free_fn_;

  std::map<uint32_t, ThreadState> threads_;
  std::map<uint32_t, uint32_t> resume_callbacks_; // pcb -> thread_obj
  std::vector<uint32_t> pending_threads_;
};

}  // namespace zeebulator
