#include "core/loader/mod.h"

namespace zeebulator {

void LoadMod(IArmCore& core, const std::vector<uint8_t>& mod_data,
             uint32_t base_address) {
  core.GetMemory().Load(base_address, mod_data.data(), mod_data.size());
  core.SetRegister(kPC, base_address);
}

}  // namespace zeebulator
