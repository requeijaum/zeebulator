#include "core/brew/compat/title_quirks.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using zeebulator::compat::BootStatus;
using zeebulator::compat::BootStatusName;
using zeebulator::compat::FindByClsid;
using zeebulator::compat::FindByFolder;
using zeebulator::compat::KnownTitles;

namespace {

TEST(TitleQuirks, RegistryIsNonEmptyAndWellFormed) {
  const auto& titles = KnownTitles();
  ASSERT_FALSE(titles.empty());
  EXPECT_EQ(titles.size(), 10u);
  for (const auto& t : titles) {
    EXPECT_NE(t.real_clsid, 0u) << t.display_name << " must have a real clsid";
    EXPECT_FALSE(t.folder.empty()) << t.display_name << " must have a folder";
    EXPECT_FALSE(t.display_name.empty());
    EXPECT_FALSE(t.mod_filename.empty());
    EXPECT_FALSE(t.evidence.empty()) << t.display_name << " must cite evidence";
  }
}

TEST(TitleQuirks, RealClsidsAreUnique) {
  std::set<uint32_t> seen;
  for (const auto& t : KnownTitles()) {
    EXPECT_TRUE(seen.insert(t.real_clsid).second)
        << "duplicate real clsid " << std::hex << t.real_clsid;
  }
}

TEST(TitleQuirks, FoldersAreUnique) {
  std::set<std::string> seen;
  for (const auto& t : KnownTitles()) {
    EXPECT_TRUE(seen.insert(t.folder).second)
        << "duplicate folder " << t.folder;
  }
}

TEST(TitleQuirks, DoubleDragonLookupByClsid) {
  auto dd = FindByClsid(0x0102F789u);  // 16971657
  ASSERT_TRUE(dd.has_value());
  EXPECT_EQ(dd->folder, "274754");
  EXPECT_EQ(dd->mod_filename, "ddragonz.mod");
  EXPECT_EQ(dd->status, BootStatus::kPlayable);
  // DD's MIF id happens to equal its real id.
  EXPECT_EQ(dd->mif_clsid, dd->real_clsid);
}

TEST(TitleQuirks, AlienBreakerRealClsidDiffersFromMifDecoy) {
  auto abd = FindByClsid(0x0108E356u);  // 17359702 -- traced value
  ASSERT_TRUE(abd.has_value());
  EXPECT_EQ(abd->folder, "279369");
  EXPECT_EQ(abd->status, BootStatus::kInGame);
  // The whole point: MIF advertises a DECOY id, not the real one.
  EXPECT_EQ(abd->mif_clsid, 0x0103081Du);  // 16975901
  EXPECT_NE(abd->mif_clsid, abd->real_clsid);
  // And the MIF decoy must NOT resolve as a real title.
  EXPECT_FALSE(FindByClsid(0x0103081Du).has_value());
}

TEST(TitleQuirks, LookupByFolder) {
  auto abd = FindByFolder("279369");
  ASSERT_TRUE(abd.has_value());
  EXPECT_EQ(abd->real_clsid, 0x0108E356u);
  EXPECT_FALSE(FindByFolder("000000").has_value());
}

TEST(TitleQuirks, BootStatusNamesAreStable) {
  EXPECT_STREQ(BootStatusName(BootStatus::kUntested), "untested");
  EXPECT_STREQ(BootStatusName(BootStatus::kLoadFail), "load-fail");
  EXPECT_STREQ(BootStatusName(BootStatus::kCreateInstanceFail),
               "createinstance-fail");
  EXPECT_STREQ(BootStatusName(BootStatus::kBoots), "boots");
  EXPECT_STREQ(BootStatusName(BootStatus::kInGame), "in-game");
  EXPECT_STREQ(BootStatusName(BootStatus::kPlayable), "playable");
}

}  // namespace
