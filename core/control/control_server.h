// core/control/control_server.h
//
// Out-of-band programmatic control channel for the game_probe runner.
//
// WHY: driving the emulator by synthesizing OS keystrokes into an Xvfb
// window is brittle (timing races, no readback, needs an X server). This
// gives an agent/script a real IPC surface instead: a line-oriented TCP
// server (NDJSON -- one JSON object per line, one response per line).
//
// THREADING CONTRACT (important): the ARM CPU, guest Memory, and all HLE
// objects are NOT thread-safe and are owned by the main tick loop. So the
// socket accept()/recv() runs on its OWN thread and only ever ENQUEUES
// requests. The main loop calls DrainInto() once per tick, executes each
// command inline (holding no other thread), and fulfills the request's
// std::promise. The network thread blocks on the future and writes the
// reply. Net effect: every command runs at a well-defined point in the
// tick, deterministically, with zero data races.
//
// Protocol (request -> response), newline-delimited:
//   {"cmd":"ping"}                       -> {"ok":true,"pong":true}
//   {"cmd":"press","button":"button2"}   -> {"ok":true}        (down+up)
//   {"cmd":"down","button":"up"}         -> {"ok":true}
//   {"cmd":"up","button":"up"}           -> {"ok":true}
//   {"cmd":"step","ticks":4}             -> {"ok":true,"tick":N}
//   {"cmd":"state"}                      -> {"ok":true,"tick":N,"pc":..,"running":true}
//   {"cmd":"reg","n":15}                 -> {"ok":true,"value":...}
//   {"cmd":"read","addr":..,"len":..}    -> {"ok":true,"hex":"...."}
//   {"cmd":"write","addr":..,"hex":"deadbeef"} -> {"ok":true,"addr":..,"len":N}
//   {"cmd":"setreg","n":0,"value":..}    -> {"ok":true,"n":..,"value":..}
//   {"cmd":"screenshot","path":"/tmp/x.ppm"} -> {"ok":true,"w":..,"h":..,"path":..}
//   {"cmd":"quit"}                       -> {"ok":true}         (stops the loop)
// Unknown / malformed -> {"ok":false,"error":"..."}.
//
// Button names: up down left right back button1 button2 button3 button4
// lshoulder rshoulder (mapped to real Zeebo Z-Pad HID UIDs by the caller).
//
// No external deps: POSIX sockets + a tiny hand-rolled JSON reader/writer
// sufficient for this flat, one-level protocol.

#ifndef ZEEBULATOR_CORE_CONTROL_CONTROL_SERVER_H_
#define ZEEBULATOR_CORE_CONTROL_CONTROL_SERVER_H_

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace zeebulator {

// A single parsed request plus the promise the main loop fulfills.
struct ControlRequest {
  std::string cmd;
  std::string button;    // for press/down/up
  std::string str_path;  // for screenshot
  std::string str_hex;   // for write (hex bytes payload)
  long i0 = 0;           // generic int arg (ticks / n / addr)
  long i1 = 0;           // generic int arg (len)
  long val = 0;          // explicit "value" arg (for setreg)
  bool has_i0 = false;
  bool has_i1 = false;
  bool has_val = false;
  std::promise<std::string> reply;  // main loop sets the JSON response line
};

class ControlServer {
 public:
  ControlServer() = default;
  ~ControlServer() { Stop(); }

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Binds 127.0.0.1:port and starts the accept thread. Returns false on
  // failure (port busy, etc.) with a message on stderr; the caller can
  // then just run without a control channel.
  bool Start(int port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      std::perror("[control] socket");
      return false;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);  // localhost only
    addr.sin_port = ::htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      std::perror("[control] bind");
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    if (::listen(listen_fd_, 4) < 0) {
      std::perror("[control] listen");
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    running_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    std::fprintf(stderr, "[control] listening on 127.0.0.1:%d\n", port);
    return true;
  }

  void Stop() {
    bool was = running_.exchange(false);
    if (!was) return;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
  }

  // Called by the main loop once per tick: moves every queued request out
  // under the lock, then the caller executes them and fulfills each
  // promise. Returns the batch (possibly empty).
  std::vector<std::shared_ptr<ControlRequest>> Drain() {
    std::vector<std::shared_ptr<ControlRequest>> out;
    std::lock_guard<std::mutex> lk(mu_);
    while (!queue_.empty()) {
      out.push_back(std::move(queue_.front()));
      queue_.pop();
    }
    return out;
  }

  bool IsRunning() const { return running_.load(); }

 private:
  void Enqueue(const std::shared_ptr<ControlRequest>& req) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push(req);
  }

  void AcceptLoop() {
    while (running_.load()) {
      sockaddr_in peer{};
      socklen_t plen = sizeof(peer);
      int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
      if (fd < 0) {
        if (!running_.load()) break;
        continue;
      }
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      // One connection handled inline (serial). Multiple concurrent
      // controllers aren't a use case here; a second connect just waits.
      HandleConnection(fd);
      ::close(fd);
    }
  }

  void HandleConnection(int fd) {
    std::string buf;
    char chunk[1024];
    while (running_.load()) {
      // Process any complete lines already buffered.
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string reply = Dispatch(line);
        reply.push_back('\n');
        if (!WriteAll(fd, reply)) return;
      }
      ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) return;  // peer closed or error
      buf.append(chunk, static_cast<size_t>(n));
    }
  }

  // Parses one request line, enqueues it, blocks on the main loop's
  // fulfillment, and returns the response line (without trailing '\n').
  std::string Dispatch(const std::string& line) {
    auto req = std::make_shared<ControlRequest>();
    if (!ParseLine(line, *req)) {
      return "{\"ok\":false,\"error\":\"parse\"}";
    }
    std::future<std::string> fut = req->reply.get_future();
    Enqueue(req);
    // Bounded wait so a wedged main loop can't hang the controller forever.
    if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
      return "{\"ok\":false,\"error\":\"timeout\"}";
    }
    return fut.get();
  }

  // --- Minimal JSON: only what this flat protocol needs ------------------
  static bool ParseLine(const std::string& s, ControlRequest& req) {
    req.cmd = ExtractString(s, "cmd");
    if (req.cmd.empty()) return false;
    req.button = ExtractString(s, "button");
    req.str_path = ExtractString(s, "path");
    req.str_hex = ExtractString(s, "hex");
    // Accept several int keys into i0 (first found wins) and len into i1.
    for (const char* k : {"ticks", "n", "addr", "port"}) {
      long v;
      if (ExtractInt(s, k, &v)) {
        req.i0 = v;
        req.has_i0 = true;
        break;
      }
    }
    long len;
    if (ExtractInt(s, "len", &len)) {
      req.i1 = len;
      req.has_i1 = true;
    }
    long value;
    if (ExtractInt(s, "value", &value)) {
      req.val = value;
      req.has_val = true;
    }
    return true;
  }

  static std::string ExtractString(const std::string& s, const char* key) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t k = s.find(pat);
    if (k == std::string::npos) return {};
    size_t c = s.find(':', k + pat.size());
    if (c == std::string::npos) return {};
    size_t q = s.find('"', c + 1);
    if (q == std::string::npos) return {};
    size_t e = s.find('"', q + 1);
    if (e == std::string::npos) return {};
    return s.substr(q + 1, e - q - 1);
  }

  // Accepts decimal or 0x-hex integer values.
  static bool ExtractInt(const std::string& s, const char* key, long* out) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t k = s.find(pat);
    if (k == std::string::npos) return false;
    size_t c = s.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t p = c + 1;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '"')) ++p;
    if (p >= s.size()) return false;
    int base = 10;
    if (p + 1 < s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) base = 16;
    char* end = nullptr;
    long v = std::strtol(s.c_str() + p, &end, base);
    if (end == s.c_str() + p) return false;
    *out = v;
    return true;
  }

  static bool WriteAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::mutex mu_;
  std::queue<std::shared_ptr<ControlRequest>> queue_;
};

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_CONTROL_SERVER_H_
