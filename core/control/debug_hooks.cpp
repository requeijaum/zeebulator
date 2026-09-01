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

}  // namespace zeebulator
