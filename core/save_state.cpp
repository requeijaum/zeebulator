#include "core/save_state.h"

#include <cstdint>

namespace zeebulator {

namespace {

// Arbitrary 4-byte tag, not meaningful beyond "is this actually one of
// our own save files" -- ASCII "ZBSS" (Zeebulator Save State).
constexpr uint32_t kMagic = 0x5A425353u;
// Bumped whenever the on-disk layout changes in a way old readers can't
// just ignore -- stage 1's own layout is version 1. A stage-2 reader
// can still recognize (and choose to accept or reject) a version-1
// file by checking this field before assuming stage-2's own fields are
// present.
constexpr uint32_t kVersion = 1;

}  // namespace

bool SaveState(const ArmInterpreter& cpu, std::ostream& out) {
  out.write(reinterpret_cast<const char*>(&kMagic), sizeof(kMagic));
  out.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));
  if (!out.good()) return false;
  return cpu.Serialize(out);
}

bool LoadState(ArmInterpreter& cpu, std::istream& in) {
  uint32_t magic = 0;
  uint32_t version = 0;
  in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  in.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (!in.good() || magic != kMagic || version != kVersion) return false;
  return cpu.Deserialize(in);
}

}  // namespace zeebulator
