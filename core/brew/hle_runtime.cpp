#include "core/brew/hle_runtime.h"

#include "core/control/call_stack_tracer.h"
#include "core/control/debug_sink.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <set>
#include <string>
#include <algorithm>
#include <vector>
#include <map>

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
  labels_.push_back(label);
  uint32_t addr = trap_base_ + index * 4;
  if (!label.empty()) {
    CallStackTracer::Instance().RegisterSymbol(addr, label);
  }
  return addr;
}

std::string HleRuntime::LabelForAddress(uint32_t sentinel_address) const {
  if (sentinel_address < trap_base_) return {};
  uint32_t index = (sentinel_address - trap_base_) / 4;
  if (index < labels_.size()) return labels_[index];
  return {};
}

namespace {
struct HleProfileEntry {
  uint64_t calls = 0;
  uint64_t zero_returns = 0;
  std::string label;
};
std::map<uint64_t, HleProfileEntry>& HleProfile() {
  static std::map<uint64_t, HleProfileEntry> m;
  return m;
}
// Guardados no primeiro dispatch perfilado para que o dump consiga
// auto-rotular cada trap: varrendo a regiao de vtables da HLE em memoria do
// guest, o endereco onde o valor do trap aparece E o (vtable + slot*4) que o
// jogo chamou. Assim o perfil sai como "vt=0x80002000 slot=7" em vez de um
// indice opaco -- e a tecnica de vtable slot fingerprinting de
// zeebo-lle/notes/MORE_INFO.md 5.4, so que resolvida pela propria ferramenta.
Memory* g_profile_mem = nullptr;
uint32_t g_profile_trap_base = 0;
std::string ResolveTrapSlot(uint32_t trap_addr) {
  if (g_profile_mem == nullptr) return {};
  for (uint32_t a = 0x80000000u; a < 0x800A0000u; a += 4) {
    if (g_profile_mem->Read32(a) == trap_addr) {
      char b[64];
      std::snprintf(b, sizeof(b), "vt~0x%08x slot=%u", a & ~0xFFFu, (a & 0xFFFu) / 4);
      return b;
    }
  }
  return {};
}
// Prints the profile once, at process exit, so a run that is killed by a
// step budget still reports.
void DumpHleProfile() {
  static bool done = false;
  if (done) return;
  done = true;
  {
    if (std::getenv("ZEEB_HLE_PROFILE") == nullptr || HleProfile().empty()) return;
    std::vector<std::pair<uint64_t, HleProfileEntry>> v(HleProfile().begin(), HleProfile().end());
    std::fprintf(stderr, "\n===== HLE PROFILE (slot x call-site) =====\n");
    std::fprintf(stderr, "-- chamados EXATAMENTE UMA VEZ (candidatos a gate de init) --\n");
    for (const auto& [k, e] : v) {
      if (e.calls != 1) continue;
      std::fprintf(stderr, "  idx=%-4u lr=0x%08x ret0=%llu  %s\n",
                   static_cast<uint32_t>(k >> 32), static_cast<uint32_t>(k),
                   static_cast<unsigned long long>(e.zero_returns), e.label.c_str());
    }
    std::fprintf(stderr, "-- que SEMPRE devolveram 0 (chamador tende a `CMP R0,#0; BEQ`) --\n");
    for (const auto& [k, e] : v) {
      if (e.zero_returns == 0 || e.zero_returns != e.calls) continue;
      std::fprintf(stderr, "  idx=%-4u lr=0x%08x calls=%llu  %s\n",
                   static_cast<uint32_t>(k >> 32), static_cast<uint32_t>(k),
                   static_cast<unsigned long long>(e.calls), e.label.c_str());
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
      return a.second.calls > b.second.calls;
    });
    std::fprintf(stderr, "-- 15 mais chamados --\n");
    for (size_t i = 0; i < v.size() && i < 15; ++i) {
      std::fprintf(stderr, "  idx=%-4u lr=0x%08x calls=%llu  %s\n",
                   static_cast<uint32_t>(v[i].first >> 32), static_cast<uint32_t>(v[i].first),
                   static_cast<unsigned long long>(v[i].second.calls), v[i].second.label.c_str());
    }
    std::fprintf(stderr, "===== fim do HLE PROFILE =====\n");
  }
}

// O dump precisa sair tambem quando a corrida e encerrada por SIGTERM/SIGINT
// (o caso comum: `timeout N ...` no harness). Destrutor estatico sozinho nao
// basta -- SIGTERM nao desenrola a pilha.
void HleProfileSignalHandler(int sig) {
  DumpHleProfile();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
struct HleProfileDumper {
  HleProfileDumper() {
    if (std::getenv("ZEEB_HLE_PROFILE") != nullptr) {
      std::signal(SIGTERM, HleProfileSignalHandler);
      std::signal(SIGINT, HleProfileSignalHandler);
    }
  }
  ~HleProfileDumper() { DumpHleProfile(); }
};
HleProfileDumper g_hle_profile_dumper;
}  // namespace

void HleRuntime::Dispatch(IArmCore& core, uint32_t address) {
  uint32_t index = (address - trap_base_) / 4;
  if (index == kReturnSentinelSlot) {
    // Leave PC exactly where it is; CallArmFunction's loop below detects
    // this by PC value, not here.
    return;
  }
  if (index < functions_.size() && functions_[index]) {
    std::string label = LabelForAddress(address);
    CallStackTracer::Instance().OnCall(core.GetRegister(kPC), address, core.GetRegister(kLR), core.GetRegister(kSP));
    // Env-gated full BREW-API trace (ZEEB_LOG_BREW=1): every IMPLEMENTED
    // vtable slot a title invokes, with its label and args. Complements
    // ZEEB_LOG_SLOT (which only fires for UNimplemented slots). Off by
    // default; stderr only; non-perturbing (logs, then runs the real handler).
    if (std::getenv("ZEEB_LOG_BREW") != nullptr ||
        ::zeebulator::DebugSink::Instance().Enabled()) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "%s idx=%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x",
                    label.empty() ? "(unlabeled)" : label.c_str(), index,
                    core.GetRegister(kR0), core.GetRegister(kR1),
                    core.GetRegister(kR2), core.GetRegister(kR3), core.GetRegister(kLR));
      ::zeebulator::DebugLog(::zeebulator::DebugCat::kBrew, buf);
      if (std::getenv("ZEEB_LOG_BREW") != nullptr)
        std::fprintf(stderr, "[brew] %s\n", buf);
    }
    // ZEEB_HLE_PROFILE=1 -- frequency + call-site profile of every IMPLEMENTED
    // slot, with the value each call returned. Rationale (zeebo-lle
    // notes/MORE_INFO.md 5.4, "stub logging por frequencia + call-site"):
    // a full per-call trace floods and hides the signal, while what actually
    // locates a boot wall is (a) slots called EXACTLY ONCE during boot --
    // high chance of being an initialization gate -- and (b) slots that hand
    // back 0/NULL to a caller that immediately does `CMP R0,#0; BEQ`. Keyed
    // by (slot, caller LR) so the same handler reached from two different
    // sites stays distinguishable. Summary is printed at exit.
    static const bool profile = std::getenv("ZEEB_HLE_PROFILE") != nullptr;
    if (profile) {
      uint32_t caller = core.GetRegister(kLR);
      g_profile_mem = &core.GetMemory();
      g_profile_trap_base = trap_base_;
      auto& e = HleProfile()[(static_cast<uint64_t>(index) << 32) | caller];
      ++e.calls;
      if (e.calls == 1) {  // resolve o rotulo UMA vez por (slot, call-site):
        e.label = LabelForAddress(address);   // a varredura de vtable e cara
        if (e.label.empty()) e.label = ResolveTrapSlot(address);
      }
      functions_[index](core);
      uint32_t ret = core.GetRegister(kR0);
      if (ret == 0) ++e.zero_returns;
      CallStackTracer::Instance().OnReturn(address, core.GetRegister(kLR), core.GetRegister(kSP));
      core.BranchExchange(core.GetRegister(kLR));
      return;
    }
    functions_[index](core);
    CallStackTracer::Instance().OnReturn(address, core.GetRegister(kLR), core.GetRegister(kSP));
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
  core.BranchExchange(core.GetRegister(kLR));  // simulate BX LR with proper ARM/Thumb interworking
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
