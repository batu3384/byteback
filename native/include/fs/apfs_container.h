#pragma once

#include "byteback_fs.h"
#include "byteback_io.h"
#include <atomic>
#include <cstdint>

namespace byteback {

// Walk an APFS container: NXSB (block size + nx_fs_oid[100]), APSB volumes,
// btree-leaf dir records (source=apfs_file, discovery-only) and file extents
// (source=apfs_extent, recoverable runs). Superblock/btree probe is the first
// 256 container blocks plus nx_fs_oid — not a full-container sequential walk.
bool walkApfsContainer(DiskReader& reader, uint64_t partitionOffsetBytes,
                       uint64_t partitionSizeBytes,
                       FileSystemParser::FileRecordCallback callback,
                       std::atomic<bool>* isRunning);

inline constexpr const char* kApfsNxsbUnreadPath = "/apfs-nxsb-unread/";
inline constexpr const char* kApfsNxsbUnreadSource = "apfs_nxsb_unread";
inline constexpr const char* kApfsBlockUnreadPath = "/apfs-block-unread/";
inline constexpr const char* kApfsBlockUnreadSource = "apfs_block_unread";
inline constexpr const char* kApfsLinearUnreadPath = "/apfs-linear-unread/";
inline constexpr const char* kApfsLinearUnreadSource = "apfs_linear_unread";
inline constexpr const char* kApfsCatalogUnreadPath = "/apfs-catalog-unread/";
inline constexpr const char* kApfsCatalogUnreadSource = "apfs_catalog_unread";
inline constexpr const char* kApfsOmapDeletedSource = "apfs_omap_deleted";

} // namespace byteback
