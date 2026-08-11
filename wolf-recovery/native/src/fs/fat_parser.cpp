#include "wolf_fs.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

FATParser::FATParser() {}
FATParser::~FATParser() {}

#pragma pack(push, 1)
struct FAT32_VBR {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t fatCount;
    uint16_t rootEntries;
    uint16_t totalSectors16;
    uint8_t mediaDescriptor;
    uint16_t fatSize16;
    uint16_t sectorsPerTrack;
    uint16_t heads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    uint32_t fatSize32;
    uint16_t extFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;
};

struct DirectoryEntry {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creationTimeTenths;
    uint16_t creationTime;
    uint16_t creationDate;
    uint16_t lastAccessDate;
    uint16_t firstClusterHigh;
    uint16_t writeTime;
    uint16_t writeDate;
    uint16_t firstClusterLow;
    uint32_t fileSize;
};
#pragma pack(pop)

bool FATParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint64_t sectorSize = reader.getSectorSize();
    std::vector<uint8_t> buffer(sectorSize);

    // Search for FAT VBR
    uint64_t partitionStart = 0;
    bool found = false;
    FAT32_VBR* vbr = nullptr;
    
    for (uint64_t i = 0; i < 100000; i++) {
        if (isRunning && !(*isRunning)) return false;
        if (!reader.readSectors(i * sectorSize, sectorSize, buffer.data()).success) continue;
        vbr = reinterpret_cast<FAT32_VBR*>(buffer.data());
        
        // Basic FAT VBR checks
        if ((vbr->bytesPerSector == 512 || vbr->bytesPerSector == 4096) && vbr->fatCount > 0 && vbr->reservedSectors > 0) {
            // Check boot signature 0x55AA at offset 510
            if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
                partitionStart = i;
                found = true;
                break;
            }
        }
    }
    
    if (!found) return false;

    bool isFAT32 = (vbr->fatSize16 == 0);
    uint32_t fatSize = isFAT32 ? vbr->fatSize32 : vbr->fatSize16;
    uint32_t rootDirSectors = ((vbr->rootEntries * 32) + (vbr->bytesPerSector - 1)) / vbr->bytesPerSector;
    
    uint64_t fatStart = partitionStart + vbr->reservedSectors;
    uint64_t rootDirStart = fatStart + (vbr->fatCount * fatSize);
    uint64_t dataStart = rootDirStart + rootDirSectors;
    
    uint64_t rootSector = isFAT32 ? (dataStart + ((vbr->rootCluster - 2) * vbr->sectorsPerCluster)) : rootDirStart;
    uint32_t readSectors = isFAT32 ? vbr->sectorsPerCluster : rootDirSectors;
    if (readSectors == 0) readSectors = 1;

    std::vector<uint8_t> clusterBuf(readSectors * sectorSize);

    if (reader.readSectors(rootSector * sectorSize, readSectors * sectorSize, clusterBuf.data()).success) {
        for (size_t offset = 0; offset < clusterBuf.size(); offset += 32) {
            if (isRunning && !(*isRunning)) break;
            DirectoryEntry* entry = reinterpret_cast<DirectoryEntry*>(clusterBuf.data() + offset);
            if (entry->name[0] == 0x00) break;
            
            // 0xE5 is deleted file marker
            if (entry->name[0] == 0xE5) {
                FileRecord fr;
                fr.id = offset;
                fr.name = "deleted_file_" + std::to_string(offset) + ".bin";
                fr.path = "/";
                fr.sizeBytes = entry->fileSize;
                
                uint32_t firstCluster = (entry->firstClusterHigh << 16) | entry->firstClusterLow;
                if (firstCluster >= 2) {
                    uint64_t startSector = dataStart + ((firstCluster - 2) * vbr->sectorsPerCluster);
                    FileRecord::DataRun run;
                    run.startSector = startSector;
                    run.sectorCount = (entry->fileSize + sectorSize - 1) / sectorSize;
                    if(run.sectorCount == 0) run.sectorCount = 1;
                    fr.runs.push_back(run);
                }
                
                fr.status = 0;
                fr.confidence = 80;
                callback(fr);
            }
        }
    }
    return true;
}

} // namespace wolf
