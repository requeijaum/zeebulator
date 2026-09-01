#include "core/brew/interface_object.h"

#include <cstdio>

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

uint32_t BuildInterfaceObjectLabeled(
    Memory& memory, HleRuntime& hle, uint32_t vtable_address,
    uint32_t object_address,
    const std::vector<HleRuntime::HleFunction>& methods,
    const std::string& interface_name,
    const std::vector<const char*>& method_names) {
  for (size_t i = 0; i < methods.size(); ++i) {
    char buf[128];
    const char* mname =
        (i < method_names.size() && method_names[i]) ? method_names[i] : nullptr;
    if (mname) {
      std::snprintf(buf, sizeof(buf), "%s::slot%zu %s", interface_name.c_str(),
                    i, mname);
    } else {
      std::snprintf(buf, sizeof(buf), "%s::slot%zu", interface_name.c_str(), i);
    }
    uint32_t sentinel = hle.RegisterLabeled(methods[i], std::string(buf));
    memory.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
  memory.Write32(object_address, vtable_address);
  return object_address;
}

}  // namespace zeebulator
