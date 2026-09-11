// core/control/call_stack_tracer.h
//
// Thread-safe call trace, shadow call stack, stack unwind, and tree-view tracer.
//
// Designed to trace function calls (BL/BLX, Thumb BL/BLX, and BREW HLE call-outs/returns),
// maintain a shadow call stack for instant stack traces, unroll runtime stack frames
// using guest Memory/SP/LR, and export a structured/ASCII Tree View for terminal & UI inspection.

#ifndef ZEEBULATOR_CORE_CONTROL_CALL_STACK_TRACER_H_
#define ZEEBULATOR_CORE_CONTROL_CALL_STACK_TRACER_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/memory/memory.h"

namespace zeebulator {

class IArmCore;

struct StackFrame {
  uint32_t pc = 0;
  uint32_t lr = 0;
  uint32_t sp = 0;
  uint32_t target = 0;
  std::string name;
  uint64_t enter_tick = 0;
};

struct CallTraceNode {
  uint32_t call_addr = 0;
  uint32_t target_addr = 0;
  uint32_t lr = 0;
  std::string name;
  int depth = 0;
  uint64_t call_count = 0;
  std::vector<std::shared_ptr<CallTraceNode>> children;
  std::shared_ptr<CallTraceNode> parent;
};

class CallStackTracer {
 public:
  static CallStackTracer& Instance() {
    static CallStackTracer tracer;
    return tracer;
  }

  void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool IsEnabled() const { return enabled_; }

  void Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    shadow_stack_.clear();
    root_node_ = std::make_shared<CallTraceNode>();
    root_node_->name = "root";
    root_node_->depth = 0;
    current_node_ = root_node_;
    trace_log_.clear();
  }

  void RegisterSymbol(uint32_t addr, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    symbols_[addr] = name;
  }

  void RegisterSymbolRange(uint32_t start_addr, uint32_t end_addr, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    range_symbols_[start_addr] = {end_addr, name};
  }

  std::string ResolveSymbol(uint32_t addr) const {
    std::lock_guard<std::mutex> lk(mu_);
    return ResolveSymbolLocked(addr);
  }

  // Hook called when a call instruction executes (BL, BLX, or HLE enter)
  void OnCall(uint32_t caller_pc, uint32_t target_addr, uint32_t lr, uint32_t sp, uint64_t tick = 0);

  // Hook called on function return (BX LR, POP {..., pc}, LDR pc, [sp], HLE return)
  void OnReturn(uint32_t current_pc, uint32_t lr, uint32_t sp);

  // Snapshot of the shadow call stack (fast, exact)
  std::vector<StackFrame> GetShadowStack() const;

  // Unwind from raw ARM SP & memory (reads stack words searching for valid return addresses / frames)
  std::vector<uint32_t> UnwindRawStack(const IArmCore& cpu, uint32_t max_depth = 32) const;

  // Get formatted Stack Trace string
  std::string FormatStackTrace(const IArmCore& cpu) const;

  // Format call tree into human-readable ASCII tree view
  std::string FormatCallTree(int max_depth = 16) const;

  // Get trace log lines
  std::vector<std::string> GetRecentTraceLog(size_t max_lines = 100) const;

 private:
  CallStackTracer() {
    if (std::getenv("ZEEB_TRACE_CALLS") != nullptr) {
      enabled_ = true;
    }
    root_node_ = std::make_shared<CallTraceNode>();
    root_node_->name = "root";
    root_node_->depth = 0;
    current_node_ = root_node_;
  }

  std::string ResolveSymbolLocked(uint32_t addr) const;
  void FormatNodeAscii(const CallTraceNode& node, const std::string& prefix, bool is_last,
                       int max_depth, std::string& out) const;

  mutable std::mutex mu_;
  bool enabled_ = false;

  std::map<uint32_t, std::string> symbols_;
  std::map<uint32_t, std::pair<uint32_t, std::string>> range_symbols_;

  std::vector<StackFrame> shadow_stack_;
  std::shared_ptr<CallTraceNode> root_node_;
  std::shared_ptr<CallTraceNode> current_node_;
  std::deque<std::string> trace_log_;
  static constexpr size_t kMaxTraceLog = 500;
};

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_CALL_STACK_TRACER_H_
