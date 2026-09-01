#include "core/cpu/dynarmic_arm_core.h"

#include <cstdlib>
#include <cstring>

namespace zeebulator {

DynarmicArmCore::DynarmicArmCore() = default;

void DynarmicArmCore::Reset() { interpreter_.Reset(); }

void DynarmicArmCore::Step() { interpreter_.Step(); }

uint64_t DynarmicArmCore::Run(uint64_t max_instructions) {
  return interpreter_.Run(max_instructions);
}

uint32_t DynarmicArmCore::GetRegister(int index) const {
  return interpreter_.GetRegister(index);
}

void DynarmicArmCore::SetRegister(int index, uint32_t value) {
  interpreter_.SetRegister(index, value);
}

uint32_t DynarmicArmCore::GetCpsr() const { return interpreter_.GetCpsr(); }

void DynarmicArmCore::SetCpsr(uint32_t value) { interpreter_.SetCpsr(value); }

Memory& DynarmicArmCore::GetMemory() { return interpreter_.GetMemory(); }

void DynarmicArmCore::SetCallOutRange(uint32_t base, uint32_t size) {
  interpreter_.SetCallOutRange(base, size);
}

void DynarmicArmCore::SetCallOutHandler(CallOutHandler handler) {
  interpreter_.SetCallOutHandler(std::move(handler));
}

CpuBackend SelectCpuBackendFromEnv() {
  const char* v = std::getenv("ZEEB_CPU");
  if (v != nullptr && (std::strcmp(v, "jit") == 0 || std::strcmp(v, "dynarmic") == 0)) {
    return CpuBackend::kDynarmic;
  }
  return CpuBackend::kInterpreter;
}

std::unique_ptr<IArmCore> MakeArmCore(CpuBackend backend) {
  switch (backend) {
    case CpuBackend::kDynarmic:
      return std::make_unique<DynarmicArmCore>();
    case CpuBackend::kInterpreter:
    default:
      return std::make_unique<ArmInterpreter>();
  }
}

}  // namespace zeebulator
