// core/control/debug_hooks.h
//
// Observation-only breakpoints and watchpoints for RE work.
//
// WHY: chasing the emulation walls (ABD command-list stall, the Data East
// tick-0 cluster) has meant hand-instrumenting ArmInterpreter::Step and
// Memory::Write32 by editing source, rebuilding, and reverting -- three
// separate times across sessions. This institutionalizes that into a
// permanent, env/IPC-driven surface: set a PC breakpoint or a data
// watchpoint over the control channel and the runner pauses (stops
// scheduling ticks) when it trips.
//
// STRICT RULE: this NEVER alters execution. The interpreter is the oracle.
// The hooks only OBSERVE (record a hit + a register snapshot) and the
// game_probe loop decides to stop advancing. Execution semantics -- what
// each instruction does, in what order -- are untouched.
//
// COST WHEN OFF: the very first thing every hook does is read a single
// relaxed atomic (`armed_` for exec, `read_armed_`/`write_armed_` for
// memory) and return immediately if disarmed. With no breakpoints or
// watchpoints set (the default), the memory read path -- the hottest path
// in the emulator -- pays one relaxed load and nothing else.

#ifndef ZEEBULATOR_CORE_CONTROL_DEBUG_HOOKS_H_
#define ZEEBULATOR_CORE_CONTROL_DEBUG_HOOKS_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace zeebulator {

class IArmCore;  // forward decl; OnExec takes it to snapshot registers.

class DebugHooks {
 public:
  enum WatchMode { kW = 1, kR = 2, kRW = 3 };

  static DebugHooks& Instance() {
    static DebugHooks inst;
    return inst;
  }

  // --- configuration (called from the control-channel executor) ---------
  void AddBreakpoint(uint32_t pc) {
    std::lock_guard<std::mutex> lk(mu_);
    bps_.insert(pc);
    Rearm();
  }
  void ClearBreakpoint(uint32_t pc) {
    std::lock_guard<std::mutex> lk(mu_);
    bps_.erase(pc);
    Rearm();
  }
  void AddWatchpoint(uint32_t addr, uint32_t len, WatchMode mode) {
    std::lock_guard<std::mutex> lk(mu_);
    wps_.push_back({addr, len ? len : 1, mode});
    Rearm();
  }
  void ClearAll() {
    std::lock_guard<std::mutex> lk(mu_);
    bps_.clear();
    wps_.clear();
    Rearm();
    hit_.store(false, std::memory_order_relaxed);
  }

  // --- hit state (read/cleared by the game_probe loop) ------------------
  bool Hit() const { return hit_.load(std::memory_order_relaxed); }
  std::string HitInfo() {
    std::lock_guard<std::mutex> lk(mu_);
    return hit_info_;
  }
  void ClearHit() { hit_.store(false, std::memory_order_relaxed); }

  // --- hooks (called from the hot paths) --------------------------------
  // Execution hook: called by ArmInterpreter::Step with the address of the
  // instruction about to execute. Declared here, defined out-of-line in
  // debug_hooks.cpp so this header needn't know IArmCore's full layout.
  inline void OnExec(uint32_t pc, const IArmCore& cpu) {
    if (!armed_.load(std::memory_order_relaxed)) return;
    OnExecSlow(pc, cpu);
  }
  inline void OnMemWrite(uint32_t addr, uint32_t len) {
    if (!write_armed_.load(std::memory_order_relaxed)) return;
    OnMemSlow(addr, len, kW);
  }
  inline void OnMemRead(uint32_t addr, uint32_t len) {
    if (!read_armed_.load(std::memory_order_relaxed)) return;
    OnMemSlow(addr, len, kR);
  }

 private:
  struct Watch {
    uint32_t addr;
    uint32_t len;
    WatchMode mode;
  };

  DebugHooks() = default;

  // Recompute the armed flags from the current bp/wp sets. Caller holds mu_.
  void Rearm() {
    armed_.store(!bps_.empty() || !wps_.empty(), std::memory_order_relaxed);
    bool r = false, w = false;
    for (const auto& wp : wps_) {
      if (wp.mode & kR) r = true;
      if (wp.mode & kW) w = true;
    }
    read_armed_.store(r, std::memory_order_relaxed);
    write_armed_.store(w, std::memory_order_relaxed);
  }

  void OnExecSlow(uint32_t pc, const IArmCore& cpu);  // in .cpp

  void OnMemSlow(uint32_t addr, uint32_t len, WatchMode kind) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& wp : wps_) {
      if (!(wp.mode & kind)) continue;
      // overlap [addr, addr+len) vs [wp.addr, wp.addr+wp.len)
      if (addr < wp.addr + wp.len && wp.addr < addr + len) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "watch %s addr=0x%08x len=%u", kind == kR ? "R" : "W",
                      addr, len);
        hit_info_ = buf;
        hit_.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[dbg] %s\n", buf);
        return;
      }
    }
  }

  std::atomic<bool> armed_{false};
  std::atomic<bool> read_armed_{false};
  std::atomic<bool> write_armed_{false};
  std::atomic<bool> hit_{false};

  std::mutex mu_;
  std::set<uint32_t> bps_;
  std::vector<Watch> wps_;
  std::string hit_info_;
};

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_DEBUG_HOOKS_H_
