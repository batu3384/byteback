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

bool FATParser::scan(DiskReader& reader, FileRecordCallback callback) {
    uint64_t sectorSize = reader.getSectorSize();
    std::vector<uint8_t> buffer(sectorSize);

    if (!reader.readSectors(0, 1, buffer.data()).success) return false;

    FAT32_VBR* vbr = reinterpret_cast<FAT32_VBR*>(buffer.data());
    if (vbr->bytesPerSector != 512 && vbr->bytesPerSector != 4096) return false;
    if (vbr->fatCount == 0) return false;

    uint64_t fatStart = vbr->reservedSectors;
    uint64_t dataStart = fatStart + (vbr->fatCount * vbr->fatSize32);
    
    uint32_t rootCluster = vbr->rootCluster;
    if (rootCluster < 2) return false;

    uint64_t rootSector = dataStart + ((rootCluster - 2) * vbr->sectorsPerCluster);
    std::vector<uint8_t> clusterBuf(vbr->sectorsPerCluster * sectorSize);

    if (reader.readSectors(rootSector, vbr->sectorsPerCluster, clusterBuf.data()).success) {
        for (size_t offset = 0; offset < clusterBuf.size(); offset += 32) {
            DirectoryEntry* entry = reinterpret_cast<DirectoryEntry*>(clusterBuf.data() + offset);
            if (entry->name[0] == 0x00) break;
            if (entry->name[0] == 0xE5) {
                FileRecord fr;
                fr.id = offset;
                fr.name = "deleted_file_" + std::to_string(offset) + ".bin";
                fr.path = "/";
                fr.sizeBytes = entry->fileSize;
                
                uint32_t firstCluster = (entry->firstClusterHigh << 16) | entry->firstClusterLow;
                uint64_t startSector = dataStart + ((firstCluster - 2) * vbr->sectorsPerCluster);
                
                FileRecord::DataRun run;
                run.startSector = startSector;
                run.sectorCount = (entry->fileSize + sectorSize - 1) / sectorSize;
                if(run.sectorCount == 0) run.sectorCount = 1;
                fr.runs.push_back(run);
                
                fr.status = 0;
                fr.confidence = 80;
                callback(fr);
            }
        }
    }
    return true;
}

} // namespace wolf
