// Dev tool: decodes a real ".obm1" sprite/texture asset (see
// core/loader/obm1.h for the format writeup) and writes it out as a
// trivial, dependency-free PPM (P6) image for inspection. Takes a path at
// runtime -- never embeds any game content into the repo.

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/loader/obm1.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <file.obm1> <out.ppm>\n", argv[0]);
    return 1;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

  zeebulator::DecodedImage image;
  try {
    image = zeebulator::Obm1Image::Decode(data);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: failed to decode OBM1: %s\n", e.what());
    return 1;
  }

  std::printf("%ux%u, %zu bytes RGB888\n", image.width, image.height, image.rgb.size());

  std::ofstream out(argv[2], std::ios::binary);
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(image.rgb.data()),
            static_cast<std::streamsize>(image.rgb.size()));
  return 0;
}
