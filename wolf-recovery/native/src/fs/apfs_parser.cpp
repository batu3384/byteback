#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

APFSParser::APFSParser() {}
APFSParser::~APFSParser() {}

bool APFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen()) return false;
    
    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    const uint32_t chunkSectors = (4 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto& buffer = *poolBuf;
    
    uint64_t maxSector = diskSize / sectorSize;
    int foundCount = 0;

    for (uint64_t sector = 0; sector < maxSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        
        auto res = reader.readSectors(sector * sectorSize, chunkSize, buffer.data());
        if (!res.success) continue;

        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            if (i + 4096 > res.bytesRead) break;
            
            // APFS Container Superblock (NXSB) magic is "NXSB" at offset 32
            if (std::strncmp(reinterpret_cast<char*>(buffer.data() + i + 32), "NXSB", 4) == 0) {
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = "APFS_Container_" + std::to_string(foundCount) + ".bin";
                fr.extension = "bin";
                fr.path = "/recovered_apfs/" + fr.name;
                fr.sizeBytes = 4096; // size of superblock
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + (4096 / sectorSize);
                fr.status = 1;
                fr.confidence = 90;
                fr.category = "System";
                fr.source = "apfs_nxsb";
                fr.createdAt = 0;
                fr.modifiedAt = 0;
                
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

} // namespace wolf
