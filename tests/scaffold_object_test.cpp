#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/brew/scaffold_object.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;

namespace {
constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x1000;
constexpr uint32_t kVtableAddr = 0x80000000;
constexpr uint32_t kObjectAddr = 0x80001000;
}  // namespace

TEST(ScaffoldObject, ObjectHeaderPointsAtVtable) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  uint32_t obj =
      zeebulator::BuildGenericStubObject(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr, 40);

  EXPECT_EQ(obj, kObjectAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kObjectAddr), kVtableAddr);
}

TEST(ScaffoldObject, EveryRequestedSlotIsCallableAndReturnsZero) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  constexpr size_t kSlotCount = 40;
  uint32_t obj = zeebulator::BuildGenericStubObject(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr,
                                                     kSlotCount);

  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    EXPECT_EQ(hle.CallArmFunction(sentinel, obj, /*arg1=*/0x1234, /*arg2=*/0x5678), 0u)
        << "slot " << slot;
  }
}

TEST(ScaffoldObject, OverrideSlotRunsTheSuppliedImplementation) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  uint32_t obj = zeebulator::BuildStubObjectWithOverride(
      cpu.GetMemory(), hle, kVtableAddr, kObjectAddr, /*slot_count=*/10, /*override_slot=*/2,
      [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0x42); });

  uint32_t override_sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  EXPECT_EQ(hle.CallArmFunction(override_sentinel, obj), 0x42u);

  uint32_t other_sentinel = cpu.GetMemory().Read32(kVtableAddr + 3 * 4);
  EXPECT_EQ(hle.CallArmFunction(other_sentinel, obj), 0u) << "non-overridden slots stay generic";
}

TEST(ScaffoldObject, RelativeVtableObjectHeaderPointsAtVtable) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  uint32_t obj = zeebulator::BuildGenericRelativeVtableStubObject(cpu.GetMemory(), hle, kVtableAddr,
                                                                   kObjectAddr, 4);

  EXPECT_EQ(obj, kObjectAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kObjectAddr), kVtableAddr);
}

TEST(ScaffoldObject, RelativeVtableSlotResolvesThroughTheRealRopiFormula) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  constexpr size_t kSlotCount = 4;
  uint32_t obj = zeebulator::BuildGenericRelativeVtableStubObject(cpu.GetMemory(), hle, kVtableAddr,
                                                                   kObjectAddr, kSlotCount);

  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    uint32_t stored_offset = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    // Real ARM RVCT ROPI virtual-call formula: target = vtable_base +
    // stored_offset, not the stored value directly (see the header's
    // doc comment for the real disassembly this matches).
    uint32_t resolved_target = kVtableAddr + stored_offset;
    EXPECT_EQ(hle.CallArmFunction(resolved_target, obj, /*arg1=*/0xAAAA), 0u) << "slot " << slot;
  }
}
