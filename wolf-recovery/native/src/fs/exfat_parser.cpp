#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

ExFATParser::ExFATParser() {}
ExFATParser::~ExFATParser() {}

bool ExFATParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
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
            if (i + 512 > res.bytesRead) break;
            
            // ExFAT Boot Sector has "EXFAT   " at offset 3
            if (std::strncmp(reinterpret_cast<char*>(buffer.data() + i + 3), "EXFAT   ", 8) == 0) {
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = "ExFAT_VBR_" + std::to_string(foundCount) + ".bin";
                fr.extension = "bin";
                fr.path = "/recovered_exfat/" + fr.name;
                fr.sizeBytes = 512;
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + 1;
                fr.status = 1;
                fr.confidence = 90;
                fr.category = "System";
                fr.source = "exfat_vbr";
                fr.createdAt = 0;
                fr.modifiedAt = 0;
                
                callback(fr);
                break; // Skip the rest of the sector
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
