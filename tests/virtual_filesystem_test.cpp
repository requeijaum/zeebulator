#include "core/brew/virtual_filesystem.h"

#include <zlib.h>

#include <gtest/gtest.h>

using zeebulator::BuildVirtualFilesystemFromGgz;
using zeebulator::GgzArchive;
using zeebulator::VirtualFilesystem;

TEST(VirtualFilesystem, AddFileThenFindRoundTrips) {
  VirtualFilesystem vfs;
  std::vector<uint8_t> data = {1, 2, 3};
  vfs.AddFile("foo.bin", data);

  EXPECT_TRUE(vfs.Exists("foo.bin"));
  ASSERT_NE(vfs.Find("foo.bin"), nullptr);
  EXPECT_EQ(*vfs.Find("foo.bin"), data);
}

TEST(VirtualFilesystem, MissingFileIsAbsent) {
  VirtualFilesystem vfs;
  EXPECT_FALSE(vfs.Exists("nope.bin"));
  EXPECT_EQ(vfs.Find("nope.bin"), nullptr);
}

TEST(VirtualFilesystem, NamesPreservesInsertionOrderWithoutDuplicates) {
  VirtualFilesystem vfs;
  vfs.AddFile("a.bin", {1});
  vfs.AddFile("b.bin", {2});
  vfs.AddFile("a.bin", {9});  // overwrite, should not duplicate the name

  ASSERT_EQ(vfs.Names().size(), 2u);
  EXPECT_EQ(vfs.Names()[0], "a.bin");
  EXPECT_EQ(vfs.Names()[1], "b.bin");
  ASSERT_NE(vfs.Find("a.bin"), nullptr);
  EXPECT_EQ((*vfs.Find("a.bin"))[0], 9) << "overwrite replaces content";
}

namespace {

std::vector<uint8_t> MakeGzipMember(const std::string& filename,
                                     const std::vector<uint8_t>& payload) {
  z_stream strm{};
  deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  std::vector<char> name_buf(filename.begin(), filename.end());
  name_buf.push_back('\0');
  gz_header header{};
  header.name = reinterpret_cast<Bytef*>(name_buf.data());
  header.name_max = static_cast<uInt>(name_buf.size());
  deflateSetHeader(&strm, &header);

  std::vector<uint8_t> out(payload.size() + 256);
  strm.next_in = const_cast<Bytef*>(payload.data());
  strm.avail_in = static_cast<uInt>(payload.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());
  deflate(&strm, Z_FINISH);
  out.resize(out.size() - strm.avail_out);
  deflateEnd(&strm);
  return out;
}

std::vector<uint8_t> BuildGgz(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<std::vector<uint8_t>> streams;
  for (const auto& f : files) streams.push_back(MakeGzipMember(f.first, f.second));

  uint32_t table_len = static_cast<uint32_t>(files.size() * 8);
  std::vector<uint32_t> offsets;
  uint32_t cursor = table_len;
  for (const auto& s : streams) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(s.size());
  }

  std::vector<uint8_t> blob(cursor, 0);
  for (size_t i = 0; i < files.size(); ++i) {
    uint32_t off = offsets[i];
    uint32_t sz = static_cast<uint32_t>(files[i].second.size());
    size_t p = i * 8;
    blob[p] = static_cast<uint8_t>(off >> 24);
    blob[p + 1] = static_cast<uint8_t>(off >> 16);
    blob[p + 2] = static_cast<uint8_t>(off >> 8);
    blob[p + 3] = static_cast<uint8_t>(off);
    blob[p + 4] = static_cast<uint8_t>(sz >> 24);
    blob[p + 5] = static_cast<uint8_t>(sz >> 16);
    blob[p + 6] = static_cast<uint8_t>(sz >> 8);
    blob[p + 7] = static_cast<uint8_t>(sz);
  }
  for (size_t i = 0; i < streams.size(); ++i) {
    std::copy(streams[i].begin(), streams[i].end(), blob.begin() + offsets[i]);
  }
  return blob;
}

}  // namespace

TEST(VirtualFilesystem, BuildFromGgzPopulatesAllEntries) {
  std::vector<uint8_t> texture_data = {0xDE, 0xAD, 0xBE, 0xEF};
  std::vector<uint8_t> sound_data(500, 0x7F);
  auto blob = BuildGgz({{"texture.obm1", texture_data}, {"sound.wav", sound_data}});
  auto archive = GgzArchive::Parse(blob);

  auto vfs = BuildVirtualFilesystemFromGgz(archive);

  ASSERT_EQ(vfs.Names().size(), 2u);
  EXPECT_TRUE(vfs.Exists("texture.obm1"));
  EXPECT_TRUE(vfs.Exists("sound.wav"));
  ASSERT_NE(vfs.Find("texture.obm1"), nullptr);
  EXPECT_EQ(*vfs.Find("texture.obm1"), texture_data);
  ASSERT_NE(vfs.Find("sound.wav"), nullptr);
  EXPECT_EQ(*vfs.Find("sound.wav"), sound_data);
}
