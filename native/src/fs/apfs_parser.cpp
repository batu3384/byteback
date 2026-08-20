#include "byteback_fs.h"
#include "fs/apfs_container.h"
#include "byteback_memory.h"
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

    for (uint64_t sector = volumeStartSector; sector < volumeEndSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;

        auto res = reader.readSectors(sector * sectorSize, chunkSize, buffer.data());
        if (!res.success) continue;

        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            if (i + 4096 > res.bytesRead) break;
            if (std::strncmp(reinterpret_cast<char*>(buffer.data() + i + 32), "NXSB", 4) == 0) {
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = "APFS_Container_" + std::to_string(foundCount) + ".bin";
                fr.extension = "bin";
                fr.path = "/recovered_apfs/" + fr.name;
                fr.sizeBytes = 4096;
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + (4096 / sectorSize);
                if (fr.endSector == fr.startSector) fr.endSector++;
                fr.status = 1;
                fr.confidence = 90;
                fr.category = "System";
                fr.source = "apfs_nxsb";
                callback(fr);
            }
        }

        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }
    return true;
}

bool APFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0);
}

} // namespace byteback
