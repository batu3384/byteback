#include "fs/fat_parser.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

#pragma pack(push, 1)

// FAT12/16/32 Boot Sector (BPB)
struct FAT_BPB {
    uint8_t  jmp[3];
    char     oemName[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t  numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t  media;
    uint16_t fatSize16;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    // FAT32 specific
    uint32_t fatSize32;
    uint16_t extFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;
    uint16_t fsInfo;
    uint16_t bkBootSec;
    uint8_t  reserved[12];
    uint8_t  drvNum;
    uint8_t  reserved1;
    uint8_t  bootSig;
    uint32_t volId;
    char     volLab[11];
    char     filSysType[8];
};

struct ExFAT_BPB {
    uint8_t  jmp[3];
    char     oemName[8];
    uint8_t  reserved[53];
    uint64_t partitionOffset;
    uint64_t volumeLength;
    uint32_t fatOffset;
    uint32_t fatLength;
    uint32_t clusterHeapOffset;
    uint32_t clusterCount;
    uint32_t rootDirectoryCluster;
    uint32_t volumeSerialNumber;
    uint16_t fileSystemRevision;
    uint16_t volumeFlags;
    uint8_t  bytesPerSectorShift;
    uint8_t  sectorsPerClusterShift;
    uint8_t  numFats;
    uint8_t  driveSelect;
    uint8_t  percentInUse;
    uint8_t  reserved2[7];
};

struct FAT_DirEntry {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntRes;
    uint8_t  crtTimeTenth;
    uint16_t crtTime;
    uint16_t crtDate;
    uint16_t lstAccDate;
    uint16_t fstClusHI;
    uint16_t wrtTime;
    uint16_t wrtDate;
    uint16_t fstClusLO;
    uint32_t fileSize;
};

struct FAT_LFNEntry {
    uint8_t  ord;
    uint16_t name1[5];
    uint8_t  attr; // 0x0F
    uint8_t  type; // 0x00
    uint8_t  chksum;
    uint16_t name2[6];
    uint16_t fstClusLO;
    uint16_t name3[2];
};

// exFAT Directory Entries
struct ExFAT_GenericEntry {
    uint8_t entryType;
    uint8_t data[31];
};

#pragma pack(pop)

FATParser::FATParser() {}
FATParser::~FATParser() {}

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

void FATParser::parse(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback) {
    if (!reader.isOpen()) return;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);

    auto res = reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    if (!res.success) return;

    if (memcmp(&buffer[3], "EXFAT   ", 8) == 0) {
        parseExFAT(reader, partitionOffset, callback);
    } else {
        parseFAT(reader, partitionOffset, callback);
    }
}

void FATParser::parseFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    
    FAT_BPB* bpb = reinterpret_cast<FAT_BPB*>(buffer.data());
    
    uint16_t bps = bpb->bytesPerSector;
    if (bps == 0 || (bps & (bps - 1)) != 0) return; // Invalid BPS
    
    uint32_t rootDirSectors = ((bpb->rootEntryCount * 32) + (bps - 1)) / bps;
    uint32_t fatSize = (bpb->fatSize16 != 0) ? bpb->fatSize16 : bpb->fatSize32;
    
    uint64_t fatStartSector = partitionOffset + bpb->reservedSectorCount;
    uint64_t rootDirStartSector = fatStartSector + (bpb->numFATs * fatSize);
    uint64_t dataStartSector = rootDirStartSector + rootDirSectors;
    
    uint32_t totalSectors = (bpb->totalSectors16 != 0) ? bpb->totalSectors16 : bpb->totalSectors32;
    uint32_t dataSectors = totalSectors - (bpb->reservedSectorCount + (bpb->numFATs * fatSize) + rootDirSectors);
    uint32_t countOfClusters = dataSectors / bpb->sectorsPerCluster;
    
    bool isFat32 = countOfClusters >= 65525;
    
    std::vector<uint32_t> dirClusters;
    if (isFat32) {
        dirClusters.push_back(bpb->rootCluster);
    } else {
        // Parse FAT16 root dir sequentially
        std::vector<uint8_t> rootBuf(rootDirSectors * bps);
        reader.readSectors(rootDirStartSector * bps, rootDirSectors * bps, rootBuf.data());
        
        for (uint32_t offset = 0; offset < rootBuf.size(); offset += 32) {
            FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(rootBuf.data() + offset);
            if (entry->name[0] == 0x00) break;
            if (entry->name[0] == 0xE5) {
                // Deleted file
                continue;
            }
            if (entry->attr == 0x0F) continue; // LFN
            
            std::string name = formatFATName(entry->name);
            uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
            if (entry->attr & 0x10) {
                if (name != "." && name != "..") dirClusters.push_back(firstCluster);
            } else {
                callback(name, entry->fileSize, firstCluster, "/", 1);
            }
        }
    }
    
    // Simplistic directory traversal for FAT32
    // Normally we'd do a recursive traversal using the FAT table
    // For brevity in this stub, we report it.
    if (isFat32) {
        // FAT table lookup lambda
        auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
            if (cluster < 2) return 0x0FFFFFFF;
            uint32_t fatOffset = cluster * 4;
            uint32_t fatSector = fatStartSector + (fatOffset / bps);
            uint32_t entOffset = fatOffset % bps;
            
            std::vector<uint8_t> secBuf(bps);
            if (!reader.readSectors(fatSector * bps, bps, secBuf.data()).success) return 0x0FFFFFFF;
            
            uint32_t next = *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
            return next & 0x0FFFFFFF;
        };
        
        uint32_t bytesPerCluster = bpb->sectorsPerCluster * bps;
        std::vector<uint8_t> clusterBuf(bytesPerCluster);
        
        while (!dirClusters.empty()) {
            uint32_t currentCluster = dirClusters.back();
            dirClusters.pop_back();
            
            uint32_t clus = currentCluster;
            while (clus >= 2 && clus < 0x0FFFFFF8) {
                uint64_t sec = dataStartSector + (clus - 2) * bpb->sectorsPerCluster;
                if (!reader.readSectors(sec * bps, bytesPerCluster, clusterBuf.data()).success) break;
                
                for (uint32_t offset = 0; offset < bytesPerCluster; offset += 32) {
                    FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(clusterBuf.data() + offset);
                    if (entry->name[0] == 0x00) break;
                    
                    int status = 1;
                    if (entry->name[0] == 0xE5) status = 0; // Deleted
                    
                    if (entry->attr == 0x0F) continue; // LFN stub
                    
                    std::string name = formatFATName(entry->name);
                    if (name == "." || name == "..") continue;
                    
                    uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
                    if (entry->attr & 0x10) {
                        dirClusters.push_back(firstCluster);
                    } else {
                        callback(name, entry->fileSize, firstCluster, "/", status);
                    }
                }
                
                clus = getNextCluster(clus);
            }
        }
    }
}

void FATParser::parseExFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    
    ExFAT_BPB* bpb = reinterpret_cast<ExFAT_BPB*>(buffer.data());
    
    uint32_t bytesPerSector = 1 << bpb->bytesPerSectorShift;
    uint32_t bytesPerCluster = bytesPerSector << bpb->sectorsPerClusterShift;
    
    uint64_t fatStartSector = partitionOffset + bpb->fatOffset;
    uint64_t dataStartSector = partitionOffset + bpb->clusterHeapOffset;
    
    auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        uint32_t fatOffset = cluster * 4;
        uint32_t fatSector = fatStartSector + (fatOffset / bytesPerSector);
        uint32_t entOffset = fatOffset % bytesPerSector;
        
        std::vector<uint8_t> secBuf(bytesPerSector);
        if (!reader.readSectors(fatSector * bytesPerSector, bytesPerSector, secBuf.data()).success) return 0xFFFFFFFF;
        
        return *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
    };
    
    std::vector<uint32_t> dirClusters = { bpb->rootDirectoryCluster };
    std::vector<uint8_t> clusterBuf(bytesPerCluster);
    
    while (!dirClusters.empty()) {
        uint32_t currentCluster = dirClusters.back();
        dirClusters.pop_back();
        
        uint32_t clus = currentCluster;
        while (clus >= 2 && clus <= 0xFFFFFFF6) {
            uint64_t sec = dataStartSector + (clus - 2) * (1 << bpb->sectorsPerClusterShift);
            if (!reader.readSectors(sec * bytesPerSector, bytesPerCluster, clusterBuf.data()).success) break;
            
            std::string currentFileName = "";
            uint64_t currentFileSize = 0;
            uint32_t currentFirstCluster = 0;
            bool isDir = false;
            int status = 1;
            
            for (uint32_t offset = 0; offset < bytesPerCluster; offset += 32) {
                ExFAT_GenericEntry* entry = reinterpret_cast<ExFAT_GenericEntry*>(clusterBuf.data() + offset);
                
                if (entry->entryType == 0x00) break;
                
                if ((entry->entryType & 0x7F) == 0x05) { // File directory entry
                    isDir = (entry->data[3] & 0x10) != 0;
                    status = (entry->entryType & 0x80) ? 1 : 0; // Allocation status
                } else if ((entry->entryType & 0x7F) == 0x00C) { // Stream extension
                    currentFirstCluster = *reinterpret_cast<uint32_t*>(&entry->data[19]);
                    currentFileSize = *reinterpret_cast<uint64_t*>(&entry->data[23]);
                } else if ((entry->entryType & 0x7F) == 0x01) { // File name
                    // LFN reconstruction simplified
                    currentFileName += "exfat_file";
                    if (isDir) {
                        dirClusters.push_back(currentFirstCluster);
                    } else {
                        callback(currentFileName, currentFileSize, currentFirstCluster, "/", status);
                    }
                    currentFileName = ""; // Reset
                }
            }
            clus = getNextCluster(clus);
        }
    }
}
