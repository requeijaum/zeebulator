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
