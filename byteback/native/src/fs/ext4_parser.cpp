#include "byteback_fs.h"
#include "byteback_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace byteback {

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

// Extent tree structures (i_block[] when EXT4_EXTENTS_FL is set).
struct Ext4_ExtentHeader {
    uint16_t eh_magic;   // 0xF30A
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;   // 0 = leaf node with extents, >0 = internal with indexes
    uint32_t eh_generation;
};
struct Ext4_Extent {
    uint32_t ee_block;      // logical first block
    uint16_t ee_len;        // length in blocks (high bit = unwritten)
    uint16_t ee_start_hi;   // high 16 bits of physical start
    uint32_t ee_start_lo;   // low 32 bits of physical start
};
struct Ext4_ExtentIdx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
};

// Directory entry inside a directory's data blocks.
struct Ext4_Dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    // name[] follows, NOT NUL-terminated
};
#pragma pack(pop)

namespace {
constexpr uint16_t EXT4_EXT_MAGIC = 0xF30A;
constexpr uint32_t EXT4_EXTENTS_FLAG = 0x80000;
constexpr uint64_t EXT4_ROOT_INO = 2;

// Metadata extracted in pass 1, consumed in pass 2.
struct InodeMeta {
    uint64_t size = 0;
    std::vector<FileRecord::DataRun> runs;
    int64_t crtime = 0;
    int64_t mtime = 0;
    bool deleted = false;
};
} // namespace

// Walk an extent tree rooted in i_block[] and append physical runs to `runs`.
// depth>0 nodes are read from disk recursively (bounded to prevent cycles on
// corrupt trees). Physical runs are converted to sector units.
static void collectExtentRuns(DiskReader& reader, const uint8_t* nodeBytes,
                              uint32_t blockSize, uint32_t sectorSize,
                              uint64_t volumeOffsetBytes,
                              std::vector<FileRecord::DataRun>& runs,
                              int depthBudget) {
    const Ext4_ExtentHeader* hdr = reinterpret_cast<const Ext4_ExtentHeader*>(nodeBytes);
    if (hdr->eh_magic != EXT4_EXT_MAGIC) return;
    if (depthBudget <= 0) return; // corrupt/deep tree guard

    if (hdr->eh_depth == 0) {
        const Ext4_Extent* ext = reinterpret_cast<const Ext4_Extent*>(nodeBytes + sizeof(Ext4_ExtentHeader));
        for (uint16_t i = 0; i < hdr->eh_entries; ++i) {
            uint64_t physBlock = (static_cast<uint64_t>(ext[i].ee_start_hi) << 32) | ext[i].ee_start_lo;
            uint64_t len = ext[i].ee_len & 0x7FFF; // mask the unwritten bit
            if (len == 0 || physBlock == 0) continue;
            FileRecord::DataRun run;
            run.startSector = (volumeOffsetBytes + physBlock * blockSize) / sectorSize;
            run.sectorCount = len * blockSize / sectorSize;
            runs.push_back(run);
        }
        return;
    }

    // Internal node: read each child block and recurse.
    const Ext4_ExtentIdx* idx = reinterpret_cast<const Ext4_ExtentIdx*>(nodeBytes + sizeof(Ext4_ExtentHeader));
    for (uint16_t i = 0; i < hdr->eh_entries; ++i) {
        uint64_t childBlock = (static_cast<uint64_t>(idx[i].ei_leaf_hi) << 32) | idx[i].ei_leaf_lo;
        if (childBlock == 0) continue;
        std::vector<uint8_t> child(blockSize);
        // Read via the aligned helper pattern: block offsets are sector-
        // aligned in practice (blockSize >= 1024, multiple of 512).
        if (!reader.readSectors(volumeOffsetBytes + childBlock * blockSize, blockSize, child.data()).success) continue;
        collectExtentRuns(reader, child.data(), blockSize, sectorSize, volumeOffsetBytes, runs, depthBudget - 1);
    }
}

bool Ext4Parser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0);
}

bool Ext4Parser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                        uint64_t volumeOffsetBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint32_t sb_read_len = ((2048 + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> sb_buffer(sb_read_len);
    if (!reader.readSectors(volumeOffsetBytes, sb_read_len, sb_buffer.data()).success) return false;

    Ext4_SuperBlock* sb = reinterpret_cast<Ext4_SuperBlock*>(sb_buffer.data() + 1024);
    if (sb->s_magic != 0xEF53) {
        bool found = false;
        uint32_t search_len = 4 * 1024 * 1024;
        std::vector<uint8_t> search_buf(search_len);
        if (reader.readSectors(volumeOffsetBytes, search_len, search_buf.data()).success) {
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
    uint64_t gdt_offset = volumeOffsetBytes + gdt_block * block_size;

    uint64_t gdt_bytes = static_cast<uint64_t>(num_groups) * desc_size;
    uint32_t gdt_read_len = ((gdt_bytes + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> gdt_buffer(gdt_read_len);

    if (!reader.readSectors(gdt_offset, gdt_read_len, gdt_buffer.data()).success) return false;

    // ---- Pass 1: inode tables -> metadata map (extent runs, sizes, times) ----
    // ponytail: the map holds every regular/directory inode in memory; for a
    // 10M-inode filesystem that is ~1 GiB. Upgrade path: spill to the SQLite
    // store and stream pass 2 from it.
    std::unordered_map<uint32_t, InodeMeta> inodeMap;
    std::vector<uint32_t> dirInodes;

    for (uint32_t g = 0; g < num_groups; ++g) {
        if (isRunning && !(*isRunning)) break;

        Ext4_GroupDesc* gd = reinterpret_cast<Ext4_GroupDesc*>(gdt_buffer.data() + g * desc_size);

        uint64_t inode_table_block = static_cast<uint64_t>(gd->bg_inode_table_lo);
        if (desc_size > 32) {
            inode_table_block |= (static_cast<uint64_t>(gd->bg_inode_table_hi) << 32);
        }

        if (inode_table_block == 0) continue;

        uint64_t inode_table_offset = volumeOffsetBytes + inode_table_block * block_size;
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

                if (inode->i_mode == 0) continue;

                bool is_regular = (inode->i_mode & 0xF000) == 0x8000;
                bool is_directory = (inode->i_mode & 0xF000) == 0x4000;
                bool allocated = inode->i_links_count > 0 && inode->i_dtime == 0;

                if (!is_regular && !is_directory) continue;

                uint64_t file_size = inode->i_size_lo;
                if (is_regular) {
                    file_size |= (static_cast<uint64_t>(inode->i_size_hi) << 32);
                }

                uint32_t inode_idx = (offset / inode_size) + i;
                uint32_t inode_num = g * inodes_per_group + inode_idx + 1;
                if (inode_num < 11) continue; // reserved metadata inodes

                InodeMeta meta;
                meta.size = file_size;
                meta.crtime = inode->i_crtime;
                meta.mtime = inode->i_mtime;
                meta.deleted = !allocated;

                if (inode->i_flags & EXT4_EXTENTS_FLAG) {
                    collectExtentRuns(reader, reinterpret_cast<const uint8_t*>(inode->i_block),
                                      block_size, sectorSize, volumeOffsetBytes, meta.runs, 5);
                } else if (inode->i_block[0] != 0) {
                    // Legacy direct-block mapping: only the first direct block
                    // is mapped (indirect blocks are not followed yet).
                    FileRecord::DataRun run;
                    run.startSector = (volumeOffsetBytes + static_cast<uint64_t>(inode->i_block[0]) * block_size) / sectorSize;
                    run.sectorCount = std::min((uint64_t)block_size / sectorSize, (file_size + sectorSize - 1) / sectorSize);
                    meta.runs.push_back(run);
                }

                if (is_directory && !meta.runs.empty()) {
                    dirInodes.push_back(inode_num);
                }
                inodeMap[inode_num] = std::move(meta);
            }
        }

        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = (inode_table_block * block_size) / sectorSize;
        callback(progressTick);
    }

    // ---- Pass 2: walk directory blocks -> real names, parents, deleted slack ----
    auto emitFile = [&](const std::string& name, uint32_t ino, const InodeMeta* meta, int confidence, int status) {
        FileRecord fr;
        fr.id = ino;
        fr.parentId = 0;
        fr.name = name;
        size_t dotPos = name.find_last_of('.');
        fr.extension = (dotPos != std::string::npos && dotPos + 1 < name.size())
                       ? name.substr(dotPos + 1) : "";
        fr.path = "/" + name; // placeholder; ext4 lacks parent ids in dirents
        fr.sizeBytes = meta ? meta->size : 0;
        if (meta && !meta->runs.empty()) {
            fr.startSector = meta->runs.front().startSector;
            fr.endSector = meta->runs.back().startSector + meta->runs.back().sectorCount;
            fr.runs = meta->runs;
        } else {
            fr.startSector = 0;
            fr.endSector = 0;
        }
        fr.status = status;
        fr.confidence = confidence;
        fr.category = "File";
        fr.source = "ext4_dirent";
        fr.createdAt = meta ? meta->crtime : 0;
        fr.modifiedAt = meta ? meta->mtime : 0;
        callback(fr);
    };

    std::vector<uint8_t> dirBuf;
    for (uint32_t dirIno : dirInodes) {
        if (isRunning && !(*isRunning)) break;
        auto it = inodeMap.find(dirIno);
        if (it == inodeMap.end()) continue;
        const InodeMeta& dm = it->second;

        for (const auto& run : dm.runs) {
            uint64_t runBytes = run.sectorCount * sectorSize;
            dirBuf.resize(static_cast<size_t>(runBytes));
            if (!reader.readSectors(run.startSector * sectorSize,
                                    static_cast<uint32_t>(runBytes), dirBuf.data()).success) {
                continue;
            }

            size_t pos = 0;
            while (pos + sizeof(Ext4_Dirent) <= dirBuf.size()) {
                const Ext4_Dirent* de = reinterpret_cast<const Ext4_Dirent*>(dirBuf.data() + pos);
                if (de->rec_len < 8 || pos + de->rec_len > dirBuf.size()) break; // corrupt

                if (de->name_len > 0 && de->name_len < de->rec_len) {
                    std::string name(reinterpret_cast<const char*>(dirBuf.data() + pos + sizeof(Ext4_Dirent)),
                                     de->name_len);
                    bool nameOk = true;
                    for (char c : name) {
                        if (static_cast<unsigned char>(c) < 0x20 || c == '/') { nameOk = false; break; }
                    }

                    if (nameOk && name != "." && name != "..") {
                        if (de->inode != 0) {
                            // Live entry: look up the inode's metadata.
                            auto mit = inodeMap.find(de->inode);
                            if (mit != inodeMap.end()) {
                                const InodeMeta& meta = mit->second;
                                emitFile(name, de->inode, &meta,
                                         meta.deleted ? 60 : 95,
                                         meta.deleted ? 0 : 1);
                            } else {
                                emitFile(name, de->inode, nullptr, 50, 1);
                            }
                        } else if (de->file_type == 1 /*EXT4_FT_REG_FILE*/) {
                            // inode == 0 with a surviving name: the entry was
                            // deleted and its space merged into this rec_len.
                            // The name is forensic evidence even though the
                            // inode link is gone.
                            emitFile(name, 0, nullptr, 45, 0);
                        }
                    }
                }
                pos += de->rec_len;
            }
        }
    }

    return true;
}

} // namespace byteback
