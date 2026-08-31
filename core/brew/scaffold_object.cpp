#include "core/brew/scaffold_object.h"

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }
}  // namespace

uint32_t BuildGenericStubObject(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                 uint32_t object_address, size_t slot_count) {
  std::vector<HleRuntime::HleFunction> methods(slot_count, Stub);
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

uint32_t BuildStubObjectWithOverride(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                      uint32_t object_address, size_t slot_count,
                                      size_t override_slot, HleRuntime::HleFunction override_fn) {
  std::vector<HleRuntime::HleFunction> methods(slot_count, Stub);
  methods.at(override_slot) = std::move(override_fn);
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

uint32_t BuildGenericRelativeVtableStubObject(Memory& memory, HleRuntime& hle,
                                               uint32_t vtable_address,
                                               uint32_t object_address, size_t slot_count) {
  for (size_t i = 0; i < slot_count; ++i) {
    uint32_t sentinel = hle.Register(Stub);
    uint32_t slot_address = vtable_address + static_cast<uint32_t>(i) * 4;
    memory.Write32(slot_address, sentinel - vtable_address);
  }
  memory.Write32(object_address, vtable_address);
  return object_address;
}

}  // namespace zeebulator
