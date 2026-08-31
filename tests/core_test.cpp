#include "core/core.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(Core, VersionStringIsNonEmpty) {
  const char* version = zeebulator::VersionString();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}
