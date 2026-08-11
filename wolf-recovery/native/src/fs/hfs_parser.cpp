#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

HFSParser::HFSParser() {}
HFSParser::~HFSParser() {}

bool HFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
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
            if (i + 1536 > res.bytesRead) break;
            
            // HFS+ Volume Header is typically at offset 1024. Magic is 'H+' (0x48 0x2B) or 'HX' (0x48 0x58)
            uint16_t magic = *reinterpret_cast<uint16_t*>(buffer.data() + i + 1024);
            // In Big Endian, H+ is 0x482B. On Little Endian (x86), it's 0x2B48.
            if (magic == 0x2B48 || magic == 0x5848) {
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = "HFSPlus_VolumeHeader_" + std::to_string(foundCount) + ".bin";
                fr.extension = "bin";
                fr.path = "/recovered_hfs/" + fr.name;
                fr.sizeBytes = 512; // Volume header size
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + (512 / sectorSize);
                if (fr.endSector == fr.startSector) fr.endSector++;
                fr.status = 1;
                fr.confidence = 90;
                fr.category = "System";
                fr.source = "hfs_vh";
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
