#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include "byteback_io.h"

namespace byteback {

// Boot-sector probe at a partition offset (not partition-table type strings).
// Unread: I/O fail or zero-pad. Not the same as Unknown (complete read, no magic).
enum class VolumeFsKind { Unknown, Ntfs, ExFat, Fat, Ext4, Apfs, Hfs, Refs, Xfs, Unread };

VolumeFsKind probeVolumeAt(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t sectorSize);

// Partition table or volume-boot probe I/O failed. Empty results are not "no FS".
inline constexpr const char* kProbeUnreadPath = "/probe-unread/";
inline constexpr const char* kProbeUnreadSource = "probe_unread";

struct PartitionInfo {
    std::string type;
    uint64_t startSector;
    uint64_t sizeInSectors;
    std::string label;
    bool isActive;
};

/** GPT wins when present — same rule as ListPartitions / StartScan. */
inline const PartitionInfo* selectedPartition(
    const std::vector<PartitionInfo>& mbr,
    const std::vector<PartitionInfo>& gpt,
    int index) {
    if (index < 0) return nullptr;
    const std::vector<PartitionInfo>& parts = gpt.empty() ? mbr : gpt;
    if (static_cast<size_t>(index) >= parts.size()) return nullptr;
    return &parts[static_cast<size_t>(index)];
}

class PartitionScanner {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

    PartitionScanner(DiskReader* reader);

    std::vector<PartitionInfo> parseMBR();
    std::vector<PartitionInfo> parseGPT();
    std::vector<PartitionInfo> scanForPartitions(uint32_t stepSectors = 512, ProgressCallback progressCallback = nullptr);

    /** MBR/GPT sector I/O failed or padded. Empty list is not "no table". */
    bool tableUnread() const { return tableUnread_; }
    /** Lost-partition step I/O failed or padded. Empty list is not "no volumes". */
    bool scanUnread() const { return scanUnread_; }

private:
    DiskReader* reader_;
    bool tableUnread_ = false;
    bool scanUnread_ = false;
};

} // namespace byteback

