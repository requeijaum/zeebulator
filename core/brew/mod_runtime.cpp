#include "core/brew/mod_runtime.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace zeebulator {

namespace {
// Offsets within the static-base table where real disassembly (see
// mod_runtime.h) shows the MALLOC/FREE/GetAppContext function pointers
// living.
constexpr uint32_t kMemcpySlotOffset = 0x0;
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kStrcpySlotOffset = 0x8;
constexpr uint32_t kStrlenSlotOffset = 0x14;
constexpr uint32_t kBoundedStrcpySlotOffset = 0xe4;
constexpr uint32_t kStrstrSlotOffset = 0xe8;
constexpr uint32_t kSprintfSlotOffset = 0x13c;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
constexpr uint32_t kDbgPrintfSlotOffset = 0x9c;
constexpr uint32_t kMemcpyAliasSlotOffset = 0x44;
constexpr uint32_t kReallocSlotOffset = 0x74;
constexpr uint32_t kUnknownSlotOffset0x40 = 0x40;
constexpr uint32_t kUnknownSlotOffset0xc = 0xc;
constexpr uint32_t kUnknownSlotOffset0xd0 = 0xd0;
constexpr uint32_t kUnknownSlotOffset0xdc = 0xdc;
constexpr uint32_t kUnknownSlotOffset0x184 = 0x184;
constexpr uint32_t kUnknownSlotOffset0x1b4 = 0x1b4;
constexpr uint32_t kStrncpySlotOffset = 0xc8;
constexpr uint32_t kStrchrSlotOffset = 0x18;
constexpr uint32_t kUnknownSlotOffset0x140 = 0x140;
constexpr uint32_t kUnknownSlotOffset0x138 = 0x138;
constexpr uint32_t kUnknownSlotOffset0x30 = 0x30;
constexpr uint32_t kUnknownSlotOffset0x144 = 0x144;
constexpr uint32_t kUnknownSlotOffset0x14c = 0x14c;
constexpr uint32_t kUnknownSlotOffset0x150 = 0x150;
constexpr uint32_t kUnknownSlotOffset0x64 = 0x64;
constexpr uint32_t kUnknownSlotOffset0xcc = 0xcc;
constexpr uint32_t kUnknownSlotOffset0x90 = 0x90;
constexpr uint32_t kUnknownSlotOffset0x10 = 0x10;
constexpr uint32_t kUnknownSlotOffset0x1c = 0x1c;
constexpr uint32_t kUnknownSlotOffset0x20 = 0x20;
constexpr uint32_t kCheckObjectFlagSlotOffset0xa8 = 0xa8;
// Offsets within the "app context" struct GetAppContext returns where
// real call sites read the current app's IShell/IDisplay pointers.
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
constexpr uint32_t kAppContextThirdObjectOffset = 0x2c;
constexpr uint32_t kAppContextFourthObjectOffset = 0x24;
constexpr uint32_t kAppContextFifthObjectOffset = 0x28;
}  // namespace

ModRuntime::ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size,
                        uint32_t context_address)
    : memory_(memory),
      hle_(hle),
      heap_cursor_(heap_region),
      heap_end_(heap_region + heap_size),
      context_address_(context_address) {}

void ModRuntime::SetShellInstance(uint32_t shell_ptr) {
  shell_ptr_ = shell_ptr;
  shell_pending_ = true;
}

void ModRuntime::SetDisplayInstance(uint32_t display_ptr) {
  display_ptr_ = display_ptr;
  display_pending_ = true;
}

void ModRuntime::SetThirdContextObject(uint32_t object_ptr) {
  third_context_object_ = object_ptr;
  third_pending_ = true;
}

void ModRuntime::SetFourthContextObject(uint32_t object_ptr) {
  fourth_context_object_ = object_ptr;
  fourth_pending_ = true;
}

void ModRuntime::SetFifthContextObject(uint32_t object_ptr) {
  fifth_context_object_ = object_ptr;
  fifth_pending_ = true;
}

void ModRuntime::SetContextAddress(uint32_t context_address) {
  context_address_ = context_address;
  // Re-prime the two confirmed-real OS-provided fields (real code
  // doesn't construct its own Shell/IDisplay) onto the new address...
  shell_pending_ = true;
  display_pending_ = true;
  // ...but deliberately leave the third/fourth/fifth placeholders
  // un-primed here -- see this method's doc comment in mod_runtime.h
  // for why (the fifth field doubles as a real "already initialized"
  // gate real code checks before running its own real construction).
  third_pending_ = false;
  fourth_pending_ = false;
  fifth_pending_ = false;
}

uint32_t ModRuntime::Allocate(uint32_t size) {
  uint32_t aligned = (size + 3) & ~3u;  // word-align every allocation
  if (aligned > heap_end_ - heap_cursor_) {
    return 0;  // NULL: out of (emulated) heap space
  }
  uint32_t result = heap_cursor_;
  heap_cursor_ += aligned;
  return result;
}

void ModRuntime::MallocImpl(IArmCore& core) {
  core.SetRegister(kR0, Allocate(core.GetRegister(kR0)));
}

void ModRuntime::ReallocImpl(IArmCore& core) {
  // void *realloc(void *ptr, size_t size)
  //
  // Real disassembly of Peggle (TASKS.md Phase 8, PHASE8_LOG.md) shows
  // two independent real growable-array implementations (56-byte and
  // 4-byte elements) calling this static-base slot identically:
  // `(old_ptr=array's current buffer, new_size=new_element_count *
  // element_size)`, checking the result for non-null before overwriting
  // their own buffer pointer -- exactly realloc's real contract,
  // including "leave the old block alone on failure" (this never writes
  // to `old_ptr`, only reads from it).
  //
  // This allocator has no free-list (see Allocate()/FREE's own doc
  // comment), so unlike real realloc there's no way to know the old
  // block's real size to copy just that many bytes -- copies `size`
  // bytes from the old block instead. Safe in this bump-allocator's
  // specific case even when that overreads past the old block's real
  // content: bump-allocated memory is never reused, so anything past
  // the old content is either still-zeroed, never-touched memory, or a
  // later allocation that hasn't happened yet -- and the real callers
  // observed always immediately overwrite that tail with new elements
  // right after a successful grow anyway.
  uint32_t old_ptr = core.GetRegister(kR0);
  uint32_t size = core.GetRegister(kR1);
  uint32_t new_ptr = Allocate(size);
  if (new_ptr != 0 && old_ptr != 0) {
    for (uint32_t i = 0; i < size; ++i) {
      memory_.Write8(new_ptr + i, memory_.Read8(old_ptr + i));
    }
  }
  core.SetRegister(kR0, new_ptr);
}

void ModRuntime::DecompressGzipInPlaceImpl(IArmCore& core) {
  // Real job identified via disassembly (see this slot's own doc
  // comment in mod_runtime.h for the full derivation): decompress the
  // real gzip stream at r0 in place. Real gzip streams don't declare
  // their own compressed length up front, so this reads incrementally
  // from emulated memory and lets zlib signal Z_STREAM_END, rather
  // than assuming a fixed input size; the output buffer grows the same
  // way. Matches this project's own already-established real gzip
  // handling (core/loader/ggz.cpp): windowBits = 15 + 16 decodes gzip
  // framing specifically.
  uint32_t ptr = core.GetRegister(kR0);

  constexpr uint32_t kChunk = 4096;
  constexpr uint32_t kMaxCompressed = 4 * 1024 * 1024;  // defensive cap

  z_stream strm{};
  if (inflateInit2(&strm, 15 + 16) != Z_OK) {
    core.SetRegister(kR0, 1);
    return;
  }

  std::vector<uint8_t> compressed;
  std::vector<uint8_t> decompressed(kChunk);
  strm.next_out = decompressed.data();
  strm.avail_out = static_cast<uInt>(decompressed.size());

  uint32_t read_offset = 0;
  int ret = Z_OK;
  bool failed = false;
  while (ret != Z_STREAM_END) {
    if (strm.avail_in == 0) {
      if (read_offset >= kMaxCompressed) {
        failed = true;
        break;
      }
      uint32_t n = std::min(kChunk, kMaxCompressed - read_offset);
      compressed.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        compressed[i] = memory_.Read8(ptr + read_offset + i);
      }
      read_offset += n;
      strm.next_in = compressed.data();
      strm.avail_in = static_cast<uInt>(n);
    }
    if (strm.avail_out == 0) {
      size_t old_size = decompressed.size();
      decompressed.resize(old_size + kChunk);
      strm.next_out = decompressed.data() + old_size;
      strm.avail_out = static_cast<uInt>(kChunk);
    }
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      failed = true;
      break;
    }
  }
  size_t produced = decompressed.size() - strm.avail_out;
  inflateEnd(&strm);

  if (failed) {
    core.SetRegister(kR0, 1);
    return;
  }
  for (size_t i = 0; i < produced; ++i) {
    memory_.Write8(ptr + static_cast<uint32_t>(i), decompressed[i]);
  }
  core.SetRegister(kR0, 0);
}

void ModRuntime::SortPointerArrayImpl(IArmCore& core) {
  // Real job identified via disassembly (see this slot's own doc
  // comment in mod_runtime.h for the full derivation): a real generic
  // sort, `void SORT(void *base, int count, int size, int
  // (*compar)(const void*, const void*))`. `size` is honored as the
  // real stride even though every real call site seen so far uses 4
  // (pointer-sized elements), since nothing here depends on the
  // elements actually being pointers.
  uint32_t base = core.GetRegister(kR0);
  int32_t count = static_cast<int32_t>(core.GetRegister(kR1));
  int32_t size = static_cast<int32_t>(core.GetRegister(kR2));
  uint32_t compar = core.GetRegister(kR3);

  // Defensive cap, not a game-logic decision: this is real ROM data
  // crossing into host C++ control flow, and a corrupt/unexpected count
  // should degrade to "did nothing" rather than let a runaway loop hang
  // the tool.
  constexpr int32_t kMaxCount = 4096;
  if (count <= 1 || size <= 0 || count > kMaxCount || compar == 0) {
    core.SetRegister(kR0, 0);
    return;
  }

  // CallArmFunction repurposes LR as its own return sentinel, so it
  // must be saved/restored around every nested call -- this HLE
  // function is itself invoked from inside another real ARM call's own
  // Dispatch(), and the real caller of *this* slot still needs its
  // original LR intact once we return.
  uint32_t saved_lr = core.GetRegister(kLR);

  // Real in-place insertion sort (stable, and correct regardless of
  // whether the real comparator forms a strict weak ordering -- unlike
  // quicksort, insertion sort never depends on that).
  //
  // Arguments are deliberately passed to the real comparator as
  // `compar(next, prev)` -- swapped from the naive `compar(prev, next)`
  // -- which makes the final array order *descending* by the real
  // comparator's own "before-or-equal" relation (elements it considers
  // later end up first). This isn't derivable from the disassembly
  // alone: both argument orders call the real comparator faithfully and
  // are equally "correct" as a generic sort, but only this one produces
  // the real, on-screen character layering confirmed live against real
  // Double Dragon footage and Infuse (an independent real BREW/Zeebo
  // reimplementation) -- see PHASE8_LOG.md's sprite z-ordering fix round.
  // With the real comparator's primary key at entity+0x7c, this means
  // entities with the *larger* +0x7c value get registered first each
  // frame (and, per this project's own already-confirmed real per-
  // vertex Z-stamping, the most-negative/farthest depth), and smaller
  // +0x7c draws last/nearest.
  for (int32_t i = 1; i < count; ++i) {
    int32_t j = i;
    while (j > 0) {
      uint32_t a_addr = base + static_cast<uint32_t>(j - 1) * size;
      uint32_t b_addr = base + static_cast<uint32_t>(j) * size;
      int32_t result = static_cast<int32_t>(hle_.CallArmFunction(compar, b_addr, a_addr));
      if (result >= 0) break;
      for (int32_t k = 0; k < size; ++k) {
        uint8_t tmp = memory_.Read8(a_addr + k);
        memory_.Write8(a_addr + k, memory_.Read8(b_addr + k));
        memory_.Write8(b_addr + k, tmp);
      }
      --j;
    }
  }

  core.SetRegister(kLR, saved_lr);
  core.SetRegister(kR0, 0);
}

void ModRuntime::MemcpyImpl(IArmCore& core) {
  // void *memcpy(void *dest, const void *src, size_t n)
  uint32_t dest = core.GetRegister(kR0);
  uint32_t src = core.GetRegister(kR1);
  uint32_t count = core.GetRegister(kR2);
  for (uint32_t i = 0; i < count; ++i) {
    memory_.Write8(dest + i, memory_.Read8(src + i));
  }
  core.SetRegister(kR0, dest);  // memcpy returns its first argument
}

void ModRuntime::MemsetImpl(IArmCore& core) {
  // void *memset(void *s, int c, size_t n)
  uint32_t dest = core.GetRegister(kR0);
  uint8_t value = static_cast<uint8_t>(core.GetRegister(kR1));
  uint32_t count = core.GetRegister(kR2);
  for (uint32_t i = 0; i < count; ++i) {
    memory_.Write8(dest + i, value);
  }
  core.SetRegister(kR0, dest);  // memset returns its first argument
}

void ModRuntime::StrlenImpl(IArmCore& core) {
  // size_t strlen(const char *s)
  uint32_t s = core.GetRegister(kR0);
  uint32_t len = 0;
  while (memory_.Read8(s + len) != 0) {
    ++len;
  }
  core.SetRegister(kR0, len);
}

void ModRuntime::StrcpyImpl(IArmCore& core) {
  // char *strcpy(char *dest, const char *src)
  uint32_t dest = core.GetRegister(kR0);
  uint32_t src = core.GetRegister(kR1);
  uint32_t i = 0;
  for (;;) {
    uint8_t byte = memory_.Read8(src + i);
    memory_.Write8(dest + i, byte);
    if (byte == 0) break;
    ++i;
  }
  core.SetRegister(kR0, dest);  // strcpy returns its first argument
}

void ModRuntime::BoundedStrcpyImpl(IArmCore& core) {
  // Copy semantics inferred from the calling convention (see
  // mod_runtime.h) rather than matched to a named reference function:
  // n = min(n, cap); memcpy(dest, src, n).
  uint32_t src = core.GetRegister(kR0);
  uint32_t n = core.GetRegister(kR1);
  uint32_t dest = core.GetRegister(kR2);
  uint32_t cap = core.GetRegister(kR3);
  n = std::min(n, cap);
  for (uint32_t i = 0; i < n; ++i) {
    memory_.Write8(dest + i, memory_.Read8(src + i));
  }
  core.SetRegister(kR0, dest);
}

void ModRuntime::StrncpyImpl(IArmCore& core) {
  // char *strncpy(char *dest, const char *src, size_t maxlen) -- real
  // standard semantics (see kStrncpySlotOffset's own doc comment):
  // copies up to maxlen bytes from src, padding the remainder of dest
  // with nulls if src is shorter, and does NOT null-terminate if src is
  // maxlen bytes or longer.
  uint32_t dest = core.GetRegister(kR0);
  uint32_t src = core.GetRegister(kR1);
  uint32_t maxlen = core.GetRegister(kR2);
  bool src_ended = false;
  for (uint32_t i = 0; i < maxlen; ++i) {
    uint8_t byte = src_ended ? 0 : memory_.Read8(src + i);
    if (byte == 0) src_ended = true;
    memory_.Write8(dest + i, byte);
  }
  core.SetRegister(kR0, dest);  // strncpy returns its first argument
}

void ModRuntime::StrchrImpl(IArmCore& core) {
  // char *strchr(const char *s, int c) -- real standard semantics:
  // scans s for the first occurrence of c (c==0 matches the string's
  // own null terminator itself), returning a pointer to it or NULL if
  // not found before the terminator.
  uint32_t s = core.GetRegister(kR0);
  auto c = static_cast<uint8_t>(core.GetRegister(kR1));
  for (uint32_t i = 0;; ++i) {
    uint8_t byte = memory_.Read8(s + i);
    if (byte == c) {
      core.SetRegister(kR0, s + i);
      return;
    }
    if (byte == 0) {
      core.SetRegister(kR0, 0);
      return;
    }
  }
}

void ModRuntime::StrstrImpl(IArmCore& core) {
  // char *strstr(const char *haystack, const char *needle)
  uint32_t haystack = core.GetRegister(kR0);
  uint32_t needle = core.GetRegister(kR1);
  uint32_t needle_len = 0;
  while (memory_.Read8(needle + needle_len) != 0) ++needle_len;
  if (needle_len == 0) {
    core.SetRegister(kR0, haystack);
    return;
  }
  for (uint32_t i = 0;; ++i) {
    if (memory_.Read8(haystack + i) == 0) {
      core.SetRegister(kR0, 0);
      return;
    }
    bool match = true;
    for (uint32_t j = 0; j < needle_len; ++j) {
      if (memory_.Read8(haystack + i + j) != memory_.Read8(needle + j)) {
        match = false;
        break;
      }
    }
    if (match) {
      core.SetRegister(kR0, haystack + i);
      return;
    }
  }
}

void ModRuntime::SprintfImpl(IArmCore& core) {
  // Real signature and behavior inferred from the calling convention
  // plus real string content (PHASE8_LOG.md): a real call site
  // (`ddragonz.mod` offset 0x23d0c) formats the real literal string
  // "ERROR CODE:%d" (found directly in the file's own bytes at the
  // literal's address) into a stack buffer, immediately measured with
  // STRLEN afterward -- a sprintf-family helper. Signature: int
  // Func(char *dest, const char *fmt, void **ppArgs) -- ppArgs is a
  // pointer to an args cursor that gets ADVANCED by 4 bytes (this ABI's
  // word size) per consumed argument, not a plain va_list, matching the
  // double indirection at the call site (R2 points at a stack slot that
  // itself holds the args block's address). Supports the directives
  // real game code has been observed needing so far: %d, %u, %x/%X,
  // %s, %c, %%, plus an optional `0`-flag and a decimal minimum-field-
  // width between `%` and the conversion character (e.g. `%7d`, real
  // Double Dragon HUD text, `ddragonz.mod` file offset 0x6c0b0's real
  // "J1 %7d" literal -- confirmed live via a memory read-watch on that
  // exact string that every read came from this function, then a
  // trap-index count confirmed which registered function trap 0x2c
  // is: PHASE8_LOG.md). Before this fix, an unrecognized single
  // character right after `%` (here, the width digit `7`) fell
  // through to the "unknown directive" fallback, which emitted `%7`
  // literally and left `d` to be copied as an ordinary character right
  // after -- passing the whole literal "%7d" straight through
  // unsubstituted, real integer argument never consumed. No
  // precision (`.N`) support (no evidence any real call needs it
  // yet; extend if one does). Returns the number of characters
  // written, matching the real sprintf() contract.
  uint32_t dest = core.GetRegister(kR0);
  uint32_t fmt = core.GetRegister(kR1);
  uint32_t args_cursor_addr = core.GetRegister(kR2);
  uint32_t args = memory_.Read32(args_cursor_addr);

  uint32_t out = dest;
  for (uint32_t i = 0;; ++i) {
    uint8_t c = memory_.Read8(fmt + i);
    if (c == 0) break;
    if (c != '%') {
      memory_.Write8(out++, c);
      continue;
    }
    uint32_t spec_start = i;  // '%' itself, for the unknown-directive fallback
    ++i;
    bool zero_pad = memory_.Read8(fmt + i) == '0';
    if (zero_pad) ++i;
    uint32_t width = 0;
    bool has_width = false;
    for (;;) {
      uint8_t d = memory_.Read8(fmt + i);
      if (d < '0' || d > '9') break;
      has_width = true;
      width = width * 10 + (d - '0');
      ++i;
    }
    uint8_t spec = memory_.Read8(fmt + i);
    if (spec == 0) break;  // trailing '%...' with nothing after: stop
    if (spec == '%' && !zero_pad && !has_width) {
      memory_.Write8(out++, '%');
      continue;
    }
    std::string formatted;
    switch (spec) {
      case 'd':
        formatted = std::to_string(static_cast<int32_t>(memory_.Read32(args)));
        args += 4;
        break;
      case 'u':
        formatted = std::to_string(memory_.Read32(args));
        args += 4;
        break;
      case 'x':
      case 'X': {
        char buf[9];
        std::snprintf(buf, sizeof(buf), spec == 'x' ? "%x" : "%X", memory_.Read32(args));
        formatted = buf;
        args += 4;
        break;
      }
      case 'c':
        formatted = std::string(1, static_cast<char>(memory_.Read32(args)));
        args += 4;
        break;
      case 's': {
        uint32_t str_ptr = memory_.Read32(args);
        args += 4;
        for (uint32_t j = 0; memory_.Read8(str_ptr + j) != 0; ++j) {
          formatted.push_back(static_cast<char>(memory_.Read8(str_ptr + j)));
        }
        break;
      }
      default:
        // Unknown directive: emit literally rather than silently
        // dropping it -- the whole real `%...` sequence (any `0`-flag
        // and width digits included) through the unrecognized
        // character itself.
        for (uint32_t k = spec_start; k <= i; ++k) {
          formatted.push_back(static_cast<char>(memory_.Read8(fmt + k)));
        }
        break;
    }
    if (has_width && formatted.size() < width) {
      formatted = std::string(width - formatted.size(), zero_pad ? '0' : ' ') + formatted;
    }
    for (char ch : formatted) memory_.Write8(out++, static_cast<uint8_t>(ch));
  }
  memory_.Write8(out, 0);
  memory_.Write32(args_cursor_addr, args);
  core.SetRegister(kR0, out - dest);
}

void ModRuntime::FormatSingleIntImpl(IArmCore& core) {
  // Real signature: char* Func(char* dest, const char* fmt, int value) --
  // see mod_runtime.h's own doc comment (the table's thirty-third slot)
  // for the full real derivation. A real sprintf-family formatter, same
  // job as SprintfImpl above, but with a real single, direct integer
  // argument instead of that one's own double-indirection ppArgs
  // cursor -- real format strings observed so far only ever consume
  // exactly one directive (`%d`, `%06d`, `x%d`), so this real value is
  // substituted at the one real numeric directive found, if any; a
  // real literal with no directive at all (`"EXTRA"`) is copied
  // through unchanged, same as SprintfImpl's own literal-text handling.
  uint32_t dest = core.GetRegister(kR0);
  uint32_t fmt = core.GetRegister(kR1);
  int32_t value = static_cast<int32_t>(core.GetRegister(kR2));

  uint32_t out = dest;
  for (uint32_t i = 0;; ++i) {
    uint8_t c = memory_.Read8(fmt + i);
    if (c == 0) break;
    if (c != '%') {
      memory_.Write8(out++, c);
      continue;
    }
    uint32_t spec_start = i;
    ++i;
    bool zero_pad = memory_.Read8(fmt + i) == '0';
    if (zero_pad) ++i;
    uint32_t width = 0;
    bool has_width = false;
    for (;;) {
      uint8_t d = memory_.Read8(fmt + i);
      if (d < '0' || d > '9') break;
      has_width = true;
      width = width * 10 + (d - '0');
      ++i;
    }
    uint8_t spec = memory_.Read8(fmt + i);
    if (spec == 0) break;
    if (spec == '%' && !zero_pad && !has_width) {
      memory_.Write8(out++, '%');
      continue;
    }
    std::string formatted;
    switch (spec) {
      case 'd':
        formatted = std::to_string(value);
        break;
      case 'u':
        formatted = std::to_string(static_cast<uint32_t>(value));
        break;
      case 'x':
      case 'X': {
        char buf[9];
        std::snprintf(buf, sizeof(buf), spec == 'x' ? "%x" : "%X",
                      static_cast<uint32_t>(value));
        formatted = buf;
        break;
      }
      default:
        // Unknown directive: emit literally, same precedent as
        // SprintfImpl's own fallback above.
        for (uint32_t k = spec_start; k <= i; ++k) {
          formatted.push_back(static_cast<char>(memory_.Read8(fmt + k)));
        }
        break;
    }
    bool numeric_directive = spec == 'd' || spec == 'u' || spec == 'x' || spec == 'X';
    if (numeric_directive && has_width && formatted.size() < width) {
      bool negative = numeric_directive && !formatted.empty() && formatted[0] == '-';
      std::string digits = negative ? formatted.substr(1) : formatted;
      std::string sign = negative ? "-" : "";
      if (sign.size() + digits.size() < width) {
        digits = std::string(width - sign.size() - digits.size(), zero_pad ? '0' : ' ') + digits;
      }
      formatted = sign + digits;
    }
    for (char ch : formatted) memory_.Write8(out++, static_cast<uint8_t>(ch));
  }
  memory_.Write8(out, 0);
  core.SetRegister(kR0, dest);
}

void ModRuntime::GetAppContextImpl(IArmCore& core) {
  // Only (re-)written when a Set*() call is actually pending, not on
  // every call -- see the `*_pending_` members' doc comment in
  // mod_runtime.h for why: real code writes directly into the
  // third/fourth/fifth fields (confirmed real, see the class doc
  // comment), and unconditionally rewriting them here every call would
  // silently clobber those real writes on the very next GetAppContext
  // call.
  if (shell_pending_) {
    memory_.Write32(context_address_ + kAppContextShellOffset, shell_ptr_);
    shell_pending_ = false;
  }
  if (display_pending_) {
    memory_.Write32(context_address_ + kAppContextDisplayOffset, display_ptr_);
    display_pending_ = false;
  }
  if (third_pending_) {
    memory_.Write32(context_address_ + kAppContextThirdObjectOffset, third_context_object_);
    third_pending_ = false;
  }
  if (fourth_pending_) {
    memory_.Write32(context_address_ + kAppContextFourthObjectOffset, fourth_context_object_);
    fourth_pending_ = false;
  }
  if (fifth_pending_) {
    memory_.Write32(context_address_ + kAppContextFifthObjectOffset, fifth_context_object_);
    fifth_pending_ = false;
  }
  core.SetRegister(kR0, context_address_);
}

void ModRuntime::GetUpTimeMsImpl(IArmCore& core) {
  core.SetRegister(kR0, uptime_ms_);
  // Real code that busy-waits on elapsed time (confirmed by real
  // disassembly -- see TASKS.md Phase 8, Super BurgerTime's real
  // ROM-readiness poll at static-base slot 0x184) calls this in a tight
  // loop entirely within a single native HLE call, with no opportunity
  // for the outer per-frame Tick() below to ever run in between. A
  // clock that only Tick() can move would stay frozen for the loop's
  // entire lifetime, so any such real busy-wait can never see its own
  // deadline pass and would spin forever -- not a real device's
  // behavior, just an artifact of our clock only being driven
  // externally. Self-advancing by a small synthetic amount on every
  // read keeps this fully deterministic (same call sequence always
  // produces the same values, unlike a genuine wall-clock read would)
  // while still letting real elapsed-time busy-waits make forward
  // progress and eventually resolve, the same way they would on real
  // hardware given enough real wall-clock time. The 1ms-per-read rate
  // is an inferred, not measured, choice -- plausible for a real
  // "checked once per real hardware poll iteration" loop.
  uptime_ms_ += 1;
}

void ModRuntime::Tick(uint32_t elapsed_ms) { uptime_ms_ += elapsed_ms; }

void ModRuntime::Install(uint32_t module_base, uint32_t table_address) {
  uint32_t memcpy_fn = hle_.Register([this](IArmCore& core) { MemcpyImpl(core); });
  uint32_t memset_fn = hle_.Register([this](IArmCore& core) { MemsetImpl(core); });
  uint32_t strlen_fn = hle_.Register([this](IArmCore& core) { StrlenImpl(core); });
  uint32_t strcpy_fn = hle_.Register([this](IArmCore& core) { StrcpyImpl(core); });
  uint32_t malloc_fn = hle_.Register([this](IArmCore& core) { MallocImpl(core); });
  uint32_t free_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t get_uptime_ms_fn = hle_.Register([this](IArmCore& core) { GetUpTimeMsImpl(core); });
  uint32_t get_app_context_fn = hle_.Register([this](IArmCore& core) { GetAppContextImpl(core); });
  uint32_t bounded_strcpy_fn = hle_.Register([this](IArmCore& core) { BoundedStrcpyImpl(core); });
  uint32_t strstr_fn = hle_.Register([this](IArmCore& core) { StrstrImpl(core); });
  uint32_t sprintf_fn = hle_.Register([this](IArmCore& core) { SprintfImpl(core); });
  uint32_t dbgprintf_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t realloc_fn = hle_.Register([this](IArmCore& core) { ReallocImpl(core); });
  uint32_t unknown_0x40_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xc_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xd0_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xdc_fn =
      hle_.Register([this](IArmCore& core) { DecompressGzipInPlaceImpl(core); });
  uint32_t unknown_0x184_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x1b4_fn =
      hle_.Register([this](IArmCore& core) { SortPointerArrayImpl(core); });
  uint32_t strncpy_fn = hle_.Register([this](IArmCore& core) { StrncpyImpl(core); });
  uint32_t strchr_fn = hle_.Register([this](IArmCore& core) { StrchrImpl(core); });
  uint32_t unknown_0x140_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x138_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x30_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x144_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x14c_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x150_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x64_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xcc_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x90_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x10_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  // Real, confirmed gap: Alien Breaker Deluxe's own real per-object init
  // loop (abd.mod 0x10619c and 0x106150, both `blx [runtime_table+0x1c]`)
  // jumps through this slot unconditionally once its own real per-object
  // initialization work is done (found live tracing that title's own
  // bring-up, TASKS.md). Left unregistered, this is a null-pointer jump
  // -- registered as a safe no-op, same precedent as every other single-
  // call-site gap in this table (e.g. 0x138 above).
  uint32_t unknown_0x1c_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  // Real, confirmed gap, resolved: Alien Breaker Deluxe's own real
  // menu-input handling (abd.mod 0x108d98, `blx [runtime_table+0x20]`)
  // reached this slot on a repeated/rapid re-press of the menu confirm
  // button -- the real crash that first surfaced this gap is fixed by
  // the blind-no-op shape alone (that one real call site's own return
  // value is genuinely unused). But the same real slot is also the
  // real single-value sprintf-family formatter behind ABD's real
  // gameplay HUD score/lives/shield text -- see mod_runtime.h's own
  // doc comment (the table's thirty-third slot) for the full real
  // derivation, and `FormatSingleIntImpl` above for the implementation.
  uint32_t unknown_0x20_fn = hle_.Register([this](IArmCore& core) { FormatSingleIntImpl(core); });
  // Real, confirmed gap: Alien Breaker Deluxe's own real ball-spawn
  // sequence (abd.mod 0x109d54, `blx [runtime_table+0xa8]`) reaches this
  // slot exactly once, right as a real new gameplay entity (the ball) is
  // about to be created -- real, reproducible crash a real human hit
  // live starting an actual level (see mod_runtime.h's own doc comment
  // for the full real derivation). Unlike every other safe-no-op slot
  // in this table, this one's real *return value* isn't what the real
  // caller reads -- real code passes `(buffer=sp+36, flag=1)` and reads
  // a real byte back out of `buffer` itself (`ldrb r0, [sp, #36]`), not
  // r0, checked twice (`cmp r0,#0` then `tst r0,#7`) before falling
  // through into what real disassembly shows is entity-creation code
  // right after. A blind "touch nothing" stub would leave that real
  // byte as whatever real stack garbage was already there, making this
  // real check's outcome nondeterministic instead of a clean skip.
  // Writes 0 into the real output buffer explicitly -- both real checks
  // read as "condition not met," matching this table's own established
  // "represent success/false uniformly as zero" convention and letting
  // real execution reach the real entity-creation call that follows,
  // instead of branching into what disassembly shows is a real
  // "skip this candidate, advance to the next linked entity" path.
  uint32_t check_object_flag_0xa8_fn = hle_.Register([](IArmCore& core) {
    uint32_t buffer = core.GetRegister(kR0);
    core.GetMemory().Write8(buffer, 0);
    core.SetRegister(kR0, 0);
  });
  memory_.Write32(table_address + kMemcpySlotOffset, memcpy_fn);
  memory_.Write32(table_address + kMemcpyAliasSlotOffset, memcpy_fn);
  memory_.Write32(table_address + kMemsetSlotOffset, memset_fn);
  memory_.Write32(table_address + kStrlenSlotOffset, strlen_fn);
  memory_.Write32(table_address + kStrcpySlotOffset, strcpy_fn);
  memory_.Write32(table_address + kBoundedStrcpySlotOffset, bounded_strcpy_fn);
  memory_.Write32(table_address + kStrstrSlotOffset, strstr_fn);
  memory_.Write32(table_address + kSprintfSlotOffset, sprintf_fn);
  memory_.Write32(table_address + kMallocSlotOffset, malloc_fn);
  memory_.Write32(table_address + kFreeSlotOffset, free_fn);
  memory_.Write32(table_address + kGetUpTimeMsSlotOffset, get_uptime_ms_fn);
  memory_.Write32(table_address + kGetAppContextSlotOffset, get_app_context_fn);
  memory_.Write32(table_address + kDbgPrintfSlotOffset, dbgprintf_fn);
  memory_.Write32(table_address + kReallocSlotOffset, realloc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x40, unknown_0x40_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xc, unknown_0xc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xd0, unknown_0xd0_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xdc, unknown_0xdc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x184, unknown_0x184_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x1b4, unknown_0x1b4_fn);
  memory_.Write32(table_address + kStrncpySlotOffset, strncpy_fn);
  memory_.Write32(table_address + kStrchrSlotOffset, strchr_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x140, unknown_0x140_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x138, unknown_0x138_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x30, unknown_0x30_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x144, unknown_0x144_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x14c, unknown_0x14c_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x150, unknown_0x150_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x64, unknown_0x64_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xcc, unknown_0xcc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x90, unknown_0x90_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x10, unknown_0x10_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x1c, unknown_0x1c_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x20, unknown_0x20_fn);
  memory_.Write32(table_address + kCheckObjectFlagSlotOffset0xa8, check_object_flag_0xa8_fn);
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
