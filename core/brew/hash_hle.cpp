#include "core/brew/hash_hle.h"

#include <cstring>
#include <vector>

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {
constexpr uint32_t kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

inline uint32_t RotL(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }
}  // namespace

void Md5State::Reset() {
  a_ = 0x67452301;
  b_ = 0xefcdab89;
  c_ = 0x98badcfe;
  d_ = 0x10325476;
  total_len_ = 0;
  buffer_len_ = 0;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Md5State::ProcessBlock(const uint8_t block[64]) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(block[i * 4]) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }
  uint32_t a = a_, b = b_, c = c_, d = d_;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    uint32_t temp = d;
    d = c;
    c = b;
    uint32_t sum = a + f + kMd5K[i] + m[g];
    b = b + RotL(sum, kMd5S[i]);
    a = temp;
  }
  a_ += a;
  b_ += b;
  c_ += c;
  d_ += d;
}

void Md5State::Update(const uint8_t* data, uint32_t len) {
  total_len_ += len;
  uint32_t offset = 0;
  if (buffer_len_ > 0) {
    uint32_t take = std::min<uint32_t>(64 - buffer_len_, len);
    std::memcpy(buffer_ + buffer_len_, data, take);
    buffer_len_ += take;
    offset += take;
    if (buffer_len_ == 64) {
      ProcessBlock(buffer_);
      buffer_len_ = 0;
    }
  }
  while (len - offset >= 64) {
    ProcessBlock(data + offset);
    offset += 64;
  }
  uint32_t remaining = len - offset;
  if (remaining > 0) {
    std::memcpy(buffer_, data + offset, remaining);
    buffer_len_ = remaining;
  }
}

std::array<uint8_t, 16> Md5State::Finish() const {
  // Copy state so repeated GetDigest() calls (no re-Update in between) are
  // idempotent, matching the zeebx oracle's `.clone().finish()` contract.
  Md5State copy = *this;
  uint64_t bit_len = copy.total_len_ * 8;
  uint8_t pad = 0x80;
  copy.Update(&pad, 1);
  uint8_t zero = 0x00;
  while (copy.buffer_len_ != 56) {
    copy.Update(&zero, 1);
  }
  uint8_t len_bytes[8];
  for (int i = 0; i < 8; ++i) {
    len_bytes[i] = static_cast<uint8_t>(bit_len >> (8 * i));
  }
  copy.Update(len_bytes, 8);

  std::array<uint8_t, 16> out;
  auto put = [&out](int idx, uint32_t v) {
    out[idx] = static_cast<uint8_t>(v);
    out[idx + 1] = static_cast<uint8_t>(v >> 8);
    out[idx + 2] = static_cast<uint8_t>(v >> 16);
    out[idx + 3] = static_cast<uint8_t>(v >> 24);
  };
  put(0, copy.a_);
  put(4, copy.b_);
  put(8, copy.c_);
  put(12, copy.d_);
  return out;
}

HashHle::HashHle(Memory& memory, HleRuntime& hle,
                  std::function<uint32_t(uint32_t)> malloc_fn)
    : memory_(memory), hle_(hle), malloc_fn_(std::move(malloc_fn)) {
  md5_.Reset();
}

uint32_t HashHle::Build(uint32_t vtable_address, uint32_t object_address) {
  // Real layout, from the SDK itself (platform/deprecated/inc/AEESecurity.h):
  //   QINTERFACE(IHash) { DECLARE_IBASE(IHash)   // AddRef, Release only
  //     void (*Update)(IHash*, const byte* pbData, int cbData);
  //     int  (*GetResult)(IHash*, byte* pbData, int* pcbData);
  //     void (*Restart)(IHash*);
  //     int  (*SetKey)(IHash*, const byte* pbKey, int cbKey); };
  // IHash has NO QueryInterface: DECLARE_IBASE stops at Release. We used to
  // start at QueryInterface and shift everything by one, so a guest Update()
  // landed on QueryInterface (which wrote 4 bytes through what was really a
  // length argument) and GetResult() landed on Reset (digest stayed zero).
  // zeebx aee_slots.rs:1088 reports the same correction, found from Zeeboids.
  std::vector<HleRuntime::HleFunction> methods = {
      [this](IArmCore& c) { AddRef(c); },         // 0 AddRef
      [this](IArmCore& c) { Release(c); },        // 1 Release
      [this](IArmCore& c) { UpdateImpl(c); },     // 2 Update
      [this](IArmCore& c) { GetResultImpl(c); },  // 3 GetResult
      [this](IArmCore& c) { ResetImpl(c); },      // 4 Restart
      [this](IArmCore& c) { SetKeyImpl(c); },     // 5 SetKey
  };
  const std::vector<const char*> slot_names = {"AddRef",  "Release", "Update",
                                               "GetResult", "Restart", "SetKey"};
  return BuildInterfaceObjectLabeled(memory_, hle_, vtable_address,
                                     object_address, methods, "IHash",
                                     slot_names);
}

void HashHle::AddRef(IArmCore& core) {
  ++ref_count_;
  core.SetRegister(kR0, ref_count_);
}

void HashHle::Release(IArmCore& core) {
  if (ref_count_ > 0) --ref_count_;
  core.SetRegister(kR0, ref_count_);
}

void HashHle::QueryInterface(IArmCore& core) {
  uint32_t ppo = core.GetRegister(kR2);
  if (ppo != 0) {
    memory_.Write32(ppo, core.GetRegister(kR0));
  }
  core.SetRegister(kR0, 0);  // SUCCESS: this object answers for itself
}

void HashHle::ResetImpl(IArmCore& core) {
  md5_.Reset();
  digest_addr_ = 0;
  core.SetRegister(kR0, 0);
}

void HashHle::UpdateImpl(IArmCore& core) {
  // void IHASH_Update(IHash *, const byte *pData, int nLen)
  uint32_t src = core.GetRegister(kR1);
  uint32_t len = core.GetRegister(kR2);
  if (len > 0 && src != 0) {
    std::vector<uint8_t> bytes(len);
    for (uint32_t i = 0; i < len; ++i) bytes[i] = memory_.Read8(src + i);
    md5_.Update(bytes.data(), len);
  }
  core.SetRegister(kR0, 0);
}

void HashHle::GetDigestImpl(IArmCore& core) {
  // Undocumented signature (no SDK 4.0.2 header): honor both plausible
  // forms observed from the zeebx oracle -- the digest address returns in
  // R0, and if the caller passed a usable output pointer in R1 the 16
  // bytes are also written there.
  auto digest = md5_.Finish();
  if (digest_addr_ == 0 && malloc_fn_) {
    digest_addr_ = malloc_fn_(static_cast<uint32_t>(digest.size()));
  }
  if (digest_addr_ != 0) {
    for (size_t i = 0; i < digest.size(); ++i) {
      memory_.Write8(digest_addr_ + static_cast<uint32_t>(i), digest[i]);
    }
  }
  uint32_t out_ptr = core.GetRegister(kR1);
  if (out_ptr != 0) {
    for (size_t i = 0; i < digest.size(); ++i) {
      memory_.Write8(out_ptr + static_cast<uint32_t>(i), digest[i]);
    }
  }
  core.SetRegister(kR0, digest_addr_);
}

// int GetResult(IHash*, byte* pbData, int* pcbData) -- writes the digest into
// the caller's buffer and reports its size through pcbData. Returns
// AEE_SUCCESS. This is the call a title makes to actually read a hash; the old
// table never reached it.
void HashHle::GetResultImpl(IArmCore& core) {
  auto digest = md5_.Finish();
  uint32_t out = core.GetRegister(kR1);
  uint32_t out_len = core.GetRegister(kR2);
  if (out != 0) {
    for (size_t i = 0; i < digest.size(); ++i) {
      memory_.Write8(out + static_cast<uint32_t>(i), digest[i]);
    }
  }
  if (out_len != 0) memory_.Write32(out_len, static_cast<uint32_t>(digest.size()));
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

// int SetKey(IHash*, const byte* pbKey, int cbKey) -- only meaningful for the
// HMAC variants. Plain MD5 has no key, and the SDK documents an error return
// for unsupported operations, so report AEE_EUNSUPPORTED instead of pretending.
void HashHle::SetKeyImpl(IArmCore& core) {
  constexpr uint32_t kAeeEunsupported = 20;
  core.SetRegister(kR0, kAeeEunsupported);
}

void HashHle::GetDigestSizeImpl(IArmCore& core) {
  core.SetRegister(kR0, 16);  // MD5 digest size in bytes
}

}  // namespace zeebulator
