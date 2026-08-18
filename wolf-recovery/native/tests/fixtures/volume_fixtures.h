#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

namespace wolf::testfix {

inline void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    if (off + 2 > img.size()) img.resize(off + 2, 0);
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    if (off + 4 > img.size()) img.resize(off + 4, 0);
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

#pragma pack(push, 1)
struct TestFatBpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t  numFats;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t  media;
    uint16_t fatSize16;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    uint8_t  drive;
    uint8_t  reserved1;
    uint8_t  bootSig;
    uint32_t volId;
    char     volLabel[11];
    char     fsType[8];
};
#pragma pack(pop)

// Minimal FAT16 superfloppy with one file TEST.TXT (cluster 2, 11 bytes).
inline std::vector<uint8_t> buildFat16Volume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t fatSectors = 16;
    constexpr uint32_t totalSectors = 4120; // countOfClusters >= 4085 => FAT16
    std::vector<uint8_t> img(totalSectors * ss, 0);

    TestFatBpb bpb{};
    bpb.jmp[0] = 0xEB; bpb.jmp[1] = 0x3C; bpb.jmp[2] = 0x90;
    std::memcpy(bpb.oem, "MSWIN4.1", 8);
    bpb.bytesPerSector = static_cast<uint16_t>(ss);
    bpb.sectorsPerCluster = 1;
    bpb.reservedSectors = 1;
    bpb.numFats = 2;
    bpb.rootEntryCount = 16;
    bpb.media = 0xF8;
    bpb.fatSize16 = static_cast<uint16_t>(fatSectors);
    bpb.totalSectors32 = totalSectors;
    bpb.bootSig = 0x29;
    std::memcpy(bpb.fsType, "FAT16   ", 8);
    std::memcpy(img.data(), &bpb, sizeof(bpb));
    img[510] = 0x55; img[511] = 0xAA;

    const uint32_t fatStart = bpb.reservedSectors;
    const uint32_t rootStart = fatStart + bpb.numFats * fatSectors;
    const uint32_t dataStart = rootStart + 1;

    writeLe16(img, fatStart * ss + 2 * 2, 0xFFFF);

    size_t de = rootStart * ss;
    std::memcpy(img.data() + de, "TEST    TXT", 11);
    img[de + 11] = 0x20;
    writeLe16(img, de + 26, 2);
    writeLe32(img, de + 28, 11);

    const char payload[] = "Hello FAT16";
    std::memcpy(img.data() + dataStart * ss, payload, sizeof(payload) - 1);
    return img;
}

inline std::vector<uint8_t> buildMbrDiskWithFatPartition(const std::vector<uint8_t>& fatVol,
                                                         uint32_t partStartSector = 2048) {
    constexpr uint32_t ss = 512;
    uint32_t partSectors = static_cast<uint32_t>(fatVol.size() / ss);
    uint32_t diskSectors = partStartSector + partSectors;
    std::vector<uint8_t> disk(diskSectors * ss, 0);
    std::memcpy(disk.data() + partStartSector * ss, fatVol.data(), fatVol.size());

    // MBR partition 1: type 0x06 FAT16
    disk[510] = 0x55; disk[511] = 0xAA;
    size_t pe = 446;
    disk[pe] = 0x00;
    writeLe32(disk, pe + 8, partStartSector);
    writeLe32(disk, pe + 12, partSectors);
    disk[pe + 4] = 0x06;
    return disk;
}

inline std::vector<uint8_t> buildPngCarveDisk() {
    std::vector<uint8_t> img(64 * 1024, 0);
    // PNG signature + minimal IHDR chunk + IEND
    const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(img.data() + 4096, sig, sizeof(sig));
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                            0xAE, 0x42, 0x60, 0x82};
    std::memcpy(img.data() + 8192 - sizeof(iend), iend, sizeof(iend));
    return img;
}

// Two equal disks for RAID0 stripe tests (64 KiB blocks).
inline std::pair<std::vector<uint8_t>, std::vector<uint8_t>> buildRaid0MemberDisks() {
    constexpr size_t kSize = 128 * 1024;
    std::vector<uint8_t> d0(kSize, 0);
    std::vector<uint8_t> d1(kSize, 0);
    for (size_t i = 0; i < 64 * 1024; ++i) d0[i] = static_cast<uint8_t>('A');
    for (size_t i = 0; i < 64 * 1024; ++i) d1[i] = static_cast<uint8_t>('B');
    return {d0, d1};
}

#pragma pack(push, 1)
struct TestExt4Sb {
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
};
struct TestExt4Gd {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
};
#pragma pack(pop)

// Minimal ext4: one regular file inode + root dir listing "note.txt".
inline std::vector<uint8_t> buildExt4Volume() {
    constexpr uint32_t bs = 1024;
    constexpr uint32_t blocks = 64;
    std::vector<uint8_t> img(blocks * bs, 0);

    TestExt4Sb sb{};
    sb.s_inodes_count = 32;
    sb.s_blocks_count_lo = blocks;
    sb.s_first_data_block = 1;
    sb.s_log_block_size = 0;
    sb.s_blocks_per_group = 8;
    sb.s_inodes_per_group = 16;
    sb.s_magic = 0xEF53;
    sb.s_inode_size = 128;
    std::memcpy(img.data() + bs, &sb, sizeof(sb));

    TestExt4Gd gd{};
    gd.bg_inode_table_lo = 5;
    std::memcpy(img.data() + 2 * bs, &gd, sizeof(gd));

    auto writeInode = [&](uint32_t ino, uint16_t mode, uint32_t size, uint32_t block, uint16_t links = 1) {
        size_t itab = 5 * bs;
        size_t off = itab + (ino - 1) * 128;
        writeLe16(img, off + 0x00, mode);
        writeLe16(img, off + 0x18, links);
        writeLe32(img, off + 0x04, size);
        writeLe32(img, off + 0x28, block); // i_block[0]
    };

    writeInode(12, 0x4000, bs, 6, 2); // directory (inode >= 11)
    writeInode(13, 0x8000, 8, 7); // file at block 7

    // Directory block 6: dirent for note.txt -> inode 13
    size_t dir = 6 * bs;
    writeLe32(img, dir + 0x00, 13);
    writeLe16(img, dir + 0x04, 16); // rec_len (8 header + 8 name)
    img[dir + 0x06] = 8;  // name_len
    img[dir + 0x07] = 1;  // file type REG
    std::memcpy(img.data() + dir + 8, "note.txt", 8);

    std::memcpy(img.data() + 7 * bs, "ext4data", 8);
    return img;
}

} // namespace wolf::testfix
