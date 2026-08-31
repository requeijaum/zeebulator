#include "core/brew/mod_runtime.h"

#include <zlib.h>

#include <vector>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::ModRuntime;

namespace {
constexpr uint32_t kMemcpySlotOffset = 0x0;
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kStrlenSlotOffset = 0x14;
constexpr uint32_t kStrcpySlotOffset = 0x8;
constexpr uint32_t kBoundedStrcpySlotOffset = 0xe4;
constexpr uint32_t kStrncpySlotOffset = 0xc8;
constexpr uint32_t kStrchrSlotOffset = 0x18;
constexpr uint32_t kStrstrSlotOffset = 0xe8;
constexpr uint32_t kSprintfSlotOffset = 0x13c;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
constexpr uint32_t kDbgPrintfSlotOffset = 0x9c;
constexpr uint32_t kMemcpyAliasSlotOffset = 0x44;
constexpr uint32_t kReallocSlotOffset = 0x74;
constexpr uint32_t kUnknownSlotOffset0x1b4 = 0x1b4;
constexpr uint32_t kUnknownSlotOffset0xdc = 0xdc;
constexpr uint32_t kUnknownSlotOffset0x138 = 0x138;
constexpr uint32_t kUnknownSlotOffset0x30 = 0x30;
constexpr uint32_t kUnknownSlotOffset0x144 = 0x144;
constexpr uint32_t kUnknownSlotOffset0x14c = 0x14c;
constexpr uint32_t kUnknownSlotOffset0x150 = 0x150;
constexpr uint32_t kUnknownSlotOffset0x64 = 0x64;
constexpr uint32_t kUnknownSlotOffset0xcc = 0xcc;
constexpr uint32_t kUnknownSlotOffset0x90 = 0x90;
constexpr uint32_t kUnknownSlotOffset0x10 = 0x10;
constexpr uint32_t kUnknownSlotOffset0x1c = 0x1c;
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
constexpr uint32_t kAppContextThirdObjectOffset = 0x2c;
constexpr uint32_t kAppContextFourthObjectOffset = 0x24;
constexpr uint32_t kTableAddress = 0x80280000;
constexpr uint32_t kContextAddress = 0x80280200;
constexpr uint32_t kHeapRegion = 0x80300000;
constexpr uint32_t kModuleBase = 0x00100000;
}  // namespace

TEST(ModRuntime, InstallWritesTablePointerAtModuleBaseMinusFour) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);

  mod_runtime.Install(kModuleBase, kTableAddress);

  EXPECT_EQ(cpu.GetMemory().Read32(kModuleBase - 4), kTableAddress);
}

TEST(ModRuntime, MallocSlotReturnsAddressWithinHeapRegion) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t result = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  EXPECT_EQ(result, kHeapRegion);
}

TEST(ModRuntime, SuccessiveAllocationsDoNotOverlap) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/20);
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, kHeapRegion + 36u);
}

TEST(ModRuntime, AllocationsAreWordAligned) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/1);   // rounds up to 4
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/1);  // should start right after
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, kHeapRegion + 4u);
}

TEST(ModRuntime, ReturnsNullWhenHeapIsExhausted) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/32, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/32);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/4);
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, 0u);
}

TEST(ModRuntime, ReallocGrowsAndPreservesContent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  uint32_t original = hle.CallArmFunction(malloc_fn, /*size=*/4);
  cpu.GetMemory().Write32(original, 0xCAFEF00D);

  uint32_t grown = hle.CallArmFunction(realloc_fn, original, /*size=*/16);
  EXPECT_NE(grown, 0u);
  EXPECT_NE(grown, original) << "this allocator never resizes in place";
  EXPECT_EQ(cpu.GetMemory().Read32(grown), 0xCAFEF00Du) << "original content preserved";
}

TEST(ModRuntime, ReallocReturnsNullWithoutTouchingOldBlockWhenHeapIsExhausted) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/8, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  uint32_t original = hle.CallArmFunction(malloc_fn, /*size=*/4);
  cpu.GetMemory().Write32(original, 0x11223344);

  EXPECT_EQ(hle.CallArmFunction(realloc_fn, original, /*size=*/1000), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(original), 0x11223344u) << "failed realloc leaves the old block alone";
}

TEST(ModRuntime, ReallocWithNullPointerBehavesLikeMalloc) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(realloc_fn, /*ptr=*/0, /*size=*/16), kHeapRegion);
}

TEST(ModRuntime, FreeSlotDoesNotCrash) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t free_fn = cpu.GetMemory().Read32(kTableAddress + kFreeSlotOffset);
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(free_fn, /*ptr=*/kHeapRegion));
}

namespace {
// Writes a real-shaped comparator at `addr`: `int compar(Entity **a, Entity
// **b)` where each `Entity` is just a single 4-byte field at offset 0.
// Every instruction word here is copied directly from the real comparator
// this slot's real call site actually uses (`ddragonz.mod` 0x10b918-
// 0x10b94c, see mod_runtime.h's doc comment for the full derivation) --
// `ldr r0,[r0]` / `ldr r1,[r1]` (deref the slot pointers), `cmp`, `movle
// r0,#1` / `mvngt r0,#0` (the real comparator's own polarity: +1 when the
// first argument sorts before-or-equal the second, -1 otherwise), `bx lr`
// -- just with the real two-field (0x7c then 0x50 tie-break) comparison
// collapsed to a single field at offset 0, since the sort implementation
// itself doesn't know or care what the real comparator compares.
void WriteRealStyleComparator(zeebulator::Memory& memory, uint32_t addr) {
  memory.Write32(addr + 0x00, 0xE5900000);   // ldr r0, [r0]
  memory.Write32(addr + 0x04, 0xE5911000);   // ldr r1, [r1]
  memory.Write32(addr + 0x08, 0xE5902000);   // ldr r2, [r0]
  memory.Write32(addr + 0x0C, 0xE5913000);   // ldr r3, [r1]
  memory.Write32(addr + 0x10, 0xE1520003);   // cmp r2, r3
  memory.Write32(addr + 0x14, 0xD3A00001);   // movle r0, #1
  memory.Write32(addr + 0x18, 0xC3E00000);   // mvngt r0, #0
  memory.Write32(addr + 0x1C, 0xE12FFF1E);   // bx lr
}
}  // namespace

TEST(ModRuntime, Slot0x1b4SortsAPointerArrayDescendingUsingTheRealComparator) {
  // Real disassembly (Double Dragon, TASKS.md Phase 8 -- the sprite
  // z-ordering investigation) shows this slot called as `(base, count,
  // size=4, compar)`, a real generic `SORT` -- not, as first guessed, an
  // "array constructor" helper (see mod_runtime.h's doc comment for the
  // full derivation, including why `size=4` rules that guess out).
  //
  // Final order is *descending* by the real comparator's own "before-
  // or-equal" relation, not ascending -- confirmed live, not guessed
  // (see SortPointerArrayImpl's own comment and PHASE8_LOG.md): the
  // real comparator here (see `WriteRealStyleComparator`) returns +1
  // when its first argument's field is <= its second's, so ascending
  // integer order would be 10,20,30,40 -- the real fix instead produces
  // 40,30,20,10.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sort_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1b4);

  constexpr uint32_t kComparAddr = 0x1000;
  WriteRealStyleComparator(cpu.GetMemory(), kComparAddr);

  // Four "entities" (a single int field each), and an array of four
  // pointers to them in an unsorted order.
  constexpr uint32_t kEntitiesAddr = 0x80300100;  // 4 entities x 4 bytes
  constexpr uint32_t kArrayAddr = 0x80300200;      // 4 pointers x 4 bytes
  const std::vector<int32_t> fields = {30, 10, 40, 20};
  for (size_t i = 0; i < fields.size(); ++i) {
    uint32_t entity_addr = kEntitiesAddr + static_cast<uint32_t>(i) * 4;
    cpu.GetMemory().Write32(entity_addr, static_cast<uint32_t>(fields[i]));
    cpu.GetMemory().Write32(kArrayAddr + static_cast<uint32_t>(i) * 4, entity_addr);
  }

  hle.CallArmFunction(sort_fn, kArrayAddr, /*count=*/4, /*size=*/4, kComparAddr);

  std::vector<int32_t> sorted_fields;
  for (int i = 0; i < 4; ++i) {
    uint32_t entity_addr = cpu.GetMemory().Read32(kArrayAddr + i * 4);
    sorted_fields.push_back(static_cast<int32_t>(cpu.GetMemory().Read32(entity_addr)));
  }
  EXPECT_EQ(sorted_fields, (std::vector<int32_t>{40, 30, 20, 10}));
}

TEST(ModRuntime, Slot0x1b4LeavesAnAlreadyDescendingArrayUnchanged) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sort_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1b4);

  constexpr uint32_t kComparAddr = 0x1000;
  WriteRealStyleComparator(cpu.GetMemory(), kComparAddr);

  constexpr uint32_t kEntitiesAddr = 0x80300100;
  constexpr uint32_t kArrayAddr = 0x80300200;
  const std::vector<int32_t> fields = {3, 2, 1};  // already in the real sort's own final order
  std::vector<uint32_t> entity_addrs;
  for (size_t i = 0; i < fields.size(); ++i) {
    uint32_t entity_addr = kEntitiesAddr + static_cast<uint32_t>(i) * 4;
    cpu.GetMemory().Write32(entity_addr, static_cast<uint32_t>(fields[i]));
    cpu.GetMemory().Write32(kArrayAddr + static_cast<uint32_t>(i) * 4, entity_addr);
    entity_addrs.push_back(entity_addr);
  }

  hle.CallArmFunction(sort_fn, kArrayAddr, /*count=*/3, /*size=*/4, kComparAddr);

  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read32(kArrayAddr + i * 4), entity_addrs[i]);
  }
}

TEST(ModRuntime, Slot0x1b4DoesNothingWhenCountIsZeroOrOne) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sort_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1b4);

  constexpr uint32_t kComparAddr = 0x1000;
  WriteRealStyleComparator(cpu.GetMemory(), kComparAddr);

  constexpr uint32_t kArrayAddr = 0x80300200;
  cpu.GetMemory().Write32(kArrayAddr, 0xDEADBEEF);

  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(sort_fn, kArrayAddr, /*count=*/0, /*size=*/4, kComparAddr));
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(sort_fn, kArrayAddr, /*count=*/1, /*size=*/4, kComparAddr));
  EXPECT_EQ(cpu.GetMemory().Read32(kArrayAddr), 0xDEADBEEFu);
}

TEST(ModRuntime, Slot0x1b4RestoresLrSoItsOwnCallerCanStillReturnCorrectly) {
  // SortPointerArrayImpl calls back into real ARM code (the comparator)
  // via HleRuntime::CallArmFunction, which repurposes LR as its own
  // return sentinel -- if the real slot's own caller's LR isn't
  // restored before returning, whatever invoked this slot in the first
  // place would resume at the wrong address. Simulate a real caller: a
  // tiny ARM function that itself calls the sort slot via `bx`.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sort_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1b4);

  constexpr uint32_t kComparAddr = 0x1000;
  WriteRealStyleComparator(cpu.GetMemory(), kComparAddr);

  constexpr uint32_t kEntitiesAddr = 0x80300100;
  constexpr uint32_t kArrayAddr = 0x80300200;
  const std::vector<int32_t> fields = {5, 1};
  for (size_t i = 0; i < fields.size(); ++i) {
    uint32_t entity_addr = kEntitiesAddr + static_cast<uint32_t>(i) * 4;
    cpu.GetMemory().Write32(entity_addr, static_cast<uint32_t>(fields[i]));
    cpu.GetMemory().Write32(kArrayAddr + static_cast<uint32_t>(i) * 4, entity_addr);
  }

  // Real non-leaf-function calling convention (matches this project's own
  // real disassembly, e.g. `ddragonz.mod` 0x11f8d8's `push {r4, lr}` /
  // 0x11f8c8's matching `pop`): save its own LR on the stack before
  // making a nested call (`mov lr,pc ; bx r5`), restore it after --
  // exactly what real compiled code has to do whenever it calls something
  // that might itself clobber LR, which the real sort call does.
  cpu.SetRegister(zeebulator::kSP, 0x80301000);
  constexpr uint32_t kCallerAddr = 0x1100;
  cpu.GetMemory().Write32(kCallerAddr + 0x00, 0xE92D4010);  // push {r4, lr}
  cpu.GetMemory().Write32(kCallerAddr + 0x04, 0xE3A0402A);  // mov r4, #0x2A
  cpu.GetMemory().Write32(kCallerAddr + 0x08, 0xE59F5010);  // ldr r5, [pc, #16] -> sort_fn
  cpu.GetMemory().Write32(kCallerAddr + 0x0C, 0xE1A0E00F);  // mov lr, pc
  cpu.GetMemory().Write32(kCallerAddr + 0x10, 0xE12FFF15);  // bx r5
  cpu.GetMemory().Write32(kCallerAddr + 0x14, 0xE1A00004);  // mov r0, r4
  cpu.GetMemory().Write32(kCallerAddr + 0x18, 0xE8BD4010);  // pop {r4, lr}
  cpu.GetMemory().Write32(kCallerAddr + 0x1C, 0xE12FFF1E);  // bx lr
  cpu.GetMemory().Write32(kCallerAddr + 0x20, sort_fn);     // literal pool

  uint32_t result = hle.CallArmFunction(kCallerAddr, kArrayAddr, /*count=*/2, /*size=*/4, kComparAddr);
  EXPECT_EQ(result, 0x2Au);
}

TEST(ModRuntime, DbgPrintfSlotDoesNotCrash) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t dbgprintf_fn = cpu.GetMemory().Read32(kTableAddress + kDbgPrintfSlotOffset);
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(dbgprintf_fn, /*dest=*/0, /*count=*/4, /*src=*/0));
}

TEST(ModRuntime, GetAppContextSlotReturnsShellInstanceAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(context, kContextAddress);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr);
}

TEST(ModRuntime, GetAppContextSlotReturnsDisplayInstanceAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kDisplayPtr = 0x80003000;
  mod_runtime.SetDisplayInstance(kDisplayPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextDisplayOffset), kDisplayPtr);
}

TEST(ModRuntime, GetAppContextSlotReturnsThirdObjectAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kThirdObjectPtr = 0x80004000;
  mod_runtime.SetThirdContextObject(kThirdObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextThirdObjectOffset), kThirdObjectPtr);
}

TEST(ModRuntime, GetAppContextSlotReturnsFourthObjectAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kFourthObjectPtr = 0x80005000;
  mod_runtime.SetFourthContextObject(kFourthObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextFourthObjectOffset), kFourthObjectPtr);
}

TEST(ModRuntime, RealCodeWritingAContextFieldDirectlySurvivesASubsequentGetAppContextCall) {
  // Regression test for the bug traced tracing Peggle (TASKS.md Phase
  // 8): real code writes directly into the fourth context field (see
  // the class doc comment), and GetAppContextImpl used to unconditionally
  // rewrite all five fields on every call, silently clobbering that real
  // write the very next time real code called GetAppContext again.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kFourthObjectPtr = 0x80005000;
  mod_runtime.SetFourthContextObject(kFourthObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);

  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  constexpr uint32_t kRealObjectPtr = 0x80300024;
  cpu.GetMemory().Write32(context + kAppContextFourthObjectOffset, kRealObjectPtr);

  hle.CallArmFunction(get_app_context_fn);  // real code's next GetAppContext call
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextFourthObjectOffset), kRealObjectPtr)
      << "a second GetAppContext call must not clobber real code's own direct write";
}

TEST(ModRuntime, SetContextAddressRedirectsGetAppContextToTheNewAddress) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);

  constexpr uint32_t kNewContextAddress = 0x80300024;
  mod_runtime.SetContextAddress(kNewContextAddress);

  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(context, kNewContextAddress);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr)
      << "already-set fields must be re-primed onto the new address";
}

TEST(ModRuntime, SetShellInstanceCanBeCalledAfterInstall) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr);
}

TEST(ModRuntime, GetUpTimeMsStartsAtZeroAndAdvancesWithTick) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_uptime_ms_fn = cpu.GetMemory().Read32(kTableAddress + kGetUpTimeMsSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 0u);
  // Each read above also self-advances the clock by 1ms (see
  // GetUpTimeMsImpl's doc comment) -- account for that alongside Tick's
  // own external advances.
  mod_runtime.Tick(16);
  mod_runtime.Tick(16);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 33u);
}

TEST(ModRuntime, GetUpTimeMsSelfAdvancesOnEveryReadEvenWithoutTick) {
  // A real busy-wait loop that polls elapsed time entirely within a
  // single native HLE call (no opportunity for the outer per-frame
  // Tick() to run in between -- confirmed by real disassembly, see
  // TASKS.md Phase 8) must still see time pass, or it can never
  // observe its own deadline and would spin forever.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_uptime_ms_fn = cpu.GetMemory().Read32(kTableAddress + kGetUpTimeMsSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 0u);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 1u);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 2u);
}

TEST(ModRuntime, MemsetFillsExactlyTheRequestedRangeAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memset_fn = cpu.GetMemory().Read32(kTableAddress + kMemsetSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  cpu.GetMemory().Write8(kDest - 1, 0xAA);  // sentinel just before the range
  cpu.GetMemory().Write8(kDest + 10, 0xAA);  // sentinel just after the range
  // void *memset(void *s, int c, size_t n)
  EXPECT_EQ(hle.CallArmFunction(memset_fn, kDest, /*c=*/0x42, /*n=*/10), kDest);

  for (uint32_t i = 0; i < 10; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + i), 0x42) << "byte " << i;
  }
  EXPECT_EQ(cpu.GetMemory().Read8(kDest - 1), 0xAA) << "wrote before the requested range";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 10), 0xAA) << "wrote past the requested range";
}

TEST(ModRuntime, StrlenReturnsLengthExcludingNullTerminator) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strlen_fn = cpu.GetMemory().Read32(kTableAddress + kStrlenSlotOffset);

  constexpr uint32_t kStr = 0x80300100;
  const char* text = "hello";
  for (size_t i = 0; text[i] != '\0'; ++i) {
    cpu.GetMemory().Write8(kStr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  cpu.GetMemory().Write8(kStr + 5, 0);

  EXPECT_EQ(hle.CallArmFunction(strlen_fn, kStr), 5u);
}

TEST(ModRuntime, StrlenReturnsZeroForEmptyString) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strlen_fn = cpu.GetMemory().Read32(kTableAddress + kStrlenSlotOffset);

  constexpr uint32_t kStr = 0x80300100;
  cpu.GetMemory().Write8(kStr, 0);

  EXPECT_EQ(hle.CallArmFunction(strlen_fn, kStr), 0u);
}

TEST(ModRuntime, BoundedStrcpyCopiesUpToRequestedLength) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t bounded_strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kBoundedStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  const char* text = "hello";
  for (size_t i = 0; i <= 5; ++i) {
    cpu.GetMemory().Write8(kSrc + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }

  EXPECT_EQ(hle.CallArmFunction(bounded_strcpy_fn, kSrc, /*n=*/6, kDest, /*cap=*/0x200), kDest);
  for (size_t i = 0; i <= 5; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + static_cast<uint32_t>(i)),
              static_cast<uint8_t>(text[i]))
        << "byte " << i;
  }
}

TEST(ModRuntime, BoundedStrcpyNeverExceedsCap) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t bounded_strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kBoundedStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  cpu.GetMemory().Write8(kSrc, 0xAB);
  cpu.GetMemory().Write8(kSrc + 1, 0xCD);
  cpu.GetMemory().Write8(kDest + 1, 0x99);  // sentinel: must not be overwritten

  hle.CallArmFunction(bounded_strcpy_fn, kSrc, /*n=*/10, kDest, /*cap=*/1);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest), 0xAB);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), 0x99) << "copied past the cap";
}

TEST(ModRuntime, MemcpyCopiesExactlyTheRequestedRangeAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memcpy_fn = cpu.GetMemory().Read32(kTableAddress + kMemcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  for (uint32_t i = 0; i < 4; ++i) {
    cpu.GetMemory().Write8(kSrc + i, static_cast<uint8_t>(0x10 + i));
  }
  cpu.GetMemory().Write8(kDest - 1, 0xAA);  // sentinel just before the range
  cpu.GetMemory().Write8(kDest + 4, 0xAA);  // sentinel just after the range

  EXPECT_EQ(hle.CallArmFunction(memcpy_fn, kDest, kSrc, /*n=*/4), kDest);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + i), static_cast<uint8_t>(0x10 + i)) << "byte " << i;
  }
  EXPECT_EQ(cpu.GetMemory().Read8(kDest - 1), 0xAA) << "wrote before the requested range";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 4), 0xAA) << "wrote past the requested range";
}

TEST(ModRuntime, MemcpyAliasSlotBehavesIdenticallyToMemcpy) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memcpy_alias_fn = cpu.GetMemory().Read32(kTableAddress + kMemcpyAliasSlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  cpu.GetMemory().Write32(kSrc, 0xCAFEF00D);

  EXPECT_EQ(hle.CallArmFunction(memcpy_alias_fn, kDest, kSrc, /*n=*/4), kDest);
  EXPECT_EQ(cpu.GetMemory().Read32(kDest), 0xCAFEF00Du);
}

TEST(ModRuntime, StrcpyCopiesThroughTheNullTerminatorAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  const char* text = "hi";
  cpu.GetMemory().Write8(kSrc + 0, 'h');
  cpu.GetMemory().Write8(kSrc + 1, 'i');
  cpu.GetMemory().Write8(kSrc + 2, 0);
  cpu.GetMemory().Write8(kDest + 3, 0xAA);  // sentinel just past the null terminator

  EXPECT_EQ(hle.CallArmFunction(strcpy_fn, kDest, kSrc), kDest);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 0), static_cast<uint8_t>(text[0]));
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), static_cast<uint8_t>(text[1]));
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 2), 0u) << "null terminator copied";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 3), 0xAAu) << "didn't write past the terminator";
}

TEST(ModRuntime, StrncpyPadsWithNullsWhenSrcIsShorterThanMaxlen) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strncpy_fn = cpu.GetMemory().Read32(kTableAddress + kStrncpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  cpu.GetMemory().Write8(kSrc + 0, 'h');
  cpu.GetMemory().Write8(kSrc + 1, 'i');
  cpu.GetMemory().Write8(kSrc + 2, 0);
  for (uint32_t i = 0; i < 8; ++i) cpu.GetMemory().Write8(kDest + i, 0xAA);

  EXPECT_EQ(hle.CallArmFunction(strncpy_fn, kDest, kSrc, 5), kDest);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 0), 'h');
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), 'i');
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 2), 0u);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 3), 0u) << "real strncpy pads the remainder with nulls";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 4), 0u);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 5), 0xAAu) << "didn't write past maxlen";
}

TEST(ModRuntime, StrncpyTruncatesWithoutNullTerminatingWhenSrcReachesMaxlen) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strncpy_fn = cpu.GetMemory().Read32(kTableAddress + kStrncpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  const char* text = "helloworld";
  for (size_t i = 0; text[i] != '\0'; ++i) {
    cpu.GetMemory().Write8(kSrc + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  cpu.GetMemory().Write8(kDest + 4, 0xAA);  // sentinel just past maxlen

  EXPECT_EQ(hle.CallArmFunction(strncpy_fn, kDest, kSrc, 4), kDest);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 0), 'h');
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), 'e');
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 2), 'l');
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 3), 'l')
      << "real strncpy doesn't null-terminate when src reaches maxlen";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 4), 0xAAu) << "didn't write past maxlen";
}

TEST(ModRuntime, StrchrFindsAPresentCharacter) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strchr_fn = cpu.GetMemory().Read32(kTableAddress + kStrchrSlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  const char* text = "a+b";
  for (size_t i = 0; text[i] != '\0'; ++i) {
    cpu.GetMemory().Write8(kSrc + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  cpu.GetMemory().Write8(kSrc + 3, 0);

  EXPECT_EQ(hle.CallArmFunction(strchr_fn, kSrc, '+'), kSrc + 1);
}

TEST(ModRuntime, StrchrReturnsNullWhenCharacterIsAbsent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strchr_fn = cpu.GetMemory().Read32(kTableAddress + kStrchrSlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  const char* text = "abc";
  for (size_t i = 0; text[i] != '\0'; ++i) {
    cpu.GetMemory().Write8(kSrc + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  cpu.GetMemory().Write8(kSrc + 3, 0);

  EXPECT_EQ(hle.CallArmFunction(strchr_fn, kSrc, '+'), 0u);
}

TEST(ModRuntime, StrchrOfNullCharacterFindsTheTerminatorItself) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strchr_fn = cpu.GetMemory().Read32(kTableAddress + kStrchrSlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  cpu.GetMemory().Write8(kSrc + 0, 'x');
  cpu.GetMemory().Write8(kSrc + 1, 0);

  EXPECT_EQ(hle.CallArmFunction(strchr_fn, kSrc, 0), kSrc + 1);
}

namespace {
void WriteCString(zeebulator::Memory& memory, uint32_t addr, const char* text) {
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    memory.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory.Write8(addr + static_cast<uint32_t>(i), 0);
}
}  // namespace

TEST(ModRuntime, StrstrFindsANeedlePresentInTheHaystack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "EGL_ARB_foo EGL_QUALCOMM_COLOR_BUFFER EGL_ARB_bar");
  WriteCString(cpu.GetMemory(), kNeedle, "EGL_QUALCOMM_COLOR_BUFFER");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), kHaystack + 12);
}

TEST(ModRuntime, StrstrReturnsNullWhenNeedleIsAbsent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "");  // real eglQueryString never returns null, but
                                                  // may return an empty extensions string
  WriteCString(cpu.GetMemory(), kNeedle, "EGL_QUALCOMM_COLOR_BUFFER");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), 0u);
}

TEST(ModRuntime, StrstrWithEmptyNeedleReturnsHaystack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "anything");
  WriteCString(cpu.GetMemory(), kNeedle, "");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), kHaystack);
}

namespace {
std::string ReadCString(zeebulator::Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}
}  // namespace

TEST(ModRuntime, SprintfFormatsARealConfirmedErrorCodeMessage) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "ERROR CODE:%d");  // real string, see PHASE8_LOG.md
  cpu.GetMemory().Write32(kArgs, 5);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  uint32_t written = hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "ERROR CODE:5");
  EXPECT_EQ(written, 12u);
  EXPECT_EQ(cpu.GetMemory().Read32(kArgsCursor), kArgs + 4) << "cursor advanced past the one arg";
}

TEST(ModRuntime, SprintfSupportsStringHexCharAndLiteralPercent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kStr = 0x80300280;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "%s=%x%% [%c]");
  WriteCString(cpu.GetMemory(), kStr, "hp");
  cpu.GetMemory().Write32(kArgs + 0, kStr);
  cpu.GetMemory().Write32(kArgs + 4, 0xFF);
  cpu.GetMemory().Write32(kArgs + 8, static_cast<uint32_t>('!'));
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "hp=ff% [!]");
}

TEST(ModRuntime, SprintfSupportsARealMinimumFieldWidthAndZeroPadding) {
  // Real Double Dragon HUD text (ddragonz.mod file offset 0x6c0b0,
  // confirmed live via a memory read-watch plus a trap-index count --
  // see PHASE8_LOG.md): "J1 %7d" was rendering completely unsubstituted
  // on screen because this function only ever looked at a single
  // character right after '%' -- the width digit '7' wasn't a
  // recognized directive, so the whole "%7d" fell through to the
  // "unknown directive" fallback and got copied through literally.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "J1 %7d");
  cpu.GetMemory().Write32(kArgs, 3);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "J1       3");

  // Zero-padding, confirmed distinct from space-padding.
  WriteCString(cpu.GetMemory(), kFmt, "%05d");
  cpu.GetMemory().Write32(kArgs, 3);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);
  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "00003");

  // A value already at or past the width is never truncated.
  WriteCString(cpu.GetMemory(), kFmt, "%2d");
  cpu.GetMemory().Write32(kArgs, 12345);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);
  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "12345");
}

TEST(ModRuntime, SprintfWithNoDirectivesCopiesTheLiteralTextUnchanged) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "LOAD ERROR");
  cpu.GetMemory().Write32(kArgsCursor, 0);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "LOAD ERROR");
}

TEST(ModRuntime, DecompressGzipInPlaceSlotDecompressesARealGzipStreamAtTheSameAddress) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t decompress_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0xdc);

  // A real-shaped OBM1 header (core/loader/obm1.h): magic "OI", flag
  // 0x04, bpp 8, width 16, height 8 (both uint16 LE) -- exactly what
  // real code reads via memcpy immediately after this slot returns.
  std::vector<uint8_t> original = {'O', 'I', 0x04, 0x08, 16, 0, 8, 0, 0xAA, 0xBB, 0xCC, 0xDD};
  std::vector<uint8_t> compressed(256);
  z_stream strm{};
  ASSERT_EQ(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                          Z_DEFAULT_STRATEGY),
            Z_OK);
  strm.next_in = original.data();
  strm.avail_in = static_cast<uInt>(original.size());
  strm.next_out = compressed.data();
  strm.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  size_t compressed_size = compressed.size() - strm.avail_out;
  deflateEnd(&strm);

  constexpr uint32_t kBufAddr = 0x80300100;
  for (size_t i = 0; i < compressed_size; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), compressed[i]);
  }

  EXPECT_EQ(hle.CallArmFunction(decompress_fn, kBufAddr), 0u);
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kBufAddr + static_cast<uint32_t>(i)), original[i])
        << "byte " << i;
  }
}

TEST(ModRuntime, DecompressGzipInPlaceSlotHandlesInputLargerThanOneChunk) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t decompress_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0xdc);

  // Larger than the implementation's internal 4096-byte streaming
  // chunk size, and incompressible (random-ish, not all-zero) so the
  // real compressed stream is also larger than one chunk -- exercises
  // both the growable-input and growable-output loop paths.
  std::vector<uint8_t> original(10000);
  for (size_t i = 0; i < original.size(); ++i) {
    original[i] = static_cast<uint8_t>((i * 2654435761u) >> 24);
  }
  std::vector<uint8_t> compressed(original.size() + 1024);
  z_stream strm{};
  ASSERT_EQ(deflateInit2(&strm, Z_NO_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY),
            Z_OK);
  strm.next_in = original.data();
  strm.avail_in = static_cast<uInt>(original.size());
  strm.next_out = compressed.data();
  strm.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  size_t compressed_size = compressed.size() - strm.avail_out;
  deflateEnd(&strm);
  ASSERT_GT(compressed_size, 4096u) << "test fixture didn't actually exceed one chunk";

  constexpr uint32_t kBufAddr = 0x80300100;
  for (size_t i = 0; i < compressed_size; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), compressed[i]);
  }

  EXPECT_EQ(hle.CallArmFunction(decompress_fn, kBufAddr), 0u);
  for (size_t i = 0; i < original.size(); ++i) {
    ASSERT_EQ(cpu.GetMemory().Read8(kBufAddr + static_cast<uint32_t>(i)), original[i])
        << "byte " << i;
  }
}

TEST(ModRuntime, UnknownSlot0x138IsWiredAndSafelyReturnsZero) {
  // Found in Alien Breaker Deluxe (TASKS.md): left unregistered, real
  // code's own `blx` through this table slot jumped through a null
  // function pointer. Only one real call site found so far, and its
  // own real identity isn't confirmed -- registered as a safe no-op
  // (matching the fifteenth/twenty-third slots' own precedent), not
  // guessed at.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x138);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x144IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round as slot
  // 0x30: a real call shape that looks sprintf-adjacent (dest, size,
  // fmt) but doesn't match this file's own confirmed SprintfImpl
  // convention (dest, fmt, ppArgs) -- registered as a safe no-op
  // rather than risk silently wrong behavior from reusing SprintfImpl.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x144);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x14cIsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round: appeared
  // the moment slot 0x144 got fixed, in the same tight 0x138-0x150
  // cluster. Calling convention not confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x14c);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x150IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round: appeared
  // the moment slot 0x14c got fixed, same tight cluster. Calling
  // convention not confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x150);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x64IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round, one gap
  // deeper still (5288 real steps in): a real 4-argument call
  // immediately before the confirmed MALLOC slot at 0x68. Calling
  // convention not confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x64);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0xccIsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round, immediately
  // after the confirmed STRNCPY slot at 0xc8. Calling convention not
  // confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0xcc);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x90IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md), same round, immediately
  // before the confirmed DBGPRINTF slot at 0x9c. Calling convention
  // not confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x90);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x10IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md): the last real gap this
  // title's own boot sequence needed -- confirmed via a temporary,
  // exhaustive diagnostic (every still-unmapped table offset filled
  // with a logging no-op) that registering this one slot was enough
  // to reach and stay in the real per-tick event loop for a full
  // 20-second run with no further gaps. Calling convention not
  // confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x10);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x1cIsWiredAndSafelyReturnsZero) {
  // Found in Alien Breaker Deluxe (TASKS.md), reached only after the
  // IShellHle slot-43 stateful/toggle fix let its own real per-object
  // init loop run to completion: real code's own `blx [table+0x1c]`
  // (two real call sites, `abd.mod` 0x106150/0x10619c) jumps through a
  // null function pointer once real per-object init work is done.
  // Calling convention not confirmed -- safe no-op.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1c);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}

TEST(ModRuntime, UnknownSlot0x30IsWiredAndSafelyReturnsZero) {
  // Found in Disney All Star Cards (TASKS.md): left unregistered, real
  // code's own `blx` through this table slot -- called directly from
  // HandleEvent(EVT_APP_START), 88 real steps in -- jumped through a
  // null function pointer. Only one real call site found so far, and
  // its own real identity isn't confirmed -- registered as a safe
  // no-op (matching the fifteenth/twenty-third/twenty-fourth slots'
  // own precedent), not guessed at.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t unknown_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x30);

  EXPECT_NE(unknown_fn, 0u) << "slot must be wired to a real trap, not left as a null pointer";
  EXPECT_EQ(hle.CallArmFunction(unknown_fn, 0x1234, 0x5678), 0u);
}
