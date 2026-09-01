#include "core/brew/hle_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace zeebulator {

namespace {
constexpr uint32_t kReturnSentinelSlot = 0;
}  // namespace

HleRuntime::HleRuntime(IArmCore& core, uint32_t trap_base, uint32_t trap_size)
    : core_(core), trap_base_(trap_base) {
  core.SetCallOutRange(trap_base, trap_size);
  core.SetCallOutHandler(
      [this](IArmCore& c, uint32_t addr) { Dispatch(c, addr); });
  functions_.push_back(nullptr);  // slot 0: reserved return sentinel
}

uint32_t HleRuntime::Register(HleFunction fn) {
  uint32_t index = static_cast<uint32_t>(functions_.size());
  functions_.push_back(std::move(fn));
  return trap_base_ + index * 4;
}

void HleRuntime::Dispatch(IArmCore& core, uint32_t address) {
  uint32_t index = (address - trap_base_) / 4;
  if (index == kReturnSentinelSlot) {
    // Leave PC exactly where it is; CallArmFunction's loop below detects
    // this by PC value, not here.
    return;
  }
  if (index < functions_.size() && functions_[index]) {
    functions_[index](core);
  } else if (std::getenv("ZEEB_LOG_SLOT") != nullptr) {
    // Phase 9c per-slot logger (env-gated, non-perturbing: stderr only, no new
    // traps / no functions_ mutation). Surfaces every unimplemented vtable slot
    // a title actually calls, with args and caller, to drive which handler to
    // implement next from real demand. Emits once per (slot) to avoid floods.
    static std::set<uint32_t> seen;
    if (seen.insert(index).second) {
      std::fprintf(stderr,
                   "[slot] unimplemented trap idx=%u addr=0x%08x "
                   "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x\n",
                   index, address, core.GetRegister(kR0), core.GetRegister(kR1),
                   core.GetRegister(kR2), core.GetRegister(kR3),
                   core.GetRegister(kLR));
    }
  }
  core.SetRegister(kPC, core.GetRegister(kLR));  // simulate BX LR
}

uint32_t HleRuntime::CallArmFunction(uint32_t target, uint32_t r0, uint32_t r1,
                                      uint32_t r2, uint32_t r3) {
  core_.SetRegister(kR0, r0);
  core_.SetRegister(kR1, r1);
  core_.SetRegister(kR2, r2);
  core_.SetRegister(kR3, r3);
  core_.SetRegister(kLR, trap_base_);  // slot 0 = return sentinel
  core_.SetRegister(kPC, target);
  while (core_.GetRegister(kPC) != trap_base_) {
    core_.Step();
  }
  return core_.GetRegister(kR0);
}

uint32_t HleRuntime::ReadStackArg(IArmCore& core, uint32_t index) {
  return core.GetMemory().Read32(core.GetRegister(kSP) + index * 4);
}

}  // namespace zeebulator
