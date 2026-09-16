#include "scan/discovery_sources.h"
#include <gtest/gtest.h>
#include <string>

using namespace byteback;

TEST(DiscoverySources, BlocksKnownDiscoveryAndDuplicate) {
    EXPECT_TRUE(isDiscoverySourceName("ntfs_i30"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_i30_unalloc"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_recycle_meta"));
    EXPECT_TRUE(isDiscoverySourceName("carver_duplicate"));
    EXPECT_TRUE(isDiscoverySourceName("refs_volume"));
    EXPECT_TRUE(isDiscoverySourceName("fat_dir_unread"));
    EXPECT_TRUE(isDiscoverySourceName("fat_chain_unread"));
    EXPECT_TRUE(isDiscoverySourceName("ext4_dir_unread"));
    EXPECT_TRUE(isDiscoverySourceName("xfs_dir_unread"));
    EXPECT_TRUE(isDiscoverySourceName("xfs_sb_unread"));
    EXPECT_TRUE(isDiscoverySourceName("xfs_inode_unread"));
    EXPECT_TRUE(isDiscoverySourceName("xfs_bmap_unread"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_i30_unread"));
    EXPECT_TRUE(isDiscoverySourceName("unalloc_map_unread"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_logfile_unread"));
    EXPECT_TRUE(isDiscoverySourceName("usn_unread"));
    EXPECT_TRUE(isDiscoverySourceName("ntfs_mft_unread"));
    EXPECT_TRUE(isDiscoverySourceName("probe_unread"));
    EXPECT_TRUE(isDiscoverySourceName("carver_unread"));
    EXPECT_TRUE(isDiscoverySourceName("hfs_catalog_unread"));
    EXPECT_TRUE(isDiscoverySourceName("hfs_linear_unread"));
    EXPECT_TRUE(isDiscoverySourceName("apfs_nxsb_unread"));
    EXPECT_TRUE(isDiscoverySourceName("apfs_block_unread"));
    EXPECT_TRUE(isDiscoverySourceName("apfs_linear_unread"));
    EXPECT_TRUE(isDiscoverySourceName("refs_supb_unread"));
    EXPECT_TRUE(isDiscoverySourceName("refs_page_unread"));
    EXPECT_FALSE(isDiscoverySourceName("hfs_catalog"));
    EXPECT_FALSE(isDiscoverySourceName("ntfs_mft"));
    EXPECT_FALSE(isDiscoverySourceName("carver"));
}

TEST(DiscoverySources, SqlListCoversCoreSources) {
    const std::string sql = discoverySourcesSqlInList();
    EXPECT_NE(sql.find("ntfs_i30"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_i30_unalloc"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_recycle_meta"), std::string::npos);
    EXPECT_NE(sql.find("refs_volume"), std::string::npos);
    EXPECT_NE(sql.find("fat_dir_unread"), std::string::npos);
    EXPECT_NE(sql.find("fat_chain_unread"), std::string::npos);
    EXPECT_NE(sql.find("ext4_dir_unread"), std::string::npos);
    EXPECT_NE(sql.find("xfs_dir_unread"), std::string::npos);
    EXPECT_NE(sql.find("xfs_sb_unread"), std::string::npos);
    EXPECT_NE(sql.find("xfs_inode_unread"), std::string::npos);
    EXPECT_NE(sql.find("xfs_bmap_unread"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_i30_unread"), std::string::npos);
    EXPECT_NE(sql.find("unalloc_map_unread"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_logfile_unread"), std::string::npos);
    EXPECT_NE(sql.find("usn_unread"), std::string::npos);
    EXPECT_NE(sql.find("ntfs_mft_unread"), std::string::npos);
    EXPECT_NE(sql.find("probe_unread"), std::string::npos);
    EXPECT_NE(sql.find("carver_unread"), std::string::npos);
    EXPECT_NE(sql.find("hfs_catalog_unread"), std::string::npos);
    EXPECT_NE(sql.find("hfs_linear_unread"), std::string::npos);
    EXPECT_NE(sql.find("apfs_nxsb_unread"), std::string::npos);
    EXPECT_NE(sql.find("apfs_block_unread"), std::string::npos);
    EXPECT_NE(sql.find("apfs_linear_unread"), std::string::npos);
    EXPECT_NE(sql.find("refs_supb_unread"), std::string::npos);
    EXPECT_NE(sql.find("refs_page_unread"), std::string::npos);
    EXPECT_EQ(sql.find("'hfs_catalog'"), std::string::npos);
    EXPECT_EQ(sql.find("carver_duplicate"), std::string::npos); // gated separately
}
