#include <gtest/gtest.h>

#include "OtaVersion.h"

using ota_version::forkBuild;
using ota_version::isNewer;

TEST(OtaVersion, ForkBuildParsesSuffix) {
  EXPECT_EQ(forkBuild("2.0.0-w98.12"), 12);
  EXPECT_EQ(forkBuild("2.0.0-x4pro"), 0);
  EXPECT_EQ(forkBuild("2.0.0"), 0);
}

TEST(OtaVersion, SameVersionIsNotNewer) {
  EXPECT_FALSE(isNewer("2.0.0-w98.12", "2.0.0-w98.12"));
  EXPECT_FALSE(isNewer("", "2.0.0-w98.12"));
}

TEST(OtaVersion, ForkBuildOrdersForkReleases) {
  EXPECT_TRUE(isNewer("2.0.0-w98.13", "2.0.0-w98.12"));
  EXPECT_FALSE(isNewer("2.0.0-w98.11", "2.0.0-w98.12"));
  // A merged upstream base change still orders by build number.
  EXPECT_TRUE(isNewer("2.0.0-w98.20", "2.1.0-w98.19"));
  EXPECT_FALSE(isNewer("2.1.0-w98.19", "2.0.0-w98.20"));
}

TEST(OtaVersion, ForkReleaseReplacesLocalBuild) {
  EXPECT_TRUE(isNewer("2.0.0-w98.1", "2.0.0-x4pro"));
  EXPECT_TRUE(isNewer("2.0.0-w98.1", "2.0.0-dev-develop-abc1234"));
}

TEST(OtaVersion, UpstreamSemverStillWorks) {
  EXPECT_TRUE(isNewer("2.0.1", "2.0.0"));
  EXPECT_TRUE(isNewer("3.0.0", "2.9.9"));
  EXPECT_FALSE(isNewer("1.9.0", "2.0.0"));
  EXPECT_TRUE(isNewer("2.0.0", "2.0.0-rc+abc1234"));
  EXPECT_FALSE(isNewer("2.0.0", "2.0.0-x4pro"));
}
