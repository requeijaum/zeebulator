#include "core/cpu/dynarmic_arm_core.h"

#include <cstdlib>
#include <cstddef>
#include <cstring>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

namespace zeebulator {

// Bridges the dynarmic JIT to our sparse Memory. Reads/writes are composed
// from byte accesses so unaligned access (which dynarmic explicitly allows)
// is always correct and matches how the interpreter's Memory behaves.
struct DynarmicArmCore::Callbacks final : Dynarmic::A32::UserCallbacks {
  Memory* memory = nullptr;
  Dynarmic::A32::Jit* jit = nullptr;  // for self-modifying-code invalidation
  std::uint64_t ticks_left = 0;

  std::uint8_t MemoryRead8(std::uint32_t a) override { return memory->Read8(a); }
  std::uint16_t MemoryRead16(std::uint32_t a) override {
    return std::uint16_t(MemoryRead8(a)) | std::uint16_t(MemoryRead8(a + 1)) << 8;
  }
  std::uint32_t MemoryRead32(std::uint32_t a) override {
    return std::uint32_t(MemoryRead16(a)) | std::uint32_t(MemoryRead16(a + 2)) << 16;
  }
  std::uint64_t MemoryRead64(std::uint32_t a) override {
    return std::uint64_t(MemoryRead32(a)) | std::uint64_t(MemoryRead32(a + 4)) << 32;
  }

  // Guest stores can be self-modifying code (STR into a code page). dynarmic
  // caches recompiled blocks by address, so every write must invalidate the
  // touched range or a stale block would re-execute. InvalidateCacheRange is
  // cheap when nothing is cached there. All wider writes compose from Write8,
  // so invalidating here covers every store width.
  void MemoryWrite8(std::uint32_t a, std::uint8_t v) override {
    memory->Write8(a, v);
    if (jit) jit->InvalidateCacheRange(a, 1);
  }
  void MemoryWrite16(std::uint32_t a, std::uint16_t v) override {
    MemoryWrite8(a, std::uint8_t(v));
    MemoryWrite8(a + 1, std::uint8_t(v >> 8));
  }
  void MemoryWrite32(std::uint32_t a, std::uint32_t v) override {
    MemoryWrite16(a, std::uint16_t(v));
    MemoryWrite16(a + 2, std::uint16_t(v >> 16));
  }
  void MemoryWrite64(std::uint32_t a, std::uint64_t v) override {
    MemoryWrite32(a, std::uint32_t(v));
    MemoryWrite32(a + 4, std::uint32_t(v >> 32));
  }

  // The interpreter never reaches SWI/coprocessor/undefined instructions on
  // any real game probed so far (it raises UnimplementedInstruction). We keep
  // these as no-ops in Phase 1 rather than throwing across JIT-generated
  // frames (undefined behaviour); Phase 2/3 differential runs are where such
  // encodings, if they ever appear, get catalogued.
  void CallSVC(std::uint32_t /*swi*/) override {}
  void ExceptionRaised(std::uint32_t /*pc*/, Dynarmic::A32::Exception /*e*/) override {}
  // Never reached in practice: we don't request interpreter fallback for any
  // block (dynarmic's own docs note this callback "is never called"). Kept as
  // a no-op rather than std::terminate() so a hypothetical stray call can't
  // abort mid-frame; a real occurrence would surface as a lockstep divergence
  // in Phase 2/3.
  void InterpreterFallback(std::uint32_t /*pc*/, std::size_t /*num_instructions*/) override {}

  void AddTicks(std::uint64_t ticks) override {
    if (ticks > ticks_left) {
      ticks_left = 0;
    } else {
      ticks_left -= ticks;
    }
  }
  std::uint64_t GetTicksRemaining() override { return ticks_left; }
};

namespace {
Dynarmic::A32::ArchVersion ZeeboArchVersion() {
  // Zeebo's application core is an ARM1136J-S (ARMv6K). Matching the arch
  // version keeps the JIT's decode/behaviour aligned with the interpreter's
  // ARMv6 (ARM1136J-S) model documented in arm_interpreter.h.
  return Dynarmic::A32::ArchVersion::v6K;
}
}  // namespace

DynarmicArmCore::DynarmicArmCore()
    : callbacks_(std::make_unique<Callbacks>()) {
  callbacks_->memory = &memory_;
  Dynarmic::A32::UserConfig cfg;
  cfg.callbacks = callbacks_.get();
  cfg.arch_version = ZeeboArchVersion();
  jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
  callbacks_->jit = jit_.get();
  Reset();
}

DynarmicArmCore::~DynarmicArmCore() = default;

void DynarmicArmCore::Reset() {
  jit_->Reset();
  jit_->ClearCache();
  jit_->Regs().fill(0);
  jit_->SetCpsr(0);
}

void DynarmicArmCore::Step() {
  // Mirror ArmInterpreter::Step (arm_interpreter.cpp:1005-1013) exactly: the
  // call-out trap is a control-flow decision made in OUR code, before any
  // instruction fetch, so it is never entangled with a JIT block boundary.
  const uint32_t fetch_addr = jit_->Regs()[15];
  if (IsCallOutAddress(fetch_addr)) {
    if (call_out_handler_) {
      call_out_handler_(*this, fetch_addr);
    }
    return;
  }
  // Keep the tick budget non-empty so cycle counting never halts a single
  // step early; Jit::Step() executes exactly one instruction regardless.
  callbacks_->ticks_left = 1;
  jit_->Step();
}

uint64_t DynarmicArmCore::Run(uint64_t max_instructions) {
  // Byte-for-byte mirror of ArmInterpreter::Run (arm_interpreter.cpp:1217).
  uint64_t executed = 0;
  while (executed < max_instructions) {
    const uint32_t fetch_addr = jit_->Regs()[15];
    const bool will_trap = IsCallOutAddress(fetch_addr);
    Step();
    ++executed;
    if (will_trap) break;
  }
  return executed;
}

uint32_t DynarmicArmCore::GetRegister(int index) const {
  return jit_->Regs()[static_cast<size_t>(index)];
}

void DynarmicArmCore::SetRegister(int index, uint32_t value) {
  jit_->Regs()[static_cast<size_t>(index)] = value;
}

uint32_t DynarmicArmCore::GetCpsr() const { return jit_->Cpsr(); }

void DynarmicArmCore::SetCpsr(uint32_t value) { jit_->SetCpsr(value); }

void DynarmicArmCore::SetCallOutRange(uint32_t base, uint32_t size) {
  call_out_base_ = base;
  call_out_size_ = size;
}

void DynarmicArmCore::SetCallOutHandler(CallOutHandler handler) {
  call_out_handler_ = std::move(handler);
}

void DynarmicArmCore::NotifyCodeChanged(uint32_t base, uint32_t size) {
  if (size == 0) {
    jit_->ClearCache();
  } else {
    jit_->InvalidateCacheRange(base, size);
  }
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
