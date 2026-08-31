#include "core/loader/mif.h"

namespace zeebulator {

std::vector<MifString> ExtractMifStrings(const uint8_t* data, size_t size) {
  std::vector<MifString> results;
  size_t i = 0;
  while (i + 1 < size) {
    if (data[i] != 0xFF || data[i + 1] != 0xFE) {
      ++i;
      continue;
    }

    size_t start = i;
    size_t j = i + 2;
    std::string text;
    bool clean = true;
    while (j + 1 < size) {
      uint16_t code = static_cast<uint16_t>(data[j]) |
                       (static_cast<uint16_t>(data[j + 1]) << 8);
      if (code == 0) {
        j += 2;
        break;
      }
      if (code == 0xFEFF) break;  // next BOM, no null terminator here
      if (code < 32 || code > 126) {
        clean = false;
      } else {
        text.push_back(static_cast<char>(code));
      }
      j += 2;
    }

    if (clean && !text.empty()) {
      results.push_back(MifString{static_cast<uint32_t>(start), text});
    }
    i = j;
  }
  return results;
}

}  // namespace zeebulator
