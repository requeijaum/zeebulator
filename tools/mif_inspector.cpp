// Dev tool: prints the human-readable strings found in a MIF file (app
// name, publisher, version, ...). Takes a path at runtime -- never embeds
// game content into the repo. See core/loader/mif.h for what is and
// isn't understood about the format yet.

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/loader/mif.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file.mif>\n", argv[0]);
    return 1;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

  auto strings = zeebulator::ExtractMifStrings(data.data(), data.size());
  std::printf("%zu string(s) found:\n", strings.size());
  for (const auto& s : strings) {
    std::printf("  @0x%04x: %s\n", s.offset, s.text.c_str());
  }
  return 0;
}
