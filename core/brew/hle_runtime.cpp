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
  labels_.emplace_back();         // keep labels_ parallel to functions_
}

uint32_t HleRuntime::Register(HleFunction fn) {
  uint32_t index = static_cast<uint32_t>(functions_.size());
  functions_.push_back(std::move(fn));
  labels_.emplace_back();  // keep labels_ parallel to functions_
  return trap_base_ + index * 4;
}

uint32_t HleRuntime::RegisterLabeled(HleFunction fn, std::string label) {
  uint32_t index = static_cast<uint32_t>(functions_.size());
  functions_.push_back(std::move(fn));
  labels_.push_back(std::move(label));
  return trap_base_ + index * 4;
}

std::string HleRuntime::LabelForAddress(uint32_t sentinel_address) const {
  if (sentinel_address < trap_base_) return {};
  uint32_t index = (sentinel_address - trap_base_) / 4;
  if (index < labels_.size()) return labels_[index];
  return {};
}

void HleRuntime::Dispatch(IArmCore& core, uint32_t address) {
  uint32_t index = (address - trap_base_) / 4;
  if (index == kReturnSentinelSlot) {
    // Leave PC exactly where it is; CallArmFunction's loop below detects
    // this by PC value, not here.
    return;
  }
  if (index < functions_.size() && functions_[index]) {
    // Env-gated full BREW-API trace (ZEEB_LOG_BREW=1): every IMPLEMENTED
    // vtable slot a title invokes, with its label and args. Complements
    // ZEEB_LOG_SLOT (which only fires for UNimplemented slots). Off by
    // default; stderr only; non-perturbing (logs, then runs the real handler).
    if (std::getenv("ZEEB_LOG_BREW") != nullptr) {
      std::string label = LabelForAddress(address);
      std::fprintf(stderr,
                   "[brew] %s idx=%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x\n",
                   label.empty() ? "(unlabeled)" : label.c_str(), index,
                   core.GetRegister(kR0), core.GetRegister(kR1),
                   core.GetRegister(kR2), core.GetRegister(kR3), core.GetRegister(kLR));
    }
    functions_[index](core);
  } else if (std::getenv("ZEEB_LOG_SLOT") != nullptr) {
    // Phase 9c per-slot logger (env-gated, non-perturbing: stderr only, no new
    // traps / no functions_ mutation). Surfaces every unimplemented vtable slot
    // a title actually calls, with args and caller, to drive which handler to
    // implement next from real demand. Emits once per (slot) to avoid floods.
    static std::set<uint32_t> seen;
    if (seen.insert(index).second) {
      std::string label = LabelForAddress(address);
      std::fprintf(stderr,
                   "[slot] unimplemented %s trap idx=%u addr=0x%08x "
                   "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x\n",
                   label.empty() ? "(unlabeled)" : label.c_str(),
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
