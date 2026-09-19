#include "byteback_fs.h"
#include "byteback_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>

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

static_assert(offsetof(Ext4_SuperBlock, s_journal_inum) == 0xE0, "jbd2 s_journal_inum");

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
static void collectLegacyBlockRuns(DiskReader& reader, const uint32_t* i_block,
                                   uint32_t blockSize, uint32_t sectorSize,
                                   uint64_t volumeOffsetBytes, uint64_t fileSize,
                                   std::vector<FileRecord::DataRun>& runs, bool& unread) {
    if (!i_block || blockSize == 0) return;
    const uint32_t ptrsPerBlock = blockSize / 4;
    const uint64_t sectorsPerBlock = blockSize / sectorSize;
    uint64_t bytesMapped = 0;

    auto addBlock = [&](uint32_t blockNum) -> bool {
        if (blockNum == 0) return false;
        if (fileSize > 0 && bytesMapped >= fileSize) return false;
        FileRecord::DataRun run;
        run.startSector = (volumeOffsetBytes + static_cast<uint64_t>(blockNum) * blockSize) / sectorSize;
        run.sectorCount = sectorsPerBlock;
        runs.push_back(run);
        bytesMapped += blockSize;
        return true;
    };

    auto readBlockPtrs = [&](uint32_t blockNum, auto&& visitor) {
        if (blockNum == 0 || ptrsPerBlock == 0) return;
        std::vector<uint8_t> buf(blockSize);
        const uint64_t off = volumeOffsetBytes + static_cast<uint64_t>(blockNum) * blockSize;
        auto ptrRes = reader.readSectors(off, blockSize, buf.data());
        if (!readComplete(ptrRes, blockSize)) {
            unread = true;
            return;
        }
        for (uint32_t i = 0; i < ptrsPerBlock; ++i) {
            const uint32_t ptr = buf[i * 4] | (buf[i * 4 + 1] << 8) | (buf[i * 4 + 2] << 16) |
                                 (buf[i * 4 + 3] << 24);
            if (ptr == 0) break;
            if (!visitor(ptr)) break;
        }
    };

    for (int i = 0; i < 12; ++i) {
        if (!addBlock(i_block[i])) break;
    }
    if (fileSize > 0 && bytesMapped >= fileSize) return;

    if (i_block[12] != 0) {
        readBlockPtrs(i_block[12], [&](uint32_t ptr) { return addBlock(ptr); });
    }
    if (fileSize > 0 && bytesMapped >= fileSize) return;

    if (i_block[13] != 0) {
        readBlockPtrs(i_block[13], [&](uint32_t indirect) {
            if (indirect == 0) return false;
            readBlockPtrs(indirect, [&](uint32_t ptr) { return addBlock(ptr); });
            return fileSize == 0 || bytesMapped < fileSize;
        });
    }
    if (fileSize > 0 && bytesMapped >= fileSize) return;

    if (i_block[14] != 0) {
        readBlockPtrs(i_block[14], [&](uint32_t indirect1) {
            if (indirect1 == 0) return false;
            readBlockPtrs(indirect1, [&](uint32_t indirect2) {
                if (indirect2 == 0) return false;
                readBlockPtrs(indirect2, [&](uint32_t ptr) { return addBlock(ptr); });
                return fileSize == 0 || bytesMapped < fileSize;
            });
            return fileSize == 0 || bytesMapped < fileSize;
        });
    }
}

// Walk an extent tree rooted in i_block[] and append physical runs to `runs`.
// depth>0 nodes are read from disk recursively (bounded to prevent cycles on
// corrupt trees). Physical runs are converted to sector units. nodeLen is the
// capacity of nodeBytes (60 for the i_block root, blockSize for disk nodes):
// eh_entries is attacker-controlled and is clamped to fit before any array
// access — otherwise a crafted header reads past the node buffer.
static void collectExtentRuns(DiskReader& reader, const uint8_t* nodeBytes, size_t nodeLen,
                              uint32_t blockSize, uint32_t sectorSize,
                              uint64_t volumeOffsetBytes,
                              std::vector<FileRecord::DataRun>& runs,
                              int depthBudget, bool& unread) {
    if (nodeLen < sizeof(Ext4_ExtentHeader) + sizeof(Ext4_Extent)) return;
    const Ext4_ExtentHeader* hdr = reinterpret_cast<const Ext4_ExtentHeader*>(nodeBytes);
    if (hdr->eh_magic != EXT4_EXT_MAGIC) return;
    if (depthBudget <= 0) return; // corrupt/deep tree guard
    size_t maxEntries = (nodeLen - sizeof(Ext4_ExtentHeader)) / 12; // extent & idx are 12 bytes
    size_t entries = hdr->eh_entries > maxEntries ? maxEntries : hdr->eh_entries;

    if (hdr->eh_depth == 0) {
        const Ext4_Extent* ext = reinterpret_cast<const Ext4_Extent*>(nodeBytes + sizeof(Ext4_ExtentHeader));
        for (size_t i = 0; i < entries; ++i) {
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
    for (size_t i = 0; i < entries; ++i) {
        uint64_t childBlock = (static_cast<uint64_t>(idx[i].ei_leaf_hi) << 32) | idx[i].ei_leaf_lo;
        if (childBlock == 0) continue;
        std::vector<uint8_t> child(blockSize);
        // Read via the aligned helper pattern: block offsets are sector-
        // aligned in practice (blockSize >= 1024, multiple of 512).
        auto childRes = reader.readSectors(volumeOffsetBytes + childBlock * blockSize, blockSize, child.data());
        if (!readComplete(childRes, blockSize)) {
            unread = true;
            continue;
        }
        collectExtentRuns(reader, child.data(), child.size(), blockSize, sectorSize, volumeOffsetBytes, runs, depthBudget - 1, unread);
    }
}

constexpr uint32_t kJbd2Magic = 0xC03B3998;
constexpr uint32_t kJbd2Desc = 1;
constexpr uint32_t kJbd2Commit = 2;
constexpr uint32_t kJbd2FlagSameUuid = 2;
constexpr uint32_t kJbd2FlagDeleted = 4;
constexpr uint32_t kJbd2FlagLastTag = 8;

uint32_t jbd2Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct Jbd2Applied {
    uint32_t fsBlock = 0;
    size_t jbufOff = 0;
};

bool jbufOffToRun(const std::vector<FileRecord::DataRun>& runs, uint32_t sectorSize,
                  size_t off, uint32_t blockSize, FileRecord::DataRun& run) {
    if (sectorSize == 0) return false;
    uint64_t byteOff = 0;
    for (const auto& r : runs) {
        const uint64_t runBytes = r.sectorCount * static_cast<uint64_t>(sectorSize);
        if (off >= byteOff && off < byteOff + runBytes) {
            const uint64_t within = off - byteOff;
            run.startSector = r.startSector + within / sectorSize;
            run.sectorCount = (blockSize + sectorSize - 1) / sectorSize;
            return true;
        }
        byteOff += runBytes;
    }
    return false;
}

// Walk descriptor+commit. Uncommitted data is marked consumed so the naive
// dirent pass cannot treat it as a live name. Disk is never written.
void walkCommittedJbd2(const std::vector<uint8_t>& jbuf, uint32_t blockSize,
                       std::vector<Jbd2Applied>& out, std::unordered_set<size_t>& consumed) {
    if (blockSize < 36 || jbuf.size() < blockSize) return;
    const uint8_t* sb = jbuf.data();
    if (jbd2Be32(sb) != kJbd2Magic) return;
    const uint32_t btype = jbd2Be32(sb + 4);
    if (btype != 3 && btype != 4) return;
    const uint32_t jbs = jbd2Be32(sb + 12);
    if (jbs != blockSize) return;
    const uint32_t sFirst = jbd2Be32(sb + 20);
    const uint32_t sStart = jbd2Be32(sb + 28);
    consumed.insert(0);
    size_t rel = sStart ? sStart : (sFirst ? sFirst : 1);
    int hops = 0;
    while (rel * static_cast<size_t>(blockSize) + blockSize <= jbuf.size() && hops++ < 4096) {
        const size_t jb = rel * blockSize;
        const uint8_t* p = jbuf.data() + jb;
        if (jbd2Be32(p) != kJbd2Magic) {
            ++rel;
            continue;
        }
        consumed.insert(rel);
        const uint32_t type = jbd2Be32(p + 4);
        const uint32_t seq = jbd2Be32(p + 8);
        if (type != kJbd2Desc) {
            ++rel;
            continue;
        }
        std::vector<uint32_t> tags;
        size_t tagOff = 12;
        while (tagOff + 8 <= blockSize) {
            const uint32_t tblock = jbd2Be32(p + tagOff);
            const uint32_t tflags = jbd2Be32(p + tagOff + 4);
            tagOff += 8;
            if ((tflags & kJbd2FlagSameUuid) == 0) {
                if (tagOff + 16 > blockSize) break;
                tagOff += 16;
            }
            if ((tflags & kJbd2FlagDeleted) == 0) tags.push_back(tblock);
            if (tflags & kJbd2FlagLastTag) break;
        }
        bool dataOk = true;
        for (size_t i = 0; i < tags.size(); ++i) {
            const size_t drel = rel + 1 + i;
            if (drel * static_cast<size_t>(blockSize) + blockSize > jbuf.size()) {
                dataOk = false;
                break;
            }
            consumed.insert(drel);
        }
        if (!dataOk) break;
        const size_t crel = rel + 1 + tags.size();
        bool committed = false;
        if (crel * static_cast<size_t>(blockSize) + blockSize <= jbuf.size()) {
            const uint8_t* cp = jbuf.data() + crel * blockSize;
            committed = jbd2Be32(cp) == kJbd2Magic && jbd2Be32(cp + 4) == kJbd2Commit &&
                        jbd2Be32(cp + 8) == seq;
        }
        if (committed) {
            consumed.insert(crel);
            for (size_t i = 0; i < tags.size(); ++i) {
                Jbd2Applied a;
                a.fsBlock = tags[i];
                a.jbufOff = (rel + 1 + i) * blockSize;
                out.push_back(a);
            }
            rel = crel + 1;
        } else {
            rel = rel + 1 + tags.size();
        }
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
    if (!readComplete(reader.readSectors(volumeOffsetBytes, sb_read_len, sb_buffer.data()),
                      sb_read_len)) {
        return false;
    }

    Ext4_SuperBlock* sb = reinterpret_cast<Ext4_SuperBlock*>(sb_buffer.data() + 1024);
    auto acceptSb = [&](const uint8_t* p) {
        const auto* cand = reinterpret_cast<const Ext4_SuperBlock*>(p);
        if (cand->s_magic != 0xEF53 || cand->s_log_block_size > 6) return false;
        const size_t n = std::min<size_t>(1024, sb_buffer.size() - 1024);
        std::memcpy(sb_buffer.data() + 1024, p, n);
        sb = reinterpret_cast<Ext4_SuperBlock*>(sb_buffer.data() + 1024);
        return true;
    };
    if (sb->s_magic != 0xEF53) {
        bool found = false;
        uint32_t search_len = 4 * 1024 * 1024;
        std::vector<uint8_t> search_buf(search_len);
        auto searchRes = reader.readSectors(volumeOffsetBytes, search_len, search_buf.data());
        const uint64_t walk = readComplete(searchRes, search_len)
            ? search_len
            : (searchRes.success ? std::min<uint64_t>(searchRes.bytesRead, search_len) : 0);
        for (uint32_t i = 1024; i + 1024 < walk; i += 512) {
            if (acceptSb(search_buf.data() + i)) {
                found = true;
                break;
            }
        }
        if (!found) {
            const uint64_t disk = reader.getDiskSize();
            const uint64_t extra[] = {
                4ull << 20,
                8193ull * 1024,
                16384ull * 1024,
                32768ull * 4096,
            };
            std::vector<uint8_t> extraBuf(2048);
            for (uint64_t rel : extra) {
                const uint64_t abs = volumeOffsetBytes + rel;
                if (abs + extraBuf.size() > disk) continue;
                if (!readComplete(reader.readSectors(abs, static_cast<uint32_t>(extraBuf.size()),
                                                     extraBuf.data()),
                                  extraBuf.size())) {
                    continue;
                }
                if (acceptSb(extraBuf.data()) || acceptSb(extraBuf.data() + 1024)) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) return false;
    }
    // s_log_block_size is validated (0..6 => 1K..64K blocks); a larger value
    // would be a shift UB and an absurd block size.
    if (sb->s_log_block_size > 6) return false;

    {
        size_t n = 16;
        while (n > 0 && (sb->s_volume_name[n - 1] == 0 || sb->s_volume_name[n - 1] == ' ')) --n;
        if (n > 0) {
            FileRecord fr;
            fr.id = -1;
            fr.name.assign(sb->s_volume_name, n);
            fr.path = "/";
            fr.status = 1;
            fr.confidence = 5;
            fr.category = "System";
            fr.source = "ext4_vol_name";
            callback(fr);
        }
    }

    uint32_t block_size = 1024 << sb->s_log_block_size;
    uint32_t inodes_per_group = sb->s_inodes_per_group;
    uint64_t blocks_count = static_cast<uint64_t>(sb->s_blocks_count_lo);
    if ((sb->s_feature_incompat & 0x80) != 0) {
        blocks_count |= (static_cast<uint64_t>(sb->s_blocks_count_hi) << 32);
    }
    uint32_t blocks_per_group = sb->s_blocks_per_group;

    if (blocks_per_group == 0 || inodes_per_group == 0) return false;

    // Clamp to what the underlying reader can actually hold: a corrupt 64-bit
    // block count would otherwise size the group-descriptor buffer in TiB.
    const uint64_t spanBytes = reader.getDiskSize() > volumeOffsetBytes
        ? reader.getDiskSize() - volumeOffsetBytes : 0;
    const uint64_t maxBlocksBySpan = spanBytes / block_size;
    if (blocks_count > maxBlocksBySpan) blocks_count = maxBlocksBySpan;

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
    std::vector<uint8_t> gdt_buffer(gdt_read_len, 0);

    // ---- Pass 1: inode tables -> metadata map (extent runs, sizes, times) ----
    // ponytail: the map holds every regular/directory inode in memory; for a
    // 10M-inode filesystem that is ~1 GiB. Upgrade path: spill to the SQLite
    // store and stream pass 2 from it.
    std::unordered_map<uint32_t, InodeMeta> inodeMap;
    std::vector<uint32_t> dirInodes;
    bool dirUnread = false;
    const uint32_t journalIno = sb->s_journal_inum;
    InodeMeta journalMeta;
    bool haveJournal = false;
    bool journalUnread = false;

    std::vector<uint8_t> gdtSector(sectorSize);
    auto loadGdtAt = [&](uint64_t off) {
        std::fill(gdt_buffer.begin(), gdt_buffer.end(), 0);
        bool anyUnread = false;
        for (uint32_t o = 0; o < gdt_read_len; o += sectorSize) {
            uint32_t take = std::min(sectorSize, gdt_read_len - o);
            auto gdtRes = reader.readSectors(off + o, take, gdtSector.data());
            if (!readComplete(gdtRes, take)) {
                anyUnread = true;
                continue;
            }
            std::memcpy(gdt_buffer.data() + o, gdtSector.data(), take);
        }
        return anyUnread;
    };
    auto gdtGroup0HasInodeTable = [&]() {
        if (gdt_buffer.size() < desc_size) return false;
        const auto* gd0 = reinterpret_cast<const Ext4_GroupDesc*>(gdt_buffer.data());
        uint64_t it = gd0->bg_inode_table_lo;
        if (desc_size > 32) it |= static_cast<uint64_t>(gd0->bg_inode_table_hi) << 32;
        return it != 0;
    };

    if (loadGdtAt(gdt_offset)) dirUnread = true;
    if (!gdtGroup0HasInodeTable()) {
        const uint64_t group0Sb = (block_size == 1024) ? 1ull : 0ull;
        const uint32_t tryGroups[] = {1, 3, 5, 7};
        for (uint32_t g : tryGroups) {
            if (g >= num_groups) break;
            const uint64_t bakBlock = group0Sb + static_cast<uint64_t>(g) * blocks_per_group + 1ull;
            const uint64_t bakOff = volumeOffsetBytes + bakBlock * block_size;
            const uint64_t disk = reader.getDiskSize();
            if (bakOff + gdt_read_len > disk) continue;
            if (loadGdtAt(bakOff)) continue;
            if (gdtGroup0HasInodeTable()) break;
        }
    }

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
            auto inodeRes = reader.readSectors(inode_table_offset + offset, to_read, inode_buffer.data());
            if (!readComplete(inodeRes, to_read)) {
                dirUnread = true;
                continue;
            }

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
                if (inode_num < 11) {
                    if (journalIno != 0 && inode_num == journalIno && (is_regular || is_directory)) {
                        journalMeta.size = file_size;
                        journalMeta.crtime = inode->i_crtime;
                        journalMeta.mtime = inode->i_mtime;
                        if (inode->i_flags & EXT4_EXTENTS_FLAG) {
                            collectExtentRuns(reader, reinterpret_cast<const uint8_t*>(inode->i_block),
                                              sizeof(inode->i_block),
                                              block_size, sectorSize, volumeOffsetBytes, journalMeta.runs, 5, journalUnread);
                        } else if (inode->i_block[0] != 0) {
                            collectLegacyBlockRuns(reader, inode->i_block, block_size, sectorSize,
                                                   volumeOffsetBytes, file_size, journalMeta.runs, journalUnread);
                        }
                        haveJournal = true;
                    }
                    continue;
                }

                InodeMeta meta;
                meta.size = file_size;
                meta.crtime = inode->i_crtime;
                meta.mtime = inode->i_mtime;
                meta.deleted = !allocated;

                if (inode->i_flags & EXT4_EXTENTS_FLAG) {
                    collectExtentRuns(reader, reinterpret_cast<const uint8_t*>(inode->i_block),
                                      sizeof(inode->i_block),
                                      block_size, sectorSize, volumeOffsetBytes, meta.runs, 5, dirUnread);
                } else if (inode->i_block[0] != 0) {
                    collectLegacyBlockRuns(reader, inode->i_block, block_size, sectorSize,
                                           volumeOffsetBytes, file_size, meta.runs, dirUnread);
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
    auto emitFile = [&](const std::string& name, uint32_t ino, const InodeMeta* meta, int confidence, int status,
                        const char* source = "ext4_dirent") {
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
        fr.source = source;
        fr.createdAt = meta ? meta->crtime : 0;
        fr.modifiedAt = meta ? meta->mtime : 0;
        callback(fr);
    };

    std::unordered_set<std::string> seenNames;

    std::vector<uint8_t> dirBuf;
    for (uint32_t dirIno : dirInodes) {
        if (isRunning && !(*isRunning)) break;
        auto it = inodeMap.find(dirIno);
        if (it == inodeMap.end()) continue;
        const InodeMeta& dm = it->second;

        for (const auto& run : dm.runs) {
            uint64_t runBytes = run.sectorCount * sectorSize;
            dirBuf.resize(static_cast<size_t>(runBytes));
            auto dirRes = reader.readSectors(run.startSector * sectorSize,
                                            static_cast<uint32_t>(runBytes), dirBuf.data());
            if (!readComplete(dirRes, runBytes)) {
                dirUnread = true;
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
                            auto mit = inodeMap.find(de->inode);
                            if (mit != inodeMap.end()) {
                                const InodeMeta& meta = mit->second;
                                emitFile(name, de->inode, &meta,
                                         meta.deleted ? 60 : 95,
                                         meta.deleted ? 0 : 1);
                            } else {
                                emitFile(name, de->inode, nullptr, 50, 1);
                            }
                            seenNames.insert(name);
                        } else if (de->file_type == 1 /*EXT4_FT_REG_FILE*/) {
                            emitFile(name, 0, nullptr, 45, 0);
                            seenNames.insert(name);
                        }
                    }
                }
                pos += de->rec_len;
            }
        }
    }

    if (dirUnread) {
        FileRecord fr;
        fr.id = -1;
        fr.name = "Ext4_DirectoryUnread";
        fr.path = "/ext4-dir-unread/";
        fr.status = 0;
        fr.confidence = 20;
        fr.category = "System";
        fr.source = "ext4_dir_unread";
        callback(fr);
    }

    if (haveJournal) {
        std::vector<uint8_t> jbuf;
        for (const auto& run : journalMeta.runs) {
            if (isRunning && !(*isRunning)) break;
            uint64_t runBytes = run.sectorCount * static_cast<uint64_t>(sectorSize);
            if (runBytes == 0 || runBytes > 8ull * 1024 * 1024) continue;
            const size_t at = jbuf.size();
            jbuf.resize(at + static_cast<size_t>(runBytes));
            auto jres = reader.readSectors(run.startSector * sectorSize,
                                           static_cast<uint32_t>(runBytes), jbuf.data() + at);
            if (!readComplete(jres, runBytes)) {
                journalUnread = true;
                jbuf.resize(at);
                continue;
            }
        }

        std::vector<Jbd2Applied> applied;
        std::unordered_set<size_t> consumed;
        walkCommittedJbd2(jbuf, block_size, applied, consumed);

        std::unordered_map<uint32_t, size_t> fsToOff;
        for (const auto& a : applied) fsToOff[a.fsBlock] = a.jbufOff;

        uint64_t itable = 0;
        if (num_groups > 0 && gdt_buffer.size() >= desc_size) {
            const auto* gd0 = reinterpret_cast<const Ext4_GroupDesc*>(gdt_buffer.data());
            itable = gd0->bg_inode_table_lo;
        }
        const uint32_t inodesPerBlk = inode_size ? (block_size / inode_size) : 0;
        const uint32_t itableBlocks = inodesPerBlk
            ? (inodes_per_group + inodesPerBlk - 1) / inodesPerBlk : 0;

        std::unordered_map<uint32_t, InodeMeta> jInodes;
        for (const auto& a : applied) {
            if (a.jbufOff + block_size > jbuf.size()) continue;
            const uint8_t* p = jbuf.data() + a.jbufOff;
            const bool inItable = itable != 0 && inodesPerBlk > 0 && a.fsBlock >= itable &&
                                  a.fsBlock < itable + itableBlocks;
            if (inItable) {
                const uint32_t first = 1 + static_cast<uint32_t>((a.fsBlock - itable) * inodesPerBlk);
                for (uint32_t i = 0; i < inodesPerBlk; ++i) {
                    const auto* ino = reinterpret_cast<const Ext4_Inode*>(p + i * inode_size);
                    if ((ino->i_mode & 0xF000) != 0x8000) continue;
                    InodeMeta m;
                    m.size = ino->i_size_lo;
                    m.deleted = ino->i_links_count == 0 || ino->i_dtime != 0;
                    FileRecord::DataRun run{};
                    auto dit = fsToOff.find(ino->i_block[0]);
                    if (dit != fsToOff.end() &&
                        jbufOffToRun(journalMeta.runs, sectorSize, dit->second, block_size, run)) {
                        m.runs.push_back(run);
                    }
                    jInodes[first + i] = std::move(m);
                }
            }
        }

        for (const auto& a : applied) {
            if (a.jbufOff + block_size > jbuf.size()) continue;
            const bool inItable = itable != 0 && inodesPerBlk > 0 && a.fsBlock >= itable &&
                                  a.fsBlock < itable + itableBlocks;
            if (inItable) continue;
            const uint8_t* p = jbuf.data() + a.jbufOff;
            size_t pos = 0;
            while (pos + sizeof(Ext4_Dirent) <= block_size) {
                const Ext4_Dirent* de = reinterpret_cast<const Ext4_Dirent*>(p + pos);
                if (de->rec_len < 8 || pos + de->rec_len > block_size) break;
                if (de->name_len > 0 && de->name_len < de->rec_len) {
                    std::string name(reinterpret_cast<const char*>(p + pos + sizeof(Ext4_Dirent)),
                                     de->name_len);
                    bool nameOk = true;
                    for (char c : name) {
                        if (static_cast<unsigned char>(c) < 0x20 || c == '/') { nameOk = false; break; }
                    }
                    if (nameOk && name != "." && name != ".." && !seenNames.count(name)) {
                        const InodeMeta* meta = nullptr;
                        auto iit = jInodes.find(de->inode);
                        if (iit != jInodes.end() && !iit->second.runs.empty()) meta = &iit->second;
                        if (meta) {
                            emitFile(name, de->inode, meta, 48, 0, "ext4_journal_replay");
                            seenNames.insert(name);
                        }
                    }
                }
                pos += de->rec_len;
            }
        }

        for (size_t blk = 0; blk + block_size <= jbuf.size(); blk += block_size) {
            if (consumed.count(blk / block_size)) continue;
            const uint8_t* p = jbuf.data() + blk;
            if (block_size >= 8 && jbd2Be32(p) == kJbd2Magic) continue;
            size_t pos = 0;
            while (pos + sizeof(Ext4_Dirent) <= block_size) {
                const Ext4_Dirent* de = reinterpret_cast<const Ext4_Dirent*>(p + pos);
                if (de->rec_len < 8 || pos + de->rec_len > block_size) break;
                if (de->name_len > 0 && de->name_len < de->rec_len) {
                    std::string name(reinterpret_cast<const char*>(p + pos + sizeof(Ext4_Dirent)),
                                     de->name_len);
                    bool nameOk = true;
                    for (char c : name) {
                        if (static_cast<unsigned char>(c) < 0x20 || c == '/') { nameOk = false; break; }
                    }
                    if (nameOk && name != "." && name != ".." && !seenNames.count(name)) {
                        emitFile(name, de->inode, nullptr, 40, 0, "ext4_journal");
                        seenNames.insert(name);
                    }
                }
                pos += de->rec_len;
            }
        }
    }

    if (journalUnread) {
        FileRecord fr;
        fr.id = -1;
        fr.name = "Ext4_JournalUnread";
        fr.path = "/ext4-journal-unread/";
        fr.status = 0;
        fr.confidence = 20;
        fr.category = "System";
        fr.source = "ext4_journal_unread";
        callback(fr);
    }

    return true;
}

} // namespace byteback
