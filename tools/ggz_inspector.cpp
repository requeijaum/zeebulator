// Dev tool: lists (and optionally extracts) the contents of a GGZ
// archive. Takes a path at runtime -- never embeds any game content into
// the repo. See core/loader/ggz.h for the format writeup.

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/loader/ggz.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file.ggz> [--extract <out_dir>]\n", argv[0]);
    return 1;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

  zeebulator::GgzArchive archive;
  try {
    archive = zeebulator::GgzArchive::Parse(std::move(data));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: failed to parse GGZ: %s\n", e.what());
    return 1;
  }

  std::printf("%zu entries:\n", archive.Entries().size());

  bool extract = argc >= 4 && std::string(argv[2]) == "--extract";
  std::string out_dir = extract ? argv[3] : "";

  size_t ok = 0, fail = 0;
  for (const auto& entry : archive.Entries()) {
    try {
      auto content = archive.Extract(entry);
      ++ok;
      std::printf("  %-40s offset=%-10u size=%-10u %s\n", entry.name.c_str(),
                  entry.offset, entry.decompressed_size,
                  extract ? "[extracted]" : "");
      if (extract) {
        std::ofstream out(out_dir + "/" + entry.name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(content.data()),
                  static_cast<std::streamsize>(content.size()));
      }
    } catch (const std::exception& e) {
      ++fail;
      std::printf("  %-40s FAILED: %s\n", entry.name.c_str(), e.what());
    }
  }

  std::printf("\n%zu extracted OK, %zu failed\n", ok, fail);
  return fail == 0 ? 0 : 1;
}
