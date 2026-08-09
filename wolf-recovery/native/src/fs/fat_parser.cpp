#include "wolf_fs.h"
#include <iostream>
#include <cstring>
#include <vector>

namespace wolf {

#pragma pack(push, 1)

// Basic FAT32 Boot Sector
struct FAT32_VBR {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_dir_entries;
    uint16_t total_sectors_16;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
};

// Standard 32-byte FAT Directory Entry
struct FAT_DIR_ENTRY {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_low;
    uint32_t file_size;
};

// Long File Name (LFN) Entry
struct FAT_LFN_ENTRY {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attributes; // Always 0x0F
    uint8_t type;       // Always 0x00
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster; // Always 0x0000
    uint16_t name3[2];
};

#pragma pack(pop)

FATParser::FATParser() {}
FATParser::~FATParser() {}

bool FATParser::scan(DiskReader& reader, FileRecordCallback callback) {
    if (!reader.isOpen()) return false;
    
    std::vector<uint8_t> vbrBuf(512);
    auto res = reader.readSectors(0, 512, vbrBuf.data());
    if (!res.success) return false;

    const FAT32_VBR* vbr = reinterpret_cast<const FAT32_VBR*>(vbrBuf.data());
    
    if (vbr->bytes_per_sector != 512 && vbr->bytes_per_sector != 4096) {
        return false;
    }
    
    if (vbr->num_fats == 0 || vbr->sectors_per_cluster == 0) {
        return false;
    }

    uint64_t fatStartSector = vbr->reserved_sectors;
    uint64_t dataStartSector = fatStartSector + (vbr->num_fats * vbr->sectors_per_fat_32);
    uint32_t clusterSize = vbr->sectors_per_cluster * vbr->bytes_per_sector;
    
    uint64_t diskSize = reader.getDiskSize();
    uint64_t maxSector = diskSize / vbr->bytes_per_sector;
    if (maxSector > dataStartSector + 1000000) {
        maxSector = dataStartSector + 1000000;
    }

    const uint32_t chunkSectors = 2048; 
    const uint32_t chunkSize = chunkSectors * vbr->bytes_per_sector;
    std::vector<uint8_t> clusterBuf(chunkSize);
    
    for (uint64_t sector = dataStartSector; sector < maxSector; sector += chunkSectors) {
        auto readRes = reader.readSectors(sector * vbr->bytes_per_sector, chunkSize, clusterBuf.data());
        if (!readRes.success || readRes.bytesRead == 0) continue;

        for (uint32_t i = 0; i + 32 <= readRes.bytesRead; i += 32) {
            const uint8_t* entryData = clusterBuf.data() + i;
            
            if (entryData[0] == 0x00) continue;
            
            if (entryData[11] == 0x0F) {
                continue;
            }

            const FAT_DIR_ENTRY* dir = reinterpret_cast<const FAT_DIR_ENTRY*>(entryData);
            
            if ((dir->attributes & ~(0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20)) != 0) continue;
            if (dir->attributes == 0x08) continue; 

            bool isDeleted = (dir->name[0] == 0xE5);
            
            std::string name;
            for (int j = 0; j < 8; ++j) {
                if (dir->name[j] == 0x20) break;
                name += (j == 0 && isDeleted) ? '_' : static_cast<char>(dir->name[j]);
            }
            
            std::string ext;
            for (int j = 8; j < 11; ++j) {
                if (dir->name[j] == 0x20) break;
                ext += static_cast<char>(dir->name[j]);
            }
            
            if (!ext.empty()) {
                name += "." + ext;
            } else {
                ext = "";
            }

            if (name.empty() || name == "." || name == "..") continue;

            uint32_t firstCluster = (static_cast<uint32_t>(dir->cluster_high) << 16) | dir->cluster_low;
            uint64_t startSector = 0;
            if (firstCluster >= 2) {
                startSector = dataStartSector + ((firstCluster - 2) * vbr->sectors_per_cluster);
            }

            FileRecord fr;
            fr.id = 0;
            fr.parentId = 0;
            fr.name = name;
            fr.extension = ext;
            fr.path = "/" + name;
            fr.sizeBytes = dir->file_size;
            fr.startSector = startSector;
            fr.endSector = startSector + ((dir->file_size + vbr->bytes_per_sector - 1) / vbr->bytes_per_sector);
            fr.status = isDeleted ? 3 : 0; 
            fr.confidence = isDeleted ? 50 : 100;
            fr.category = ""; 
            fr.source = "fat";
            fr.createdAt = 0; 
            fr.modifiedAt = 0;
            
            callback(fr);
        }
    }

    return true;
}

} // namespace wolf
