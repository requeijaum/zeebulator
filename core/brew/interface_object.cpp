#include "core/brew/interface_object.h"

namespace zeebulator {

uint32_t BuildInterfaceObject(Memory& memory, HleRuntime& hle,
                               uint32_t vtable_address, uint32_t object_address,
                               const std::vector<HleRuntime::HleFunction>& methods) {
  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle.Register(methods[i]);
    memory.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
  memory.Write32(object_address, vtable_address);
  return object_address;
}

}  // namespace zeebulator
