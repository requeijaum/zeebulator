#include "core/cpu/dynarmic_arm_core.h"
#include "core/control/debug_hooks.h"

#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"

namespace zeebulator {

// No-op coprocessor: the reference interpreter THROWS UnimplementedInstruction
// when a coprocessor op actually executes (it never does at runtime for the
// titles we boot). But dynarmic compiles whole blocks ahead of the PC, so a
// coprocessor opcode sitting anywhere inside a compiled block (often data or
// never-reached code past a forced/optional branch) makes the x64 backend hit
// `ASSERT_FALSE("Should raise coproc exception here")` at COMPILE time and
// abort the process. Registering a benign coprocessor makes the backend emit a
// harmless callback/scratch access instead of asserting, so the JIT can compile
// past such opcodes; if one were ever truly executed it is a silent no-op,
// which is strictly safer than crashing and matches "coproc is inert" on the
// userspace BREW target. Reads yield 0 via a shared scratch word.
class NopCoprocessor final : public Dynarmic::A32::Coprocessor {
 public:
  using Coprocessor = Dynarmic::A32::Coprocessor;
  using CoprocReg = Dynarmic::A32::CoprocReg;
  static std::uint64_t NopFn(void*, std::uint32_t, std::uint32_t) { return 0; }

  std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg,
                                                   CoprocReg, CoprocReg,
                                                   unsigned) override {
    return Callback{&NopFn, std::nullopt};
  }
  CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg,
                                             CoprocReg, unsigned) override {
    return Callback{&NopFn, std::nullopt};
  }
  CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned,
                                               CoprocReg) override {
    return Callback{&NopFn, std::nullopt};
  }
  CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, CoprocReg,
                                            CoprocReg, unsigned) override {
    return &scratch_;
  }
  CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned,
                                              CoprocReg) override {
    return std::array<std::uint32_t*, 2>{&scratch_, &scratch2_};
  }
  std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg,
                                           std::optional<std::uint8_t>) override {
    return Callback{&NopFn, std::nullopt};
  }
  std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg,
                                            std::optional<std::uint8_t>) override {
    return Callback{&NopFn, std::nullopt};
  }

 private:
  std::uint32_t scratch_ = 0;
  std::uint32_t scratch2_ = 0;
};


// Bridges the dynarmic JIT to our sparse Memory. Reads/writes are composed
// from byte accesses so unaligned access (which dynarmic explicitly allows)
// is always correct and matches how the interpreter's Memory behaves.
struct DynarmicArmCore::Callbacks final : Dynarmic::A32::UserCallbacks {
  Memory* memory = nullptr;
  Dynarmic::A32::Jit* jit = nullptr;  // for self-modifying-code invalidation
  std::uint64_t ticks_left = 0;
  std::uint32_t call_out_base = 0;
  std::uint32_t call_out_size = 0;
  bool trap_faulted = false;  // set when a NoExecuteFault halted the block
  // Pages (4 KiB) we have executed code from — the only pages where a guest
  // store can invalidate a cached block.
  std::unordered_set<std::uint32_t> code_pages;
  bool code_pages_enabled = true;
  bool IsExecutedPage(std::uint32_t a) const {
    return code_pages.count(a >> 12) != 0;
  }

  bool InTrap(std::uint32_t a) const {
    return call_out_size != 0 && a >= call_out_base &&
           a < call_out_base + call_out_size;
  }
  // Code fetch inside the call-out range must NOT execute: returning nullopt
  // makes dynarmic halt the block before running it (config.h: "Attempted to
  // execute a code block at an address for which MemoryReadCode returned
  // std::nullopt"). This is the block-mode equivalent of the interpreter's
  // pre-fetch trap check, and it stops Jit::Run() exactly at the trap PC.
  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t a) override {
    if (InTrap(a)) return std::nullopt;
    if (code_pages_enabled) code_pages.insert(a >> 12);
    return MemoryRead32(a);
  }

  std::uint8_t MemoryRead8(std::uint32_t a) override { return memory->Read8(a); }
  std::uint16_t MemoryRead16(std::uint32_t a) override {
    return std::uint16_t(MemoryRead8(a)) | std::uint16_t(MemoryRead8(a + 1)) << 8;
  }
  std::uint32_t MemoryRead32(std::uint32_t a) override {
    // Delegate to Memory::Read32 so backend-independent read logic (e.g. the
    // persistent +0x63c optional-callback seed) applies under the JIT too.
    // Code fetches go through MemoryReadCode below, which is fine: the seed
    // only rewrites a specific BSS data word that is never fetched as code.
    return memory->Read32(a);
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
    // Only invalidate when the store lands in a page we've actually executed
    // from. Blind invalidate-on-every-store defeats the block cache for the
    // common case (writing DATA, not code) and made block mode slower than the
    // interpreter. Correctness is preserved: a write to a never-executed page
    // can't have a cached block, and the first execution of any page compiles
    // fresh; a write to an executed (code) page still invalidates. SMC into a
    // page that is both written and executed is covered because that page is
    // marked executed on its first run.
    if (jit && IsExecutedPage(a)) jit->InvalidateCacheRange(a, 1);
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

  // ARM semihosting mirrors ArmInterpreter::ExecuteSwi exactly for the
  // supported Angel operations. Dynarmic invokes this from generated code, so
  // replicate the observable R0/memory/log behavior here rather than discard it.
  void CallSVC(std::uint32_t /*swi*/) override {
    if (jit == nullptr || memory == nullptr) return;
    uint32_t op = jit->Regs()[kR0];
    uint32_t arg = jit->Regs()[kR1];
    if (op == 0x03) {  // SYS_WRITEC
      if (std::getenv("ZEEB_LOG_SEMIHOSTING")) {
        std::fprintf(stderr, "%c", static_cast<char>(memory->Read8(arg)));
      }
    } else if (op == 0x04) {  // SYS_WRITE0
      if (std::getenv("ZEEB_LOG_SEMIHOSTING")) {
        std::string text;
        for (uint32_t i = 0; i < 4096; ++i) {
          char c = static_cast<char>(memory->Read8(arg + i));
          if (c == 0) break;
          text.push_back(c);
        }
        std::fprintf(stderr, "%s", text.c_str());
      }
    }
    jit->Regs()[kR0] = 0;  // semihosting success, same as ArmInterpreter
  }
  void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override {
    // MemoryReadCode returning nullopt in the trap range makes the frontend
    // emit a NoExecuteFault exception at the trap PC. That is our block-mode
    // call-out signal: halt the JIT so Jit::Run() returns with PC parked at the
    // trap address, exactly where the interpreter's pre-fetch check would stop.
    if (e == Dynarmic::A32::Exception::NoExecuteFault && jit) {
      trap_faulted = true;
      jit->HaltExecution();
    }
    (void)pc;
  }
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
  // Opt-in benign coprocessor so the block compiler doesn't ASSERT/abort on a
  // coprocessor opcode embedded in a compiled-ahead block (see NopCoprocessor).
  // Gated to keep default/test behavior byte-identical unless requested.
  if (const char* c = std::getenv("ZEEB_NOP_COPROC")) {
    if (c[0] == '1') {
      auto nop = std::make_shared<NopCoprocessor>();
      for (auto& slot : cfg.coprocessors) slot = nop;
    }
  }
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
  callbacks_->code_pages.clear();
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
  // Keep the debug contract identical to ArmInterpreter::Step: breakpoints,
  // trace and writer-PC attribution observe the guest instruction before it
  // executes. Memory watches already flow through MemoryRead*/MemoryWrite*.
  DebugHooks::Instance().OnExec(fetch_addr, *this);
  // Keep the tick budget non-empty so cycle counting never halts a single
  // step early; Jit::Step() executes exactly one instruction regardless.
  callbacks_->ticks_left = 1;
  jit_->Step();
}

uint64_t DynarmicArmCore::Run(uint64_t max_instructions) {
  // Block execution: this is where the JIT actually pays off. Instead of
  // recompiling one instruction per Step(), we let dynarmic run whole compiled
  // blocks via Jit::Run() until either the instruction budget is exhausted or
  // the PC reaches the call-out trap (MemoryReadCode returns nullopt there,
  // halting the block exactly at the trap PC). Semantics stay identical to the
  // interpreter's Run: same instruction count, same trap-then-handler order.
  uint64_t executed = 0;
  while (executed < max_instructions) {
    const uint32_t fetch_addr = jit_->Regs()[15];
    if (IsCallOutAddress(fetch_addr)) {
      if (call_out_handler_) call_out_handler_(*this, fetch_addr);
      ++executed;  // the trap counts as one step, mirroring Step()
      break;       // caller re-enters after servicing the call-out
    }
    const uint64_t budget = max_instructions - executed;
    callbacks_->ticks_left = budget;
    callbacks_->trap_faulted = false;
    jit_->Run();
    // AddTicks drained ticks_left by the number of instructions executed.
    uint64_t ran = budget - callbacks_->ticks_left;
    if (callbacks_->trap_faulted) {
      // The block halted on a NoExecuteFault: dynarmic counted the faulting
      // trap fetch as one cycle and advanced PC past it. Undo both so the
      // observable state matches the interpreter's pre-fetch stop: PC parked
      // AT the trap address, and that fetch NOT counted as an executed
      // instruction. Instruction width is 2 in Thumb state, else 4.
      const uint32_t step = (jit_->Cpsr() & (1u << 5)) ? 2u : 4u;
      jit_->Regs()[15] -= step;
      if (ran > 0) --ran;
      executed += ran;
      // Loop top will now see the trap PC and dispatch the handler.
      continue;
    }
    executed += ran;
    if (ran == 0) {
      // No forward progress (e.g. immediately at a trap fetch that the loop
      // top will service, or a zero-budget edge) — fall back to one Step to
      // guarantee termination and exact single-instruction semantics.
      Step();
      ++executed;
      if (IsCallOutAddress(jit_->Regs()[15])) break;
    }
  }
  return executed;
}

uint32_t DynarmicArmCore::GetRegister(int index) const {
  return jit_->Regs()[static_cast<size_t>(index)];
}

void DynarmicArmCore::SetRegister(int index, uint32_t value) {
  jit_->Regs()[static_cast<size_t>(index)] = value;
}

void DynarmicArmCore::BranchExchange(uint32_t target) {
  bool to_thumb = (target & 1) != 0;
  uint32_t cpsr = jit_->Cpsr();
  if (to_thumb) {
    cpsr |= (1u << kCpsrT);
  } else {
    cpsr &= ~(1u << kCpsrT);
  }
  jit_->SetCpsr(cpsr);
  jit_->Regs()[15] = to_thumb ? (target & ~1u) : (target & ~3u);
}

uint32_t DynarmicArmCore::GetCpsr() const { return jit_->Cpsr(); }

void DynarmicArmCore::SetCpsr(uint32_t value) { jit_->SetCpsr(value); }

void DynarmicArmCore::SetCallOutRange(uint32_t base, uint32_t size) {
  call_out_base_ = base;
  call_out_size_ = size;
  callbacks_->call_out_base = base;
  callbacks_->call_out_size = size;
  // A change to the trap range can change which fetches halt, so any cached
  // block covering the old/new range must be recompiled.
  jit_->ClearCache();
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
