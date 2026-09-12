#include "core/memory/memory.h"

#include <sstream>

#include <gtest/gtest.h>

using zeebulator::Memory;

TEST(Memory, UnmappedReadsAreZero) {
  Memory mem;
  EXPECT_EQ(mem.Read8(0x1000), 0);
  EXPECT_EQ(mem.Read32(0x2000), 0u);
}

TEST(Memory, Write8ThenRead8RoundTrips) {
  Memory mem;
  mem.Write8(0x100, 0xAB);
  EXPECT_EQ(mem.Read8(0x100), 0xAB);
}

TEST(Memory, Write32IsLittleEndian) {
  Memory mem;
  mem.Write32(0x200, 0x11223344);
  EXPECT_EQ(mem.Read8(0x200), 0x44);
  EXPECT_EQ(mem.Read8(0x201), 0x33);
  EXPECT_EQ(mem.Read8(0x202), 0x22);
  EXPECT_EQ(mem.Read8(0x203), 0x11);
  EXPECT_EQ(mem.Read32(0x200), 0x11223344u);
}

TEST(Memory, Write16RoundTrips) {
  Memory mem;
  mem.Write16(0x300, 0xBEEF);
  EXPECT_EQ(mem.Read16(0x300), 0xBEEF);
}

TEST(Memory, AccessSpanningPageBoundaryRoundTrips) {
  Memory mem;
  // kPageSize = 4096, so address 4094 straddles two pages for a 4-byte access.
  uint32_t address = Memory::kPageSize - 2;
  mem.Write32(address, 0xCAFEBABE);
  EXPECT_EQ(mem.Read32(address), 0xCAFEBABEu);
}

TEST(Memory, LoadCopiesBytesIntoAddressSpace) {
  Memory mem;
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  mem.Load(0x400, data, sizeof(data));
  for (size_t i = 0; i < sizeof(data); ++i) {
    EXPECT_EQ(mem.Read8(0x400 + static_cast<uint32_t>(i)), data[i]);
  }
}

TEST(Memory, UntouchedPagesRemainZero) {
  Memory mem;
  mem.Write8(0x500, 0x7F);
  EXPECT_EQ(mem.Read8(0x501), 0);
  EXPECT_EQ(mem.Read8(0x4FF), 0);
}

TEST(Memory, SerializeThenDeserializeRoundTripsAllAllocatedPages) {
  Memory mem;
  mem.Write32(0x2000, 0xCAFEBABE);
  mem.Write8(0x9000, 0x42);  // a second, non-adjacent page
  mem.Write32(Memory::kPageSize - 2, 0xDEADBEEF);  // straddles two pages

  std::stringstream stream;
  ASSERT_TRUE(mem.Serialize(stream));

  Memory restored;
  restored.Write8(0x1, 0xFF);  // pre-existing content Deserialize must discard
  ASSERT_TRUE(restored.Deserialize(stream));

  EXPECT_EQ(restored.Read32(0x2000), 0xCAFEBABEu);
  EXPECT_EQ(restored.Read8(0x9000), 0x42);
  EXPECT_EQ(restored.Read32(Memory::kPageSize - 2), 0xDEADBEEFu);
  EXPECT_EQ(restored.Read8(0x1), 0) << "Deserialize should replace, not merge";
}

TEST(Memory, SerializeOfAnEmptyMemoryDeserializesToAllZeros) {
  Memory mem;
  std::stringstream stream;
  ASSERT_TRUE(mem.Serialize(stream));

  Memory restored;
  ASSERT_TRUE(restored.Deserialize(stream));
  EXPECT_EQ(restored.Read32(0x1234), 0u);
}

TEST(Memory, DeserializeFromATruncatedStreamFails) {
  Memory mem;
  mem.Write8(0x100, 0xAB);
  std::stringstream stream;
  ASSERT_TRUE(mem.Serialize(stream));

  std::string truncated = stream.str().substr(0, 5);  // well short of one full page
  std::stringstream truncated_stream(truncated);
  Memory restored;
  EXPECT_FALSE(restored.Deserialize(truncated_stream));
}

// Phase-1 media-interface binding guard. Models the proven DD bug: the
// game binds an HLE-owned media interface (a pointer into the media
// object region) into a media_source+8 slot, then its own Release helper
// zeroes that slot before the input-gated Play path reads it. The guard
// must keep the binding alive across the Release-clear so Play's
// vtable[6] call does not dereference NULL. See
// research/sources/2026-08-31_dd-media-interface-contract.md.
TEST(Memory, MediaBindingGuardSurvivesReleaseClear) {
  Memory mem;
  constexpr uint32_t kRegionStart = 0x80200000;
  constexpr uint32_t kRegionEnd = 0x80300000;
  constexpr uint32_t kMediaSourcePlus8 = 0x803007c0;  // real DD slot addr
  constexpr uint32_t kHleMediaObj = 0x802000c0;        // real DD MediaHle obj
  mem.SetMediaBindingGuardRegion(kRegionStart, kRegionEnd);

  // 1) HLE binds the real media interface into the slot (trampoline
  //    0xf000019c in the live trace).
  mem.Write32(kMediaSourcePlus8, kHleMediaObj);
  EXPECT_EQ(mem.Read32(kMediaSourcePlus8), kHleMediaObj);

  // 2) The game's Release helper (0x11f424) tries to zero the slot.
  mem.Write32(kMediaSourcePlus8, 0);

  // 3) The binding must survive so the (later) Play path finds it.
  EXPECT_EQ(mem.Read32(kMediaSourcePlus8), kHleMediaObj)
      << "Release-clear of a live HLE media binding must be suppressed";
}

TEST(Memory, MediaBindingGuardAllowsRebindAndIsSlotScoped) {
  Memory mem;
  constexpr uint32_t kRegionStart = 0x80200000;
  constexpr uint32_t kRegionEnd = 0x80300000;
  constexpr uint32_t kSlot = 0x803007c0;
  constexpr uint32_t kObjA = 0x80200040;
  constexpr uint32_t kObjB = 0x80200080;
  mem.SetMediaBindingGuardRegion(kRegionStart, kRegionEnd);

  // Genuine rebinding to a different media object is always honored.
  mem.Write32(kSlot, kObjA);
  mem.Write32(kSlot, kObjB);
  EXPECT_EQ(mem.Read32(kSlot), kObjB);

  // The guard only protects slots that actually hold a media binding;
  // an ordinary slot clears normally.
  constexpr uint32_t kOrdinary = 0x80305000;
  mem.Write32(kOrdinary, 0x12345678);
  mem.Write32(kOrdinary, 0);
  EXPECT_EQ(mem.Read32(kOrdinary), 0u);
}

TEST(Memory, MediaBindingGuardDisabledByDefault) {
  Memory mem;  // no SetMediaBindingGuardRegion -> region (0,0) = off
  constexpr uint32_t kSlot = 0x803007c0;
  mem.Write32(kSlot, 0x802000c0);
  mem.Write32(kSlot, 0);
  EXPECT_EQ(mem.Read32(kSlot), 0u) << "guard must be opt-in; default is passthrough";
}

TEST(Memory, DeserializeRejectsImpossiblePageCountBeforeReserve) {
  zeebulator::Memory memory;
  std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
  const uint32_t count = 0xffffffffu;
  in.write(reinterpret_cast<const char*>(&count), sizeof(count));
  in.seekg(0);
  EXPECT_FALSE(memory.Deserialize(in));
}
