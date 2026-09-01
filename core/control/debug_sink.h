// core/control/debug_sink.h
//
// Global, thread-safe collection point for developer-facing debug data,
// consumed by the tabbed debug UI (see mirror_server.h). Modeled on how
// established emulator debuggers (Dolphin, PCSX2, mGBA) organize a debugger:
// a Log panel (including the game's own reported logs), a CPU/registers
// panel, a memory view, plus -- specific to this project -- the subsystem
// event traces already instrumented (BREW API calls, GPU draws, input,
// media).
//
// WHY a global singleton: the log helpers that already exist in several HLE
// modules (ZEEB_LOG_BREW/GPU/INPUT/MEDIA/FILE) each fprintf to stderr from
// deep inside call stacks. Threading a sink pointer through all of them would
// be invasive; a process-wide singleton they can push into is the minimal
// change. It is inert (near-zero cost) until Enable() is called, so it stays
// off in normal test/CI runs and only collects when the debug UI is on.
//
// THREADING: pushes come from the single tick thread (all HLE runs there);
// reads come from the HTTP serve thread. A mutex per structure keeps it safe.

#ifndef ZEEBULATOR_CORE_CONTROL_DEBUG_SINK_H_
#define ZEEBULATOR_CORE_CONTROL_DEBUG_SINK_H_

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace zeebulator {

// Category tags for the ring buffers the UI shows as separate tabs.
enum class DebugCat { kLog, kBrew, kGpu, kInput, kMedia };

// A snapshot of live CPU/loop state the UI's "CPU" tab renders.
struct DebugState {
  uint32_t regs[16] = {0};  // r0..r15 (r15 = pc)
  uint32_t cpsr = 0;
  uint64_t tick = 0;
  double fps = 0.0;
  bool running = false;
  bool valid = false;
};

class DebugSink {
 public:
  static DebugSink& Instance() {
    static DebugSink s;
    return s;
  }

  void Enable() { enabled_ = true; }
  bool Enabled() const { return enabled_; }

  // Push one line into a category ring buffer (bounded). No-op when disabled.
  void Push(DebugCat cat, const std::string& line) {
    if (!enabled_) return;
    std::deque<std::string>& q = QueueFor(cat);
    std::lock_guard<std::mutex> lk(mu_);
    q.push_back(line);
    if (q.size() > kMaxLines) q.pop_front();
    ++seq_;
  }

  // printf-style convenience.
  void Pushf(DebugCat cat, const char* fmt, ...) {
    if (!enabled_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Push(cat, buf);
  }

  // Snapshot copy of a category's lines (newest last), for the HTTP thread.
  std::deque<std::string> Snapshot(DebugCat cat) {
    std::lock_guard<std::mutex> lk(mu_);
    return QueueFor(cat);
  }

  void SetState(const DebugState& s) {
    std::lock_guard<std::mutex> lk(state_mu_);
    state_ = s;
    state_.valid = true;
  }
  DebugState State() {
    std::lock_guard<std::mutex> lk(state_mu_);
    return state_;
  }

  uint64_t Seq() const { return seq_; }

 private:
  DebugSink() = default;
  std::deque<std::string>& QueueFor(DebugCat cat) {
    switch (cat) {
      case DebugCat::kBrew: return brew_;
      case DebugCat::kGpu: return gpu_;
      case DebugCat::kInput: return input_;
      case DebugCat::kMedia: return media_;
      case DebugCat::kLog:
      default: return log_;
    }
  }

  static constexpr size_t kMaxLines = 500;
  bool enabled_ = false;
  std::mutex mu_;
  std::deque<std::string> log_, brew_, gpu_, input_, media_;
  std::mutex state_mu_;
  DebugState state_;
  uint64_t seq_ = 0;
};

// Free helpers so call sites stay terse.
inline void DebugLog(DebugCat cat, const std::string& line) {
  DebugSink::Instance().Push(cat, line);
}

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_DEBUG_SINK_H_
