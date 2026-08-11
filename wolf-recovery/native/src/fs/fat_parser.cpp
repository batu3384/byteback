#include "wolf_fs.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

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

static std::string formatFATName(const uint8_t name[11]) {
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (name[i] == ' ' || name[i] == 0) break;
        s += static_cast<char>(name[i]);
    }
    std::string ext;
    for (int i = 8; i < 11; ++i) {
        if (name[i] == ' ' || name[i] == 0) break;
        ext += static_cast<char>(name[i]);
    }
    if (!ext.empty()) s += "." + ext;
    return s;
}

bool FATParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint64_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);

    uint64_t partitionStart = 0;
    bool found = false;
    FAT32_VBR* vbr = nullptr;
    
    for (uint64_t i = 0; i < 10000; i++) {
        if (isRunning && !(*isRunning)) return false;
        if (!reader.readSectors(i * sectorSize, sectorSize, buffer.data()).success) continue;
        vbr = reinterpret_cast<FAT32_VBR*>(buffer.data());
        
        if ((vbr->bytesPerSector == 512 || vbr->bytesPerSector == 4096) && vbr->fatCount > 0 && vbr->reservedSectors > 0) {
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
    
    uint32_t bytesPerCluster = vbr->sectorsPerCluster * vbr->bytesPerSector;
    if (bytesPerCluster == 0) bytesPerCluster = sectorSize;

    auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
        if (cluster < 2) return 0x0FFFFFFF;
        uint32_t fatOffset = isFAT32 ? (cluster * 4) : (cluster + (cluster / 2));
        uint32_t fatSector = fatStart + (fatOffset / vbr->bytesPerSector);
        uint32_t entOffset = fatOffset % vbr->bytesPerSector;
        
        std::vector<uint8_t> secBuf(vbr->bytesPerSector);
        if (!reader.readSectors(fatSector * sectorSize, vbr->bytesPerSector, secBuf.data()).success) {
            return 0x0FFFFFFF;
        }
        
        if (isFAT32) {
            uint32_t next = *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
            return next & 0x0FFFFFFF;
        } else {
            // FAT12/FAT16 not fully supported in deep chain lookup for brevity, assuming FAT32 mainly
            if (entOffset == vbr->bytesPerSector - 1) return 0x0FFFFFFF; // Requires reading next sector
            uint16_t next = *reinterpret_cast<uint16_t*>(secBuf.data() + entOffset);
            return next;
        }
    };

    std::vector<uint32_t> dirClustersToProcess;
    if (isFAT32) {
        dirClustersToProcess.push_back(vbr->rootCluster);
    } else {
        // FAT16 root dir
    }
    
    std::vector<uint8_t> clusterBuf(bytesPerCluster);
    uint64_t nextFileId = 0;

    std::vector<uint32_t> processedClusters;

    while (!dirClustersToProcess.empty()) {
        if (isRunning && !(*isRunning)) break;
        uint32_t dirCluster = dirClustersToProcess.back();
        dirClustersToProcess.pop_back();

        if (std::find(processedClusters.begin(), processedClusters.end(), dirCluster) != processedClusters.end()) {
            continue;
        }
        processedClusters.push_back(dirCluster);

        uint32_t currentCluster = dirCluster;
        while (currentCluster >= 2 && currentCluster < 0x0FFFFFF8) {
            if (isRunning && !(*isRunning)) break;
            uint64_t clusterSector = dataStart + ((currentCluster - 2) * vbr->sectorsPerCluster);
            if (!reader.readSectors(clusterSector * sectorSize, bytesPerCluster, clusterBuf.data()).success) break;

            for (size_t offset = 0; offset < bytesPerCluster; offset += 32) {
                DirectoryEntry* entry = reinterpret_cast<DirectoryEntry*>(clusterBuf.data() + offset);
                if (entry->name[0] == 0x00) break;
                if (entry->name[0] == 0xE5) continue; // Deleted
                
                if (entry->attributes == 0x0F) continue; // LFN

                bool isDir = (entry->attributes & 0x10) != 0;
                bool isHidden = (entry->attributes & 0x02) != 0;
                bool isSystem = (entry->attributes & 0x04) != 0;
                
                std::string name = formatFATName(entry->name);
                if (name == "." || name == ".." || name.empty()) continue;

                uint32_t firstCluster = (entry->firstClusterHigh << 16) | entry->firstClusterLow;
                
                if (isDir) {
                    if (firstCluster >= 2) {
                        dirClustersToProcess.push_back(firstCluster);
                    }
                } else {
                    FileRecord fr;
                    fr.id = nextFileId++;
                    fr.name = name;
                    fr.path = "/"; // Path logic can be expanded
                    fr.sizeBytes = entry->fileSize;
                    
                    if (firstCluster >= 2) {
                        uint32_t runCluster = firstCluster;
                        while (runCluster >= 2 && runCluster < 0x0FFFFFF8) {
                            uint64_t runSector = dataStart + ((runCluster - 2) * vbr->sectorsPerCluster);
                            FileRecord::DataRun run;
                            run.startSector = runSector;
                            run.sectorCount = vbr->sectorsPerCluster;
                            fr.runs.push_back(run);
                            
                            uint32_t next = getNextCluster(runCluster);
                            if (next == runCluster) break; // Avoid infinite loop
                            runCluster = next;
                            if (fr.runs.size() > 10000) break; // Arbitrary limit for corrupted chains
                        }
                    }
                    
                    fr.status = 1;
                    fr.confidence = 90;
                    fr.category = "File";
                    callback(fr);
                }
            }
            
            uint32_t next = getNextCluster(currentCluster);
            if (next == currentCluster) break;
            currentCluster = next;
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = partitionStart; 
        callback(progressTick);
    }
    
    return true;
}

} // namespace wolf
