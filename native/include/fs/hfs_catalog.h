#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include "byteback_fs.h"
#include <atomic>
#include <cstdint>
#include <functional>

namespace byteback {

// Offset table lives at the tail of the node: (numRecords+1) * uint16.
inline bool hfsOffsetTableFits(uint32_t blockSize, uint16_t numRecords) {
    const uint64_t need = (static_cast<uint64_t>(numRecords) + 1u) * 2u;
    return blockSize >= 14 && need <= blockSize;
}

// Walk an HFS+ catalog B-tree and emit FileRecord entries for files/folders.
// maxFiles==0 walks until the tree ends or isRunning is false. maxFiles>0 is a
// test/emergency brake that emits source=hfs_limit.
inline constexpr const char* kHfsCatalogUnreadPath = "/hfs-catalog-unread/";
inline constexpr const char* kHfsCatalogUnreadSource = "hfs_catalog_unread";
inline constexpr const char* kHfsCatalogUnusedSource = "hfs_catalog_unused";
inline constexpr const char* kHfsJournalSource = "hfs_journal";
inline constexpr const char* kHfsVolNameSource = "hfs_vol_name";
inline constexpr const char* kHfsLinearUnreadPath = "/hfs-linear-unread/";
inline constexpr const char* kHfsLinearUnreadSource = "hfs_linear_unread";

bool scanHfsPlusCatalog(DiskReader& reader, uint64_t partitionOffsetBytes,
                        uint64_t partitionSizeBytes,
                        FileSystemParser::FileRecordCallback callback,
                        std::atomic<bool>* isRunning,
                        int maxFiles = 0);

} // namespace byteback
