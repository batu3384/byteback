#pragma once

#include "fs/partition_scanner.h"
#include "wolf_io.h"
#include <cstdint>
#include <vector>

namespace wolf {

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

// Collect unallocated ranges for all partitions, or one partition when
// partitionStartSector >= 0 and partitionSizeSectors > 0.
std::vector<SectorRange> collectUnallocatedForScan(DiskReader& reader,
                                                   int64_t partitionStartSector = -1,
                                                   uint64_t partitionSizeSectors = 0);

} // namespace wolf
