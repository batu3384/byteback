#include "byteback_fs.h"
#include "fs/apfs_container.h"
#include "byteback_memory.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace byteback {

APFSParser::APFSParser() {}
APFSParser::~APFSParser() {}

bool APFSParser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                        uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t volumeStartSector = partitionOffsetBytes / sectorSize;
    uint64_t volumeEndSector = reader.getDiskSize() / sectorSize;
    if (partitionSizeBytes > 0) {
        volumeEndSector = std::min(volumeEndSector, volumeStartSector + partitionSizeBytes / sectorSize);
    }

    if (walkApfsContainer(reader, partitionOffsetBytes, partitionSizeBytes, callback, isRunning)) {
        return true;
    }

    const uint32_t chunkSectors = std::max<uint32_t>(1, (4 * 1024 * 1024) / sectorSize);
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto& buffer = *poolBuf;
    int foundCount = 0;
    bool linearUnreadEmitted = false;
    auto emitLinearUnread = [&]() {
        if (linearUnreadEmitted) return;
        linearUnreadEmitted = true;
        FileRecord fr;
        fr.id = -1;
        fr.parentId = -1;
        fr.name = "Apfs_LinearUnread";
        fr.path = kApfsLinearUnreadPath;
        fr.source = kApfsLinearUnreadSource;
        fr.category = "System";
        fr.status = 0;
        fr.confidence = 20;
        callback(fr);
    };
    auto scanNxsb = [&](uint64_t baseByte, const uint8_t* data, uint64_t bytesRead) {
        for (uint32_t i = 0; i + 36 <= bytesRead; i += sectorSize) {
            if (std::strncmp(reinterpret_cast<const char*>(data + i + 32), "NXSB", 4) != 0) continue;
            FileRecord fr;
            fr.id = foundCount++;
            fr.parentId = 0;
            fr.name = "APFS_Container_" + std::to_string(foundCount) + ".bin";
            fr.extension = "bin";
            fr.path = "/recovered_apfs/" + fr.name;
            fr.sizeBytes = 4096;
            fr.startSector = (baseByte + i) / sectorSize;
            fr.endSector = fr.startSector + (4096 / sectorSize);
            if (fr.endSector == fr.startSector) fr.endSector++;
            fr.status = 1;
            fr.confidence = 90;
            fr.category = "System";
            fr.source = "apfs_nxsb";
            callback(fr);
        }
    };
    struct SubRead { uint64_t sector; uint32_t sectors; };
    for (uint64_t sector = volumeStartSector; sector < volumeEndSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        const uint32_t thisChunk = static_cast<uint32_t>(
            std::min<uint64_t>(chunkSectors, volumeEndSector - sector));
        std::vector<SubRead> subReads{{sector, thisChunk}};
        while (!subReads.empty()) {
            if (isRunning && !(*isRunning)) break;
            const SubRead sub = subReads.back();
            subReads.pop_back();
            const uint32_t wantBytes = sub.sectors * sectorSize;
            auto res = reader.readSectors(sub.sector * sectorSize, wantBytes, buffer.data());
            if ((!res.success || res.bytesRead < wantBytes) && sub.sectors > 1) {
                const uint32_t half = sub.sectors / 2;
                subReads.push_back({sub.sector + half, sub.sectors - half});
                subReads.push_back({sub.sector, half});
                continue;
            }
            if (!res.success) {
                emitLinearUnread();
                continue;
            }
            scanNxsb(sub.sector * sectorSize, buffer.data(), res.bytesRead);
        }

        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + thisChunk;
        callback(progressTick);
    }
    return true;
}

bool APFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0);
}

} // namespace byteback
