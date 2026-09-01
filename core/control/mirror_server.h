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
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/control/debug_sink.h"

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
    encoder_thread_ = std::thread([this] { EncodeLoop(); });
    thread_ = std::thread([this] { ServeLoop(); });
    std::fprintf(stderr, "[mirror] serving on http://127.0.0.1:%d/\n", port);
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    // Wake the encoder thread so it can observe running_==false and exit.
    raw_cv_.notify_all();
    if (encoder_thread_.joinable()) encoder_thread_.join();
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
  }

  bool IsRunning() const { return running_.load(); }

  // Optional guest-memory reader, set by the runner (game_probe). Given an
  // address and length it returns a lowercase hex string of that many bytes.
  // Called from the HTTP thread for the /api/mem route; the read is a
  // best-effort live peek (Memory::Read8 is a const page lookup), matching
  // the mirror's latest-wins, no-guest-state-mutation contract.
  void SetMemReader(std::function<std::string(uint32_t, uint32_t)> fn) {
    mem_reader_ = std::move(fn);
  }

  // Called from the main (GL-owning) thread. Stashes the latest raw RGBA
  // frame (a cheap copy) and wakes the encoder thread. The expensive PNG
  // encode (CRC32/Adler32/DEFLATE over ~1.2 MB) runs OFF this thread so it
  // never steals wall-clock from the emulation loop -- that PNG encode inline
  // here was measured to drop Double Dragon from 62 to ~55 fps. Stale frames
  // are dropped: if the encoder is still busy, the previous pending frame is
  // simply overwritten (mirror is best-effort, latest-wins).
  void PublishFrame(const uint8_t* rgba, int width, int height) {
    size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    {
      std::lock_guard<std::mutex> lk(raw_mu_);
      pending_raw_.assign(rgba, rgba + bytes);
      pending_w_ = width;
      pending_h_ = height;
      pending_ready_ = true;
    }
    raw_cv_.notify_one();
  }

 private:
  // Dedicated encoder thread: waits for a raw frame, encodes PNG, publishes.
  void EncodeLoop() {
    std::vector<uint8_t> raw;
    int w = 0, h = 0;
    while (running_.load()) {
      {
        std::unique_lock<std::mutex> lk(raw_mu_);
        raw_cv_.wait(lk, [this] { return pending_ready_ || !running_.load(); });
        if (!running_.load()) break;
        raw.swap(pending_raw_);
        w = pending_w_;
        h = pending_h_;
        pending_ready_ = false;
      }
      if (raw.empty() || w <= 0 || h <= 0) continue;
      std::vector<uint8_t> png = EncodePng(raw.data(), w, h);
      std::lock_guard<std::mutex> lk(mu_);
      latest_png_ = std::move(png);
      w_ = w;
      h_ = h;
      ++frames_;
    }
  }

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
    } else if (path == "/debug") {
      SendText(fd, "200 OK", "text/html", DebugHtml());
    } else if (path == "/api/state") {
      SendText(fd, "200 OK", "application/json", StateJson());
    } else if (path.rfind("/api/log", 0) == 0) {
      // /api/log?cat=log|brew|gpu|input|media
      std::string cat = "log";
      auto q = path.find("cat=");
      if (q != std::string::npos) cat = path.substr(q + 4);
      SendText(fd, "200 OK", "application/json", LogJson(cat));
    } else if (path.rfind("/api/mem", 0) == 0) {
      // /api/mem?addr=<hex-or-dec>&len=<n> -> {"addr":..,"len":..,"hex":".."}
      uint32_t addr = 0, len = 64;
      auto pa = path.find("addr=");
      if (pa != std::string::npos) addr = static_cast<uint32_t>(std::strtoul(path.c_str() + pa + 5, nullptr, 0));
      auto pl = path.find("len=");
      if (pl != std::string::npos) len = static_cast<uint32_t>(std::strtoul(path.c_str() + pl + 4, nullptr, 0));
      if (len == 0) len = 1;
      if (len > 4096) len = 4096;  // cap one request
      std::string hex = mem_reader_ ? mem_reader_(addr, len) : std::string();
      char hb[64];
      std::snprintf(hb, sizeof(hb), "{\"addr\":%u,\"len\":%u,\"hex\":\"", addr, len);
      SendText(fd, "200 OK", "application/json", std::string(hb) + hex + "\"}");
    } else {
      SendText(fd, "200 OK", "text/html", IndexHtml());
    }
  }

  static std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
      switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
          } else {
            o += c;
          }
      }
    }
    return o;
  }

  static DebugCat CatFromName(const std::string& name) {
    if (name.rfind("brew", 0) == 0) return DebugCat::kBrew;
    if (name.rfind("gpu", 0) == 0) return DebugCat::kGpu;
    if (name.rfind("input", 0) == 0) return DebugCat::kInput;
    if (name.rfind("media", 0) == 0) return DebugCat::kMedia;
    return DebugCat::kLog;
  }

  static std::string LogJson(const std::string& cat) {
    auto lines = DebugSink::Instance().Snapshot(CatFromName(cat));
    std::string body = "{\"cat\":\"" + JsonEscape(cat) + "\",\"lines\":[";
    bool first = true;
    for (const auto& l : lines) {
      if (!first) body += ",";
      first = false;
      body += "\"" + JsonEscape(l) + "\"";
    }
    body += "]}";
    return body;
  }

  static std::string StateJson() {
    DebugState s = DebugSink::Instance().State();
    if (!s.valid) return "{\"valid\":false}";
    char b[1024];
    int off = std::snprintf(b, sizeof(b),
                            "{\"valid\":true,\"tick\":%llu,\"fps\":%.1f,\"running\":%s,\"cpsr\":%u,\"regs\":[",
                            static_cast<unsigned long long>(s.tick), s.fps,
                            s.running ? "true" : "false", s.cpsr);
    std::string body(b, off);
    for (int i = 0; i < 16; ++i) {
      if (i) body += ",";
      body += std::to_string(s.regs[i]);
    }
    body += "]}";
    return body;
  }

  // Tabbed debug UI. Tabs mirror what established emulator debuggers surface
  // (Dolphin/PCSX2/mGBA): Screen, CPU registers, and per-subsystem logs.
  static std::string DebugHtml() {
    return
        "<!doctype html><html><head><meta charset=utf-8><title>Zeebulator debug</title>"
        "<style>"
        "body{margin:0;font:13px monospace;background:#151515;color:#ddd}"
        "#tabs{display:flex;background:#222;border-bottom:1px solid #000}"
        "#tabs button{background:#222;color:#aaa;border:0;padding:8px 14px;cursor:pointer}"
        "#tabs button.on{background:#151515;color:#fff;border-top:2px solid #6cf}"
        "#panes>div{display:none;padding:10px}"
        "#panes>div.on{display:block}"
        "pre{margin:0;white-space:pre-wrap;word-break:break-all;max-height:82vh;overflow:auto}"
        "img{image-rendering:pixelated;max-width:100%;background:#000}"
        "table{border-collapse:collapse}td{padding:2px 10px;border:1px solid #333}"
        ".k{color:#6cf}.hdr{color:#8f8}"
        "</style></head><body>"
        "<div id=tabs>"
        "<button data-t=screen class=on>Screen</button>"
        "<button data-t=cpu>CPU</button>"
        "<button data-t=brew>BREW API</button>"
        "<button data-t=gpu>GPU</button>"
        "<button data-t=input>Input</button>"
        "<button data-t=media>Media</button>"
        "<button data-t=mem>Memory</button>"
        "<button data-t=log>Log</button>"
        "</div><div id=panes>"
        "<div id=screen class=on><img id=v src=/frame.png><div id=sstat class=hdr></div></div>"
        "<div id=cpu><div id=cpubox></div></div>"
        "<div id=brew><pre id=pbrew></pre></div>"
        "<div id=gpu><pre id=pgpu></pre></div>"
        "<div id=input><pre id=pinput></pre></div>"
        "<div id=media><pre id=pmedia></pre></div>"
        "<div id=mem><div class=hdr>addr <input id=maddr value=0x80200000 size=12> "
        "len <input id=mlen value=256 size=5> <button id=mgo>read</button> "
        "<label><input type=checkbox id=mauto> auto</label></div>"
        "<pre id=pmem></pre></div>"
        "<div id=log><pre id=plog></pre></div>"
        "</div>"
        "<script>"
        "let cur='screen';"
        "document.querySelectorAll('#tabs button').forEach(b=>b.onclick=()=>{"
        "cur=b.dataset.t;"
        "document.querySelectorAll('#tabs button').forEach(x=>x.classList.toggle('on',x==b));"
        "document.querySelectorAll('#panes>div').forEach(d=>d.classList.toggle('on',d.id==cur));"
        "});"
        "const RN=['r0','r1','r2','r3','r4','r5','r6','r7','r8','r9','r10','r11','r12','sp','lr','pc'];"
        "function hx(n){return '0x'+(n>>>0).toString(16).padStart(8,'0')}"
        "function memDump(base,hex){let o='';for(let i=0;i<hex.length/2;i+=16){"
        "let a=(base+i)>>>0;o+=a.toString(16).padStart(8,'0')+'  ';let asc='';"
        "for(let j=0;j<16;j++){if(i+j<hex.length/2){let b=parseInt(hex.substr((i+j)*2,2),16);"
        "o+=hex.substr((i+j)*2,2)+' ';asc+=(b>=32&&b<127)?String.fromCharCode(b):'.';}else{o+='   ';}}"
        "o+=' '+asc+'\\n';}return o;}"
        "async function readMem(){let a=parseInt(document.getElementById('maddr').value)>>>0;"
        "let l=parseInt(document.getElementById('mlen').value)||256;"
        "try{let r=await(await fetch('/api/mem?addr='+a+'&len='+l)).json();"
        "document.getElementById('pmem').textContent=memDump(r.addr,r.hex);}catch(e){}}"
        "document.getElementById('mgo').onclick=readMem;"
        "async function tickUI(){"
        " if(cur=='screen'){document.getElementById('v').src='/frame.png?t='+Date.now();"
        "  try{let s=await(await fetch('/status')).json();"
        "   document.getElementById('sstat').textContent=s.w+'x'+s.h+'  frames='+s.frames;}catch(e){}}"
        " else if(cur=='cpu'){try{let s=await(await fetch('/api/state')).json();"
        "  if(!s.valid){document.getElementById('cpubox').textContent='(no state yet)';}"
        "  else{let h='<div class=hdr>tick '+s.tick+'   fps '+s.fps+'   running '+s.running+'   cpsr '+hx(s.cpsr)+'</div><table>';"
        "   for(let i=0;i<16;i+=4){h+='<tr>';for(let j=0;j<4;j++){let k=i+j;h+='<td><span class=k>'+RN[k]+'</span> '+hx(s.regs[k])+'</td>';}h+='</tr>';}"
        "   h+='</table>';document.getElementById('cpubox').innerHTML=h;}}catch(e){}}"
        " else if(cur=='mem'){if(document.getElementById('mauto').checked)readMem();}"
        " else{try{let r=await(await fetch('/api/log?cat='+cur)).json();"
        "  let el=document.getElementById('p'+cur);let at=el.scrollTop+el.clientHeight>=el.scrollHeight-30;"
        "  el.textContent=r.lines.join('\\n');if(at)el.scrollTop=el.scrollHeight;}catch(e){}}"
        "}"
        "setInterval(tickUI,250);tickUI();"
        "</script></body></html>";
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
  std::thread encoder_thread_;
  std::mutex mu_;
  std::vector<uint8_t> latest_png_;
  int w_ = 0, h_ = 0;
  uint64_t frames_ = 0;

  // Raw-frame handoff to the encoder thread (latest-wins, stale dropped).
  std::mutex raw_mu_;
  std::condition_variable raw_cv_;
  std::vector<uint8_t> pending_raw_;
  int pending_w_ = 0, pending_h_ = 0;
  bool pending_ready_ = false;

  // Optional live guest-memory reader (set by the runner); see SetMemReader.
  std::function<std::string(uint32_t, uint32_t)> mem_reader_;
};

}  // namespace zeebulator

#endif  // ZEEBULATOR_CORE_CONTROL_MIRROR_SERVER_H_
