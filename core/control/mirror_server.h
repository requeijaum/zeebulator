// core/control/mirror_server.h
//
// Loopback HTTP screen-mirror for the game_probe runner.
//
// WHY: the native SDL2 window already shows the emulator live on the host's
// Wayland/X session -- that's what a human watches. This adds a SECOND view
// of the exact same composited frames over HTTP so an out-of-band agent (or a
// browser tab) can see what's on screen without VNC, X11 grabbing, or any
// external dep. Chosen over a real VNC/RFB server because RFB needs a full
// framebuffer-protocol state machine + a client; a tiny HTTP endpoint serving
// PNG frames is trivially consumable by any browser and by an image-capable
// agent, with zero libraries.
//
// THREADING: the main tick loop (which owns the GL context) calls
// PublishFrame(rgba,w,h) every few ticks; that encodes a PNG and stores it
// under a mutex. The HTTP accept/serve runs on its own thread and only reads
// the latest stored PNG -- it never touches GL or guest state.
//
// Routes (GET):
//   /            -> minimal HTML page that auto-refreshes <img> ~10 fps
//   /frame.png   -> latest composited frame as PNG (image/png)
//   /status      -> {"w":..,"h":..,"frames":..}
//
// PNG is emitted with STORED (uncompressed) DEFLATE blocks -- no zlib needed,
// valid PNG, a bit larger on the wire but this is a localhost mirror.

#ifndef ZEEBULATOR_CORE_CONTROL_MIRROR_SERVER_H_
#define ZEEBULATOR_CORE_CONTROL_MIRROR_SERVER_H_

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeebulator {

// --- Minimal PNG encoder (RGBA8 in, PNG out; STORED DEFLATE) --------------
namespace png_detail {

inline uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[n] = c;
    }
    init = true;
  }
  crc = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

inline uint32_t Adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

inline void PutU32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

inline void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  PutU32(out, static_cast<uint32_t>(data.size()));
  size_t crc_start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  uint32_t crc = Crc32(out.data() + crc_start, out.size() - crc_start);
  PutU32(out, crc);
}

}  // namespace png_detail

// Encodes width*height RGBA8 (row 0 = top) as a PNG byte stream.
inline std::vector<uint8_t> EncodePng(const uint8_t* rgba, int width, int height) {
  using namespace png_detail;
  // Build raw scanlines with a 0 (None) filter byte per row.
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: None
    raw.insert(raw.end(), rgba + static_cast<size_t>(y) * stride,
               rgba + static_cast<size_t>(y) * stride + stride);
  }
  // zlib stream: header (0x78 0x01) + STORED deflate blocks + adler32.
  std::vector<uint8_t> zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    size_t block = raw.size() - off;
    if (block > 65535) block = 65535;
    bool final = (off + block) >= raw.size();
    zlib.push_back(final ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
    uint16_t len = static_cast<uint16_t>(block);
    uint16_t nlen = static_cast<uint16_t>(~len);
    zlib.push_back(static_cast<uint8_t>(len & 0xFF));
    zlib.push_back(static_cast<uint8_t>(len >> 8));
    zlib.push_back(static_cast<uint8_t>(nlen & 0xFF));
    zlib.push_back(static_cast<uint8_t>(nlen >> 8));
    zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + block);
    off += block;
  }
  uint32_t adler = Adler32(raw.data(), raw.size());
  PutU32(zlib, adler);

  std::vector<uint8_t> out;
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  out.insert(out.end(), sig, sig + 8);
  std::vector<uint8_t> ihdr;
  PutU32(ihdr, static_cast<uint32_t>(width));
  PutU32(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);   // bit depth
  ihdr.push_back(6);   // color type 6 = RGBA
  ihdr.push_back(0);   // compression
  ihdr.push_back(0);   // filter
  ihdr.push_back(0);   // interlace
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", zlib);
  Chunk(out, "IEND", {});
  return out;
}

// --- Mirror HTTP server ---------------------------------------------------
class MirrorServer {
 public:
  MirrorServer() = default;
  ~MirrorServer() { Stop(); }
  MirrorServer(const MirrorServer&) = delete;
  MirrorServer& operator=(const MirrorServer&) = delete;

  bool Start(int port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 8) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { ServeLoop(); });
    std::fprintf(stderr, "[mirror] serving on http://127.0.0.1:%d/\n", port);
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
  }

  bool IsRunning() const { return running_.load(); }

  // Called from the main (GL-owning) thread. Encodes and stores the latest
  // frame; the HTTP thread just serves whatever is stored.
  void PublishFrame(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> png = EncodePng(rgba, width, height);
    std::lock_guard<std::mutex> lk(mu_);
    latest_png_ = std::move(png);
    w_ = width;
    h_ = height;
    ++frames_;
  }

 private:
  void ServeLoop() {
    while (running_.load()) {
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (!running_.load()) break;
        continue;
      }
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      HandleClient(fd);
      ::close(fd);
    }
  }

  void HandleClient(int fd) {
    char buf[2048];
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    // Parse "GET <path> ..."
    std::string path = "/";
    if (std::strncmp(buf, "GET ", 4) == 0) {
      const char* p = buf + 4;
      const char* sp = std::strchr(p, ' ');
      if (sp) path.assign(p, sp - p);
    }
    if (path == "/frame.png") {
      std::vector<uint8_t> png;
      {
        std::lock_guard<std::mutex> lk(mu_);
        png = latest_png_;
      }
      if (png.empty()) {
        SendText(fd, "503 Service Unavailable", "text/plain", "no frame yet\n");
      } else {
        SendBinary(fd, "image/png", png);
      }
    } else if (path == "/status") {
      std::string body;
      {
        std::lock_guard<std::mutex> lk(mu_);
        body = "{\"w\":" + std::to_string(w_) + ",\"h\":" + std::to_string(h_) +
               ",\"frames\":" + std::to_string(frames_) + "}";
      }
      SendText(fd, "200 OK", "application/json", body);
    } else {
      SendText(fd, "200 OK", "text/html", IndexHtml());
    }
  }

  static std::string IndexHtml() {
    return
        "<!doctype html><html><head><meta charset=utf-8><title>Zeebulator mirror</title>"
        "<style>body{margin:0;background:#111;display:flex;justify-content:center;"
        "align-items:center;height:100vh}img{image-rendering:pixelated;max-width:100vw;"
        "max-height:100vh}</style></head><body><img id=v src=/frame.png>"
        "<script>setInterval(()=>{document.getElementById('v').src='/frame.png?t='+Date.now()"
        "},100)</script></body></html>";
  }

  static bool WriteAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  void SendText(int fd, const char* status, const char* ctype, const std::string& body) {
    std::string hdr = "HTTP/1.1 " + std::string(status) + "\r\nContent-Type: " + ctype +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
    WriteAll(fd, hdr.data(), hdr.size());
    WriteAll(fd, body.data(), body.size());
  }

  void SendBinary(int fd, const char* ctype, const std::vector<uint8_t>& body) {
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(ctype) +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
    WriteAll(fd, hdr.data(), hdr.size());
    WriteAll(fd, reinterpret_cast<const char*>(body.data()), body.size());
  }

  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mu_;
  std::vector<uint8_t> latest_png_;
  int w_ = 0, h_ = 0;
  uint64_t frames_ = 0;
};

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_MIRROR_SERVER_H_
