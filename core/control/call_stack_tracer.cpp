// core/control/call_stack_tracer.cpp

#include "core/control/call_stack_tracer.h"

#include <cstdio>
#include <iomanip>
#include <sstream>

#include "core/cpu/arm_core.h"

namespace zeebulator {

std::string CallStackTracer::ResolveSymbolLocked(uint32_t addr) const {
  // Exact match first
  auto it = symbols_.find(addr);
  if (it != symbols_.end()) {
    return it->second;
  }
  // Mask off thumb bit if any
  auto it_thumb = symbols_.find(addr & ~1u);
  if (it_thumb != symbols_.end()) {
    return it_thumb->second;
  }
  // Check range symbols
  for (const auto& [start, info] : range_symbols_) {
    if (addr >= start && addr < info.first) {
      char offset_buf[32];
      std::snprintf(offset_buf, sizeof(offset_buf), "+0x%x", addr - start);
      return info.second + offset_buf;
    }
  }

  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%08X", addr);
  return std::string(buf);
}

void CallStackTracer::OnCall(uint32_t caller_pc, uint32_t target_addr, uint32_t lr,
                             uint32_t sp, uint64_t tick) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lk(mu_);

  std::string name = ResolveSymbolLocked(target_addr);

  StackFrame frame;
  frame.pc = caller_pc;
  frame.lr = lr;
  frame.sp = sp;
  frame.target = target_addr;
  frame.name = name;
  frame.enter_tick = tick;

  shadow_stack_.push_back(frame);

  // Tree view tracking
  if (!current_node_) {
    current_node_ = root_node_;
  }

  std::shared_ptr<CallTraceNode> child = nullptr;
  for (auto& existing : current_node_->children) {
    if (existing->target_addr == target_addr) {
      child = existing;
      break;
    }
  }

  if (!child) {
    child = std::make_shared<CallTraceNode>();
    child->call_addr = caller_pc;
    child->target_addr = target_addr;
    child->lr = lr;
    child->name = name;
    child->depth = current_node_->depth + 1;
    child->parent = current_node_;
    current_node_->children.push_back(child);
  }

  child->call_count++;
  current_node_ = child;

  // Log record
  char log_buf[256];
  int indent = static_cast<int>(shadow_stack_.size());
  if (indent > 20) indent = 20;
  std::string pad(indent * 2, ' ');
  std::snprintf(log_buf, sizeof(log_buf), "%s[CALL] 0x%08X -> %s (LR=0x%08X, SP=0x%08X)",
                pad.c_str(), caller_pc, name.c_str(), lr, sp);
  trace_log_.push_back(log_buf);
  if (trace_log_.size() > kMaxTraceLog) trace_log_.pop_front();
}

void CallStackTracer::OnReturn(uint32_t current_pc, uint32_t lr, uint32_t sp) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lk(mu_);

  if (!shadow_stack_.empty()) {
    // If returning, pop matching or top
    shadow_stack_.pop_back();
  }

  if (current_node_ && current_node_->parent) {
    current_node_ = current_node_->parent;
  }

  char log_buf[256];
  int indent = static_cast<int>(shadow_stack_.size());
  if (indent > 20) indent = 20;
  std::string pad(indent * 2, ' ');
  std::snprintf(log_buf, sizeof(log_buf), "%s[RET ] 0x%08X (LR=0x%08X, SP=0x%08X)",
                pad.c_str(), current_pc, lr, sp);
  trace_log_.push_back(log_buf);
  if (trace_log_.size() > kMaxTraceLog) trace_log_.pop_front();
}

std::vector<StackFrame> CallStackTracer::GetShadowStack() const {
  std::lock_guard<std::mutex> lk(mu_);
  return shadow_stack_;
}

std::vector<uint32_t> CallStackTracer::UnwindRawStack(const IArmCore& cpu, uint32_t max_depth) const {
  std::vector<uint32_t> frames;
  uint32_t sp = cpu.GetRegister(kSP);
  uint32_t pc = cpu.GetRegister(kPC);
  uint32_t lr = cpu.GetRegister(kLR);

  frames.push_back(pc);
  if (lr != 0 && lr != 0xFFFFFFFFu) {
    frames.push_back(lr);
  }

  Memory& mem = const_cast<IArmCore&>(cpu).GetMemory();
  // Scan SP memory upwards for plausible code addresses
  for (uint32_t offset = 0; offset < max_depth * 4; offset += 4) {
    uint32_t addr = sp + offset;
    // Keep within valid memory range
    if (addr < 0x00001000 || addr >= 0xFFFFFFF0) break;
    uint32_t val = mem.Read32(addr);
    // Plausible code pointers (e.g. within 0x00010000..0x03000000 or trap range)
    if ((val >= 0x00010000 && val < 0x10000000) || (val >= 0x80000000 && val < 0x81000000)) {
      // Avoid duplicate consecutive entries
      if (frames.empty() || frames.back() != val) {
        frames.push_back(val);
      }
    }
  }

  return frames;
}

std::string CallStackTracer::FormatStackTrace(const IArmCore& cpu) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream oss;
  oss << "=== Call Stack Trace ===" << std::endl;

  if (!shadow_stack_.empty()) {
    oss << "-- Shadow Call Stack (exact call history) --" << std::endl;
    for (int i = static_cast<int>(shadow_stack_.size()) - 1; i >= 0; --i) {
      const auto& f = shadow_stack_[i];
      oss << "#" << std::setw(2) << std::left << (shadow_stack_.size() - 1 - i)
          << " [PC=0x" << std::hex << std::setw(8) << std::setfill('0') << f.pc
          << "] Target: " << f.name << " (0x" << f.target << ")"
          << " [LR=0x" << f.lr << " SP=0x" << f.sp << "]"
          << std::dec << std::setfill(' ') << std::endl;
    }
  } else {
    oss << "-- Raw Memory Stack Unwind --" << std::endl;
    std::vector<uint32_t> raw = UnwindRawStack(cpu);
    for (size_t i = 0; i < raw.size(); ++i) {
      std::string sym = ResolveSymbolLocked(raw[i]);
      oss << "#" << std::setw(2) << std::left << i
          << " 0x" << std::hex << std::setw(8) << std::setfill('0') << raw[i]
          << " in " << sym
          << std::dec << std::setfill(' ') << std::endl;
    }
  }
  return oss.str();
}

void CallStackTracer::FormatNodeAscii(const CallTraceNode& node, const std::string& prefix,
                                      bool is_last, int max_depth, std::string& out) const {
  if (node.depth > max_depth) return;

  if (node.depth > 0) {
    out += prefix;
    out += is_last ? "└── " : "├── ";
    char count_buf[64];
    std::snprintf(count_buf, sizeof(count_buf), " (called %llu times)",
                  static_cast<unsigned long long>(node.call_count));
    out += node.name + count_buf + "\n";
  }

  std::string next_prefix = prefix;
  if (node.depth > 0) {
    next_prefix += is_last ? "    " : "│   ";
  }

  for (size_t i = 0; i < node.children.size(); ++i) {
    bool last_child = (i == node.children.size() - 1);
    FormatNodeAscii(*node.children[i], next_prefix, last_child, max_depth, out);
  }
}

std::string CallStackTracer::FormatCallTree(int max_depth) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tree = "=== Call Trace Tree View ===\n";
  if (!root_node_ || root_node_->children.empty()) {
    tree += "(no call trace recorded yet)\n";
    return tree;
  }
  FormatNodeAscii(*root_node_, "", true, max_depth, tree);
  return tree;
}

std::vector<std::string> CallStackTracer::GetRecentTraceLog(size_t max_lines) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> res;
  size_t count = std::min(max_lines, trace_log_.size());
  size_t start = trace_log_.size() - count;
  for (size_t i = start; i < trace_log_.size(); ++i) {
    res.push_back(trace_log_[i]);
  }
  return res;
}

}  // namespace zeebulator
