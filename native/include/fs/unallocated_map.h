#pragma once

#include "fs/partition_scanner.h"
#include "byteback_io.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace byteback {

struct SectorRange {
    uint64_t start = 0;
    uint64_t count = 0;
};

void mergeSectorRanges(std::vector<SectorRange>& ranges);

// Free/unallocated sector ranges for signature carving. Empty => caller should
// fall back to scanning the full volume bounds.
std::vector<SectorRange> buildUnallocatedRanges(DiskReader& reader, VolumeFsKind kind,
                                                uint64_t volumeOffsetBytes,
                                                uint64_t volumeSizeBytes);

uint64_t totalSectorCount(const std::vector<SectorRange>& ranges);

// Collect unallocated ranges for all partitions, or one partition when
// partitionStartSector >= 0 and partitionSizeSectors > 0.
std::vector<SectorRange> collectUnallocatedForScan(DiskReader& reader,
                                                   int64_t partitionStartSector = -1,
                                                   uint64_t partitionSizeSectors = 0);

// Set by collectUnallocatedForScan when CA-022 filled whole-partition ranges
// because the FS bitmap was empty (APFS/HFS/ReFS/unknown). Reset on the next
// collect. Deep scan reads this to label the phase carve_fallback.
extern std::atomic<bool> g_unallocatedUsedFallback;

// Set when a bitmap/FAT window read failed or zero-padded. A partial map must
// not look like a complete free-space walk — scan emits unalloc_map_unread.
extern std::atomic<bool> g_unallocatedMapUnread;

inline constexpr const char* kUnallocMapUnreadPath = "/unalloc-map-unread/";
inline constexpr const char* kUnallocMapUnreadSource = "unalloc_map_unread";

} // namespace byteback
