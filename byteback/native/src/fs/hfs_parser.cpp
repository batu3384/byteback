#include "byteback_fs.h"
#include "fs/hfs_catalog.h"
#include <cstring>

namespace byteback {

HFSParser::HFSParser() {}
HFSParser::~HFSParser() {}

bool HFSParser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                       uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t volumeStartSector = partitionOffsetBytes / sectorSize;
    uint64_t volumeEndSector = reader.getDiskSize() / sectorSize;
    if (partitionSizeBytes > 0) {
        volumeEndSector = std::min(volumeEndSector, volumeStartSector + partitionSizeBytes / sectorSize);
    }

    bool catalogOk = scanHfsPlusCatalog(reader, partitionOffsetBytes, partitionSizeBytes, callback, isRunning);

  auto progress = [&](uint64_t sector) {
        FileRecord tick;
        tick.id = -1;
        tick.startSector = sector;
        callback(tick);
    };

    if (catalogOk) {
        progress(volumeEndSector);
        return true;
    }

    // Fallback: superblock signature discovery within partition bounds.
    const uint32_t chunkSectors = std::max<uint32_t>(1, (4 * 1024 * 1024) / sectorSize);
    for (uint64_t sector = volumeStartSector; sector < volumeEndSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        std::vector<uint8_t> buf(chunkSectors * sectorSize);
        auto res = reader.readSectors(sector * sectorSize, static_cast<uint32_t>(buf.size()), buf.data());
        if (!res.success) continue;
        for (uint32_t i = 0; i + 1536 <= res.bytesRead; i += sectorSize) {
            uint16_t magic = static_cast<uint16_t>(buf[i + 1024] | (buf[i + 1025] << 8));
            if (magic == 0x482B || magic == 0x5848) {
                FileRecord fr;
                fr.id = -1;
                fr.name = "HFSPlus_VolumeHeader.bin";
                fr.path = "/recovered_hfs/";
                fr.sizeBytes = 512;
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + 1;
                fr.status = 1;
                fr.confidence = 60;
                fr.category = "System";
                fr.source = "hfs_vh";
                callback(fr);
                break;
            }
        }
        progress(sector + chunkSectors);
    }
    return true;
}

bool HFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0);
}

} // namespace byteback
