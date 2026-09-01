// core/control/debug_hooks.cpp
//
// Out-of-line breakpoint handling (needs IArmCore's full definition to
// snapshot registers on a hit).

#include "core/control/debug_hooks.h"

#include "core/cpu/arm_core.h"

namespace zeebulator {

void DebugHooks::OnExecSlow(uint32_t pc, const IArmCore& cpu) {
  std::lock_guard<std::mutex> lk(mu_);
  if (bps_.find(pc) == bps_.end()) return;
  char buf[256];
  std::snprintf(
      buf, sizeof(buf),
      "bp pc=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x sp=0x%08x "
      "lr=0x%08x cpsr=0x%08x",
      pc, cpu.GetRegister(kR0), cpu.GetRegister(kR1), cpu.GetRegister(kR2),
      cpu.GetRegister(kR3), cpu.GetRegister(kSP), cpu.GetRegister(kLR),
      cpu.GetCpsr());
  hit_info_ = buf;
  hit_.store(true, std::memory_order_relaxed);
  std::fprintf(stderr, "[dbg] %s\n", buf);
}

void DebugHooks::OnTraceSlow(uint32_t pc, const IArmCore& cpu) {
  if (pc < trace_lo_ || pc >= trace_hi_) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (!trace_file_) return;
  if (trace_count_ >= trace_limit_) {
    // Reached the cap: flush/close once and disarm so the hot path goes
    // cold again. Observation-only -- execution is unaffected.
    std::fprintf(trace_file_, "# trace limit %llu reached\n",
                 static_cast<unsigned long long>(trace_limit_));
    std::fclose(trace_file_);
    trace_file_ = nullptr;
    trace_armed_.store(false, std::memory_order_relaxed);
    return;
  }
  // Fetch the opcode word at PC (const_cast: GetMemory() is non-const but
  // Read32 is a pure lookup; we never write).
  uint32_t opcode = const_cast<IArmCore&>(cpu).GetMemory().Read32(pc);
  std::fprintf(trace_file_,
               "%08x %08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x "
               "sp=%08x lr=%08x cpsr=%08x\n",
               pc, opcode, cpu.GetRegister(kR0), cpu.GetRegister(kR1),
               cpu.GetRegister(kR2), cpu.GetRegister(kR3), cpu.GetRegister(kR4),
               cpu.GetRegister(kR5), cpu.GetRegister(kSP), cpu.GetRegister(kLR),
               cpu.GetCpsr());
  ++trace_count_;
}

void DebugHooks::OnWriteWatchSlow(uint32_t addr, uint32_t len) {
  uint32_t pc = last_pc_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(mu_);
  if (!wwatch_file_) return;
  uint64_t key = (static_cast<uint64_t>(addr) << 32) | pc;
  if (!wwatch_seen_.insert(key).second) return;  // dedupe (addr,pc)
  std::fprintf(wwatch_file_, "write addr=0x%08x len=%u  writer_pc=0x%08x\n",
               addr, len, pc);
  std::fflush(wwatch_file_);
}

}  // namespace zeebulator
