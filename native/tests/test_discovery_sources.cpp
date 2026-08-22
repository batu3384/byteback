#include "scan/discovery_sources.h"
#include <gtest/gtest.h>
#include <string>

using namespace byteback;

TEST(DiscoverySources, BlocksKnownDiscoveryAndDuplicate) {
    EXPECT_TRUE(isDiscoverySourceName("ntfs_i30"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_recycle_meta"));
    EXPECT_TRUE(isDiscoverySourceName("carver_duplicate"));
    EXPECT_FALSE(isDiscoverySourceName("ntfs_mft"));
    EXPECT_FALSE(isDiscoverySourceName("carver"));
}

TEST(DiscoverySources, SqlListCoversCoreSources) {
    const std::string sql = discoverySourcesSqlInList();
    EXPECT_NE(sql.find("ntfs_i30"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_recycle_meta"), std::string::npos);
    EXPECT_EQ(sql.find("carver_duplicate"), std::string::npos); // gated separately
}
