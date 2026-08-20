#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include "wolf_io.h"

namespace wolf {

// Boot-sector probe at a partition offset (not partition-table type strings).
enum class VolumeFsKind { Unknown, Ntfs, ExFat, Fat, Ext4, Apfs, Hfs, Refs };

VolumeFsKind probeVolumeAt(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t sectorSize);

struct PartitionInfo {
    std::string type;
    uint64_t startSector;
    uint64_t sizeInSectors;
    std::string label;
    bool isActive;
};

class PartitionScanner {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

    PartitionScanner(DiskReader* reader);

    std::vector<PartitionInfo> parseMBR();
    std::vector<PartitionInfo> parseGPT();
    std::vector<PartitionInfo> scanForPartitions(uint32_t stepSectors = 512, ProgressCallback progressCallback = nullptr);

private:
    DiskReader* reader_;
};

} // namespace wolf

