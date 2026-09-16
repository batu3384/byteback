#include "byteback_fs.h"
#include "fs/hfs_catalog.h"
#include <algorithm>
#include <cstring>
#include <vector>

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
    // CA-007 style: one unread sector must not blind the rest of a 4 MiB window.
    const uint32_t chunkSectors = std::max<uint32_t>(1, (4 * 1024 * 1024) / sectorSize);
    bool linearUnreadEmitted = false;
    auto emitLinearUnread = [&]() {
        if (linearUnreadEmitted) return;
        linearUnreadEmitted = true;
        FileRecord fr;
        fr.id = -1;
        fr.parentId = -1;
        fr.name = "Hfs_LinearUnread";
        fr.path = kHfsLinearUnreadPath;
        fr.source = kHfsLinearUnreadSource;
        fr.category = "System";
        fr.status = 0;
        fr.confidence = 20;
        callback(fr);
    };
    auto scanHfsMagic = [&](uint64_t baseByte, const uint8_t* data, uint64_t bytesRead) {
        for (uint32_t i = 0; i + 2 <= bytesRead; i += sectorSize) {
            const uint64_t abs = baseByte + i;
            if (abs < partitionOffsetBytes + 1024) continue;
            uint16_t magic = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
            if (magic != 0x482B && magic != 0x5848) continue;
            FileRecord fr;
            fr.id = -1;
            fr.name = "HFSPlus_VolumeHeader.bin";
            fr.path = "/recovered_hfs/";
            fr.sizeBytes = 512;
            fr.startSector = (abs - 1024) / sectorSize;
            fr.endSector = fr.startSector + 1;
            fr.status = 1;
            fr.confidence = 60;
            fr.category = "System";
            fr.source = "hfs_vh";
            callback(fr);
            break;
        }
    };
    struct SubRead { uint64_t sector; uint32_t sectors; };
    for (uint64_t sector = volumeStartSector; sector < volumeEndSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        const uint32_t thisChunk = static_cast<uint32_t>(
            std::min<uint64_t>(chunkSectors, volumeEndSector - sector));
        std::vector<uint8_t> buf(static_cast<size_t>(thisChunk) * sectorSize);
        std::vector<SubRead> subReads{{sector, thisChunk}};
        while (!subReads.empty()) {
            if (isRunning && !(*isRunning)) break;
            const SubRead sub = subReads.back();
            subReads.pop_back();
            const uint32_t wantBytes = sub.sectors * sectorSize;
            auto res = reader.readSectors(sub.sector * sectorSize, wantBytes, buf.data());
            if ((!res.success || res.bytesRead < wantBytes) && sub.sectors > 1) {
                const uint32_t half = sub.sectors / 2;
                subReads.push_back({sub.sector + half, sub.sectors - half});
                subReads.push_back({sub.sector, half});
                continue;
            }
            if (ioUnread(res, wantBytes)) {
                emitLinearUnread();
                continue;
            }
            scanHfsMagic(sub.sector * sectorSize, buf.data(), res.bytesRead);
        }
        progress(sector + thisChunk);
    }
    return true;
}

bool HFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0);
}

} // namespace byteback
