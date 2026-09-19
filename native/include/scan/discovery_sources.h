#pragma once

#include <string>

namespace byteback {

// Single source of truth for discovery-only / non-recoverable sources.
// Keep src/shared/source-label.ts DISCOVERY_ONLY in sync when editing.
inline bool isDiscoverySourceName(const std::string& source) {
    return source == "apfs_volume" || source == "apfs_container" ||
           source == "apfs_file" || source == "apfs_omap_deleted" || source == "bitlocker_detect" ||
           source == "bitlocker_fve" || source == "luks_detect" || source == "spaces_detect" ||
           source == "vss_unbound" ||
           source == "vss_bind" || source == "vss_snapshot" ||
           source == "hfs_limit" || source == "hfs_vh" || source == "hfs_vol_name" || source == "hfs_catalog_unread" ||
           source == "hfs_linear_unread" ||
           source == "apfs_nxsb_unread" || source == "apfs_block_unread" ||
           source == "apfs_linear_unread" || source == "apfs_catalog_unread" ||
           source == "usn_journal" || source == "usn_unread" ||
           source == "ntfs_logfile" ||
           source == "ntfs_logfile_restart" || source == "ntfs_logfile_unread" ||
           source == "ntfs_recycle_meta" ||
           source == "ntfs_i30" || source == "ntfs_i30_unalloc" || source == "ntfs_i30_unread" ||
           source == "ntfs_mft_unread" || source == "ntfs_efs" ||
           source == "ntfs_object_id" || source == "ntfs_vol_name" ||
           source == "Folder" || source == "refs_volume" || source == "refs_supb_unread" ||
           source == "refs_page_unread" ||
           source == "fat_dir_unread" || source == "fat_chain_unread" ||
           source == "fat_vol_label" || source == "iso9660_vol_id" ||
           source == "exfat_vol_label" || source == "udf_vol_id" || source == "udf_pvd_id" ||
           source == "udf_fsd_id" ||
           source == "ext4_dir_unread" ||
           source == "ext4_journal" || source == "ext4_journal_unread" ||
           source == "ext4_vol_name" ||
           source == "xfs_dir_unread" || source == "xfs_sb_unread" ||
           source == "xfs_inode_unread" || source == "xfs_bmap_unread" ||
           source == "xfs_vol_name" ||
           source == "unalloc_map_unread" ||
           source == "probe_unread" ||
           source == "carver_unread" ||
           source == "carver_duplicate";
}

// SQL IN (...) fragment for MetadataStore list filters (same set minus carver_duplicate,
// which is gated by includeDuplicates instead).
inline const char* discoverySourcesSqlInList() {
    return "'apfs_container','apfs_volume','apfs_file','apfs_omap_deleted','bitlocker_detect','bitlocker_fve','luks_detect','spaces_detect',"
           "'vss_unbound','vss_bind','vss_snapshot','hfs_limit','hfs_vh','hfs_vol_name','hfs_catalog_unread',"
           "'hfs_linear_unread','apfs_nxsb_unread','apfs_block_unread','apfs_linear_unread','apfs_catalog_unread',"
           "'usn_journal','usn_unread','ntfs_logfile','ntfs_logfile_restart','ntfs_logfile_unread',"
           "'ntfs_recycle_meta',"
           "'ntfs_i30','ntfs_i30_unalloc','ntfs_i30_unread','ntfs_mft_unread','ntfs_efs','ntfs_object_id','ntfs_vol_name','Folder','refs_volume','refs_supb_unread','refs_page_unread',"
           "'fat_dir_unread','fat_chain_unread','fat_vol_label','iso9660_vol_id','exfat_vol_label','udf_vol_id','udf_pvd_id','udf_fsd_id','ext4_dir_unread','ext4_journal','ext4_journal_unread','ext4_vol_name','xfs_dir_unread','xfs_sb_unread','xfs_inode_unread','xfs_bmap_unread','xfs_vol_name',"
           "'unalloc_map_unread','probe_unread','carver_unread'";
}

} // namespace byteback
