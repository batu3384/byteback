#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

Ext4Parser::Ext4Parser() {}
Ext4Parser::~Ext4Parser() {}

#pragma pack(push, 1)
struct Ext4_SuperBlock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // ... we only need up to magic (offset 0x38 = 56 bytes)
};
#pragma pack(pop)

bool Ext4Parser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
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
            // Ext4 superblock is at offset 1024 from the start of the partition
            // Since we don't know partition start, we look for magic 0xEF53 at offset 0x38 in any sector
            if (i + 1024 > res.bytesRead) break;
            
            // Check offset 0x38 (56) for 0xEF53
            uint16_t magic = *reinterpret_cast<uint16_t*>(buffer.data() + i + 56);
            if (magic == 0xEF53) {
                Ext4_SuperBlock* sb = reinterpret_cast<Ext4_SuperBlock*>(buffer.data() + i);
                
                // Found a Superblock, carve a dummy file representing this volume structure for now
                // A full Ext4 parser would parse Block Group Descriptors here.
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = "Ext4_Superblock_" + std::to_string(foundCount) + ".bin";
                fr.extension = "bin";
                fr.path = "/recovered_ext4/" + fr.name;
                fr.sizeBytes = static_cast<uint64_t>(sb->s_blocks_count_lo) * (static_cast<uint64_t>(1024) << std::min(sb->s_log_block_size, 20u));
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + 1;
                fr.status = 1;
                fr.confidence = 90;
                fr.category = "System";
                fr.source = "ext4_sb";
                fr.createdAt = sb->s_mtime;
                fr.modifiedAt = sb->s_wtime;
                
                callback(fr);
                
                // Skip the rest of this sector to avoid false positives
                break;
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
