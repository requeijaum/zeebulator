// Dev tool: lists (and optionally extracts) the contents of a real
// Zeebo/PopCap ".bar" resource archive. Takes a path at runtime -- never
// embeds any game content into the repo. See core/loader/bar.h for the
// format writeup.

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/loader/bar.h"

namespace {

// Best-effort real-format sniff for display purposes only -- BarArchive
// itself doesn't interpret entry content at all (see core/loader/bar.h).
const char* GuessKind(const std::vector<uint8_t>& data) {
  if (data.size() >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
    return "wav";
  }
  if (data.size() >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') return "mp3";
  if (data.size() >= 2) {
    uint16_t header_len = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    if (header_len > 2 && header_len < 64 && data.size() > header_len) {
      bool printable = true;
      for (size_t i = 2; i < static_cast<size_t>(header_len) - 1; ++i) {
        if (data[i] < 32 || data[i] > 126) {
          printable = false;
          break;
        }
      }
      if (printable && data[header_len - 1] == 0) return "mime-wrapped (likely image)";
    }
  }
  return "unknown (raw texture/string/other)";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <resources.bar> [--extract <out_dir>]\n", argv[0]);
    return 1;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

  zeebulator::BarArchive archive;
  try {
    archive = zeebulator::BarArchive::Parse(std::move(data));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: failed to parse BAR: %s\n", e.what());
    return 1;
  }

  std::printf("%zu entries:\n", archive.Entries().size());

  bool extract = argc >= 4 && std::string(argv[2]) == "--extract";
  std::string out_dir = extract ? argv[3] : "";

  for (size_t i = 0; i < archive.Entries().size(); ++i) {
    const auto& entry = archive.Entries()[i];
    auto content = archive.Extract(entry);
    std::printf("  [%3zu] offset=%-10u size=%-10u kind=%s\n", i, entry.offset, entry.size,
                GuessKind(content));
    if (extract) {
      std::ofstream out(out_dir + "/" + std::to_string(i) + ".bin", std::ios::binary);
      out.write(reinterpret_cast<const char*>(content.data()),
                static_cast<std::streamsize>(content.size()));
    }
  }

  std::printf("%zu resource-ID directory records:\n", archive.ResourceIds().size());
  for (const auto& res_id : archive.ResourceIds()) {
    std::printf("  type=%-5u id=%-6u unknown=%-5u -> entry %u\n", res_id.type, res_id.requested_id,
                res_id.unknown, res_id.entry_index);
  }
  return 0;
}
