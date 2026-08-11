#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

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
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
};

struct Ext4_GroupDesc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
};

struct Ext4_Inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_hi;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_hi;
    uint16_t i_file_acl_hi;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
};
#pragma pack(pop)

bool Ext4Parser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen()) return false;
    
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    uint32_t sb_read_len = ((2048 + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> sb_buffer(sb_read_len);
    if (!reader.readSectors(0, sb_read_len, sb_buffer.data()).success) return false;
    
    Ext4_SuperBlock* sb = reinterpret_cast<Ext4_SuperBlock*>(sb_buffer.data() + 1024);
    if (sb->s_magic != 0xEF53) {
        bool found = false;
        uint32_t search_len = 4 * 1024 * 1024;
        std::vector<uint8_t> search_buf(search_len);
        if (reader.readSectors(0, search_len, search_buf.data()).success) {
            for (uint32_t i = 1024; i < search_len - 1024; i += 512) {
                sb = reinterpret_cast<Ext4_SuperBlock*>(search_buf.data() + i);
                if (sb->s_magic == 0xEF53) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) return false;
    }
    
    uint32_t block_size = 1024 << sb->s_log_block_size;
    uint32_t inodes_per_group = sb->s_inodes_per_group;
    uint64_t blocks_count = static_cast<uint64_t>(sb->s_blocks_count_lo);
    if ((sb->s_feature_incompat & 0x80) != 0) {
        blocks_count |= (static_cast<uint64_t>(sb->s_blocks_count_hi) << 32);
    }
    uint32_t blocks_per_group = sb->s_blocks_per_group;
    
    if (blocks_per_group == 0 || inodes_per_group == 0) return false;
    
    uint32_t num_groups = (blocks_count + blocks_per_group - 1) / blocks_per_group;
    
    uint16_t inode_size = sb->s_inode_size;
    if (inode_size == 0) inode_size = 128;
    
    uint32_t desc_size = sb->s_desc_size;
    if ((sb->s_feature_incompat & 0x80) == 0 || desc_size < 32) {
        desc_size = 32;
    }

    uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
    uint64_t gdt_offset = gdt_block * block_size;
    
    uint64_t gdt_bytes = static_cast<uint64_t>(num_groups) * desc_size;
    uint32_t gdt_read_len = ((gdt_bytes + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> gdt_buffer(gdt_read_len);
    
    if (!reader.readSectors(gdt_offset, gdt_read_len, gdt_buffer.data()).success) return false;
    
    for (uint32_t g = 0; g < num_groups; ++g) {
        if (isRunning && !(*isRunning)) break;
        
        Ext4_GroupDesc* gd = reinterpret_cast<Ext4_GroupDesc*>(gdt_buffer.data() + g * desc_size);
        
        uint64_t inode_table_block = static_cast<uint64_t>(gd->bg_inode_table_lo);
        if (desc_size > 32) {
            inode_table_block |= (static_cast<uint64_t>(gd->bg_inode_table_hi) << 32);
        }
        
        if (inode_table_block == 0) continue;
        
        uint64_t inode_table_offset = inode_table_block * block_size;
        uint64_t inode_table_bytes = static_cast<uint64_t>(inodes_per_group) * inode_size;
        
        uint32_t inode_read_len = ((inode_table_bytes + sectorSize - 1) / sectorSize) * sectorSize;
        
        uint32_t chunk_size = 1024 * 1024;
        if (chunk_size % sectorSize != 0) chunk_size = (chunk_size / sectorSize) * sectorSize;
        
        std::vector<uint8_t> inode_buffer(std::min((uint32_t)inode_read_len, chunk_size));
        
        for (uint32_t offset = 0; offset < inode_read_len; offset += chunk_size) {
            if (isRunning && !(*isRunning)) break;
            
            uint32_t to_read = std::min(chunk_size, inode_read_len - offset);
            if (!reader.readSectors(inode_table_offset + offset, to_read, inode_buffer.data()).success) continue;
            
            uint32_t inodes_in_chunk = std::min((uint32_t)(to_read / inode_size), inodes_per_group - (offset / inode_size));
            
            for (uint32_t i = 0; i < inodes_in_chunk; ++i) {
                Ext4_Inode* inode = reinterpret_cast<Ext4_Inode*>(inode_buffer.data() + i * inode_size);
                
                if (inode->i_mode == 0 || inode->i_links_count == 0) continue;
                
                bool is_regular = (inode->i_mode & 0xF000) == 0x8000;
                bool is_directory = (inode->i_mode & 0xF000) == 0x4000;
                
                if (!is_regular && !is_directory) continue;
                
                uint64_t file_size = inode->i_size_lo;
                if (is_regular) {
                    file_size |= (static_cast<uint64_t>(inode->i_size_hi) << 32);
                }
                
                uint32_t inode_idx = (offset / inode_size) + i;
                uint32_t inode_num = g * inodes_per_group + inode_idx + 1;
                
                FileRecord fr;
                fr.id = inode_num;
                fr.parentId = 0;
                fr.name = (is_directory ? "dir_" : "file_") + std::to_string(inode_num);
                if (is_regular && file_size > 0) fr.extension = "bin";
                fr.path = "/";
                fr.sizeBytes = file_size;
                fr.startSector = (inode_table_offset + offset + i * inode_size) / sectorSize;
                fr.endSector = fr.startSector + (file_size + sectorSize - 1) / sectorSize;
                fr.status = 1;
                fr.confidence = 90;
                fr.category = is_directory ? "Directory" : "File";
                fr.source = "ext4_inode";
                fr.createdAt = inode->i_crtime;
                fr.modifiedAt = inode->i_mtime;
                
                if (inode->i_flags & 0x80000) {
                } else if (inode->i_block[0] != 0) {
                    FileRecord::DataRun run;
                    run.startSector = (static_cast<uint64_t>(inode->i_block[0]) * block_size) / sectorSize;
                    run.sectorCount = std::min((uint64_t)block_size / sectorSize, (file_size + sectorSize - 1) / sectorSize);
                    fr.runs.push_back(run);
                }
                
                callback(fr);
            }
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = (inode_table_block * block_size) / sectorSize;
        callback(progressTick);
    }
    
    return true;
}

} // namespace wolf
