#pragma once

#include <string>

namespace byteback {

// Single source of truth for discovery-only / non-recoverable sources.
// Keep src/shared/source-label.ts DISCOVERY_ONLY in sync when editing.
inline bool isDiscoverySourceName(const std::string& source) {
    return source == "apfs_volume" || source == "apfs_container" ||
           source == "apfs_file" || source == "bitlocker_detect" ||
           source == "bitlocker_fve" || source == "vss_unbound" ||
           source == "vss_bind" || source == "vss_snapshot" ||
           source == "hfs_limit" || source == "hfs_vh" || source == "hfs_catalog_unread" ||
           source == "hfs_linear_unread" ||
           source == "apfs_nxsb_unread" || source == "apfs_block_unread" ||
           source == "apfs_linear_unread" ||
           source == "usn_journal" || source == "usn_unread" ||
           source == "ntfs_logfile" ||
           source == "ntfs_logfile_restart" || source == "ntfs_logfile_unread" ||
           source == "ntfs_recycle_meta" ||
           source == "ntfs_i30" || source == "ntfs_i30_unalloc" || source == "ntfs_i30_unread" ||
           source == "ntfs_mft_unread" ||
           source == "Folder" || source == "refs_volume" || source == "refs_supb_unread" ||
           source == "refs_page_unread" ||
           source == "fat_dir_unread" || source == "fat_chain_unread" ||
           source == "ext4_dir_unread" ||
           source == "xfs_dir_unread" || source == "xfs_sb_unread" ||
           source == "xfs_inode_unread" || source == "xfs_bmap_unread" ||
           source == "unalloc_map_unread" ||
           source == "probe_unread" ||
           source == "carver_unread" ||
           source == "carver_duplicate";
}

// SQL IN (...) fragment for MetadataStore list filters (same set minus carver_duplicate,
// which is gated by includeDuplicates instead).
inline const char* discoverySourcesSqlInList() {
    return "'apfs_container','apfs_volume','apfs_file','bitlocker_detect','bitlocker_fve',"
           "'vss_unbound','vss_bind','vss_snapshot','hfs_limit','hfs_vh','hfs_catalog_unread',"
           "'hfs_linear_unread','apfs_nxsb_unread','apfs_block_unread','apfs_linear_unread',"
           "'usn_journal','usn_unread','ntfs_logfile','ntfs_logfile_restart','ntfs_logfile_unread',"
           "'ntfs_recycle_meta',"
           "'ntfs_i30','ntfs_i30_unalloc','ntfs_i30_unread','ntfs_mft_unread','Folder','refs_volume','refs_supb_unread','refs_page_unread',"
           "'fat_dir_unread','fat_chain_unread','ext4_dir_unread','xfs_dir_unread','xfs_sb_unread','xfs_inode_unread','xfs_bmap_unread',"
           "'unalloc_map_unread','probe_unread','carver_unread'";
}

} // namespace byteback
