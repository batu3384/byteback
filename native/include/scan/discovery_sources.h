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
           source == "hfs_limit" || source == "hfs_vh" || source == "hfs_catalog" ||
           source == "usn_journal" || source == "ntfs_logfile" ||
           source == "ntfs_logfile_restart" || source == "ntfs_recycle_meta" ||
           source == "ntfs_i30" || source == "Folder" || source == "refs_volume" ||
           source == "carver_duplicate";
}

// SQL IN (...) fragment for MetadataStore list filters (same set minus carver_duplicate,
// which is gated by includeDuplicates instead).
inline const char* discoverySourcesSqlInList() {
    return "'apfs_container','apfs_volume','apfs_file','bitlocker_detect','bitlocker_fve',"
           "'vss_unbound','vss_bind','vss_snapshot','hfs_limit','hfs_vh','hfs_catalog',"
           "'usn_journal','ntfs_logfile','ntfs_logfile_restart','ntfs_recycle_meta',"
           "'ntfs_i30','Folder','refs_volume'";
}

} // namespace byteback
