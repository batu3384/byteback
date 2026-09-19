#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include "carver/file_validators.h"

namespace byteback::testfix {

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

inline void writeBe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    if (off + 2 > img.size()) img.resize(off + 2, 0);
    img[off] = static_cast<uint8_t>(v >> 8);
    img[off + 1] = static_cast<uint8_t>(v & 0xFF);
}

inline void writeBe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    if (off + 4 > img.size()) img.resize(off + 4, 0);
    img[off] = static_cast<uint8_t>(v >> 24);
    img[off + 1] = static_cast<uint8_t>(v >> 16);
    img[off + 2] = static_cast<uint8_t>(v >> 8);
    img[off + 3] = static_cast<uint8_t>(v);
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

inline std::vector<uint8_t> buildApmDiskWithFatPartition(const std::vector<uint8_t>& fatVol,
                                                         uint32_t partStartSector = 64,
                                                         uint32_t apmBlk = 512) {
    constexpr uint32_t ss = 512;
    if (apmBlk != 512 && apmBlk != 2048) apmBlk = 512;
    uint32_t partSectors = static_cast<uint32_t>(fatVol.size() / ss);
    uint32_t diskSectors = partStartSector + partSectors;
    const uint32_t apmSecs = apmBlk / ss;
    if (apmSecs && (diskSectors % apmSecs) != 0)
        diskSectors += apmSecs - (diskSectors % apmSecs);
    std::vector<uint8_t> disk(diskSectors * ss, 0);
    std::memcpy(disk.data() + partStartSector * ss, fatVol.data(), fatVol.size());
    disk[0] = 'E';
    disk[1] = 'R';
    writeBe16(disk, 2, static_cast<uint16_t>(apmBlk));
    writeBe32(disk, 4, diskSectors / apmSecs);
    auto plantPm = [&](uint32_t apmIndex, uint32_t startApm, uint32_t cntApm, const char* type) {
        const size_t o = static_cast<size_t>(apmIndex) * apmBlk;
        disk[o] = 'P';
        disk[o + 1] = 'M';
        writeBe32(disk, o + 4, 2);
        writeBe32(disk, o + 8, startApm);
        writeBe32(disk, o + 12, cntApm);
        const size_t n = std::strlen(type);
        if (n > 31) return;
        std::memcpy(disk.data() + o + 48, type, n);
    };
    const uint32_t startApm = partStartSector * ss / apmBlk;
    const uint32_t cntApm = (partSectors * ss + apmBlk - 1) / apmBlk;
    plantPm(1, 1, 2, "Apple_partition_map");
    plantPm(2, startApm, cntApm, "Apple_HFS");
    return disk;
}

// Contiguous valid minimal PNG (SIG + IHDR + IEND). Carve fixtures must
// survive post-recovery structural validation (score >= 60) — SIG+IEND with
// zero-filled middle fails IHDR checks and correctly marks recover as failed.
inline std::vector<uint8_t> buildMinimalValidPng() {
    std::vector<uint8_t> png;
    const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + sizeof(sig));

    auto appendBe32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>(x & 0xFF));
    };

    // IHDR: length 13, type IHDR, 13 zero payload bytes, CRC(type+data)
    appendBe32(png, 13);
    const size_t ihdrTypeOff = png.size();
    const uint8_t ihdrType[] = {'I', 'H', 'D', 'R'};
    png.insert(png.end(), ihdrType, ihdrType + 4);
    png.insert(png.end(), 13, 0x00);
    appendBe32(png, carver::crc32(png.data() + ihdrTypeOff, 4 + 13));

    // IEND: length 0, type IEND, CRC(type) = 0xAE426082
    appendBe32(png, 0);
    const uint8_t iend[] = {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    png.insert(png.end(), iend, iend + sizeof(iend));
    return png;
}

inline std::vector<uint8_t> buildPngCarveDisk() {
    std::vector<uint8_t> img(64 * 1024, 0);
    const auto png = buildMinimalValidPng();
    constexpr size_t kOff = 4096;
    if (kOff + png.size() > img.size()) return img;
    std::memcpy(img.data() + kOff, png.data(), png.size());
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

    writeInode(12, 0x4000, bs, 3, 2); // directory at block 3 (inode table occupies 5-6)
    writeInode(13, 0x8000, 8, 7); // file at block 7

    // Directory block 3: dirent for note.txt -> inode 13
    size_t dir = 3 * bs;
    writeLe32(img, dir + 0x00, 13);
    writeLe16(img, dir + 0x04, 16); // rec_len (8 header + 8 name)
    img[dir + 0x06] = 8;  // name_len
    img[dir + 0x07] = 1;  // file type REG
    std::memcpy(img.data() + dir + 8, "note.txt", 8);

    std::memcpy(img.data() + 7 * bs, "ext4data", 8);
    return img;
}

// ext4 with block bitmap: blocks 0-7 allocated, block 8+ free (for unallocated carve tests).
inline std::vector<uint8_t> buildExt4CarveVolume() {
    auto img = buildExt4Volume();
    constexpr uint32_t bs = 1024;

    TestExt4Sb sb{};
    std::memcpy(&sb, img.data() + bs, sizeof(sb));
    sb.s_blocks_per_group = 64;
    std::memcpy(img.data() + bs, &sb, sizeof(sb));

    TestExt4Gd gd{};
    gd.bg_block_bitmap_lo = 4;
    gd.bg_inode_bitmap_lo = 3;
    std::memcpy(img.data() + 2 * bs, &gd, sizeof(gd));

    const size_t bmp = 4 * bs;
    img[bmp] = 0xFF; // blocks 0-7 allocated; blocks 8+ clear in following bytes
    return img;
}

// ext4 + jbd2 journal holding a dirent the live directory no longer has.
inline std::vector<uint8_t> buildExt4JournalVolume() {
    auto img = buildExt4Volume();
    constexpr uint32_t bs = 1024;
    writeLe32(img, bs + 0xE0, 8); // s_journal_inum

    auto writeInode = [&](uint32_t ino, uint16_t mode, uint32_t size, uint32_t b0, uint32_t b1 = 0, uint32_t b2 = 0) {
        size_t off = 5 * bs + (ino - 1) * 128;
        writeLe16(img, off + 0x00, mode);
        writeLe16(img, off + 0x18, 1);
        writeLe32(img, off + 0x04, size);
        writeLe32(img, off + 0x28, b0);
        writeLe32(img, off + 0x2C, b1);
        writeLe32(img, off + 0x30, b2);
    };
    writeInode(8, 0x8000, 3 * bs, 8, 9, 10);

    auto writeBe32 = [&](size_t off, uint32_t v) {
        img[off] = static_cast<uint8_t>(v >> 24);
        img[off + 1] = static_cast<uint8_t>(v >> 16);
        img[off + 2] = static_cast<uint8_t>(v >> 8);
        img[off + 3] = static_cast<uint8_t>(v);
    };
    constexpr uint32_t kJbd2Magic = 0xC03B3998;
    writeBe32(8 * bs, kJbd2Magic);
    writeBe32(8 * bs + 4, 4); // superblock v2
    writeBe32(9 * bs, kJbd2Magic);
    writeBe32(9 * bs + 4, 1); // descriptor
    size_t de = 10 * bs;
    writeLe32(img, de + 0x00, 99);
    writeLe16(img, de + 0x04, 16);
    img[de + 0x06] = 8;
    img[de + 0x07] = 1;
    std::memcpy(img.data() + de + 8, "gone.txt", 8);
    return img;
}

// Committed jbd2 txn: dirent + inode + data. Uncommitted txn: phantom dirent, no commit.
inline std::vector<uint8_t> buildExt4JournalReplayVolume() {
    auto img = buildExt4Volume();
    constexpr uint32_t bs = 1024;
    writeLe32(img, bs + 0xE0, 8); // s_journal_inum

    uint32_t jblocks[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    {
        size_t off = 5 * bs + 7 * 128; // inode 8
        writeLe16(img, off + 0x00, 0x8000);
        writeLe16(img, off + 0x1A, 1); // i_links_count
        writeLe32(img, off + 0x04, 8 * bs);
        for (int i = 0; i < 8; ++i) writeLe32(img, off + 0x28 + i * 4, jblocks[i]);
    }

    auto writeBe32 = [&](size_t off, uint32_t v) {
        img[off] = static_cast<uint8_t>(v >> 24);
        img[off + 1] = static_cast<uint8_t>(v >> 16);
        img[off + 2] = static_cast<uint8_t>(v >> 8);
        img[off + 3] = static_cast<uint8_t>(v);
    };
    constexpr uint32_t kJbd2Magic = 0xC03B3998;
    constexpr uint32_t kSameUuid = 2;
    constexpr uint32_t kLastTag = 8;

    // journal superblock @ FS block 8 (journal block 0)
    writeBe32(8 * bs, kJbd2Magic);
    writeBe32(8 * bs + 4, 4);
    writeBe32(8 * bs + 8, 1);
    writeBe32(8 * bs + 12, bs);
    writeBe32(8 * bs + 16, 8);
    writeBe32(8 * bs + 20, 1); // s_first
    writeBe32(8 * bs + 24, 1); // s_sequence
    writeBe32(8 * bs + 28, 1); // s_start

    // descriptor seq 1 @ journal block 1 (FS 9): tags FS 30, 6, 32
    writeBe32(9 * bs, kJbd2Magic);
    writeBe32(9 * bs + 4, 1);
    writeBe32(9 * bs + 8, 1);
    writeBe32(9 * bs + 12, 30);
    writeBe32(9 * bs + 16, kSameUuid);
    writeBe32(9 * bs + 20, 6);
    writeBe32(9 * bs + 24, kSameUuid);
    writeBe32(9 * bs + 28, 32);
    writeBe32(9 * bs + 32, kSameUuid | kLastTag);

    // data 0: dirent gone.dat -> inode 14 @ FS 30 copy (FS 10)
    size_t de = 10 * bs;
    writeLe32(img, de + 0x00, 14);
    writeLe16(img, de + 0x04, 20);
    img[de + 0x06] = 8;
    img[de + 0x07] = 1;
    std::memcpy(img.data() + de + 8, "gone.dat", 8);

    // data 1: inode table block 6 copy (inodes 9-16); inode 14 at offset 640
    size_t it = 11 * bs;
    size_t i14 = it + (14 - 9) * 128;
    writeLe16(img, i14 + 0x00, 0x8000);
    writeLe32(img, i14 + 0x04, 8);     // size
    writeLe32(img, i14 + 0x14, 1);     // i_dtime
    writeLe16(img, i14 + 0x1A, 0);     // links=0 deleted
    writeLe32(img, i14 + 0x28, 32);    // i_block[0] = FS 32

    // data 2: file payload @ FS 32 copy (FS 12)
    std::memcpy(img.data() + 12 * bs, "JBD2DATA", 8);

    // commit seq 1 @ FS 13
    writeBe32(13 * bs, kJbd2Magic);
    writeBe32(13 * bs + 4, 2);
    writeBe32(13 * bs + 8, 1);

    // uncommitted descriptor seq 2 + phantom dirent, no commit
    writeBe32(14 * bs, kJbd2Magic);
    writeBe32(14 * bs + 4, 1);
    writeBe32(14 * bs + 8, 2);
    writeBe32(14 * bs + 12, 40);
    writeBe32(14 * bs + 16, kSameUuid | kLastTag);
    writeLe32(img, 15 * bs + 0x00, 50);
    writeLe16(img, 15 * bs + 0x04, 20);
    img[15 * bs + 0x06] = 11;
    img[15 * bs + 0x07] = 1;
    std::memcpy(img.data() + 15 * bs + 8, "phantom.bin", 11);
    return img;
}

inline void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) img[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

// NTFS superfloppy: boot $MFT walk, deleted resident doc.txt ("hello").
inline std::vector<uint8_t> buildNtfsDeletedResidentVolume() {
    constexpr uint32_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, ss);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 1024);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x00); // deleted (not in use)
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

// NTFS: deleted MFT record with non-resident $DATA at cluster 5.
inline std::vector<uint8_t> buildNtfsDeletedNonResidentVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t spc = 8;
    const uint32_t dataCluster = 5;
    const uint32_t dataSector = dataCluster * spc;
    const char* payload = "hello nonres";
    const uint32_t payloadLen = 12;

    std::vector<uint8_t> img(ss * 128, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, ss);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, static_cast<uint64_t>(spc) * ss);
    writeLe64(img, attr + 0x30, 1024);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x00);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, payloadLen);
    writeLe64(img, attr + 56, payloadLen);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe64(img, attr + 16, 0);
    writeLe64(img, attr + 24, 0);
    writeLe16(img, attr + 32, 0x40);
    writeLe64(img, attr + 40, static_cast<uint64_t>(spc) * ss);
    writeLe64(img, attr + 48, payloadLen);
    writeLe64(img, attr + 56, payloadLen);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = static_cast<uint8_t>(dataCluster);
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    std::memcpy(img.data() + static_cast<size_t>(dataSector) * ss, payload, payloadLen);
    return img;
}

// exFAT: clusters 2-3 allocated, cluster 4+ free for unallocated carve.
inline std::vector<uint8_t> buildExFatUnallocatedVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t fatOff = 24;
    constexpr uint32_t heapOff = 25;
    constexpr uint32_t totalSectors = 64;
    std::vector<uint8_t> img(totalSectors * ss, 0);

    std::memcpy(img.data() + 3, "EXFAT   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;
    img[108] = 9;
    img[109] = 0;
    img[110] = 1;
    writeLe32(img, 80, fatOff);
    writeLe32(img, 84, 1);
    writeLe32(img, 88, heapOff);
    writeLe32(img, 92, 8);
    writeLe32(img, 96, 2);

    writeLe32(img, fatOff * ss + 8, 0xFFFFFFFF);
    writeLe32(img, fatOff * ss + 12, 0xFFFFFFFF);

    const uint16_t chk = 0xBEEF;
    size_t de = heapOff * ss;
    img[de] = 0x85;
    img[de + 1] = 2;
    writeLe16(img, de + 2, chk);
    de += 32;
    img[de] = 0xC0; // File Stream Extension entry (spec type 0xC0)
    img[de + 3] = 8; // FileNameLength (spec offset 3)
    writeLe32(img, de + 20, 3);
    writeLe64(img, de + 24, 17); // DataLength (spec offset 24, LE64)
    de += 32;
    img[de] = 0xC1;
    static const uint8_t lostName[] = {'L',0,'O',0,'S',0,'T',0,'.',0,'D',0,'A',0,'T',0};
    std::memcpy(img.data() + de + 2, lostName, sizeof(lostName));
    std::memcpy(img.data() + (heapOff + 1) * ss, "recovered!", 10);
    return img;
}

// Two named files: first $DATA at a high cluster, second resident (MFT sector).
// Using file startSector as scan progress rewinds the bar on this volume.
inline std::vector<uint8_t> buildNtfsJumpingDataRunsVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t spc = 8;
    const uint32_t dataCluster = 20;
    const uint32_t dataSector = dataCluster * spc;
    const char* payload = "high";
    const uint32_t payloadLen = 4;

    std::vector<uint8_t> img(ss * 256, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, ss);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, static_cast<uint64_t>(spc) * ss);
    writeLe64(img, attr + 0x30, 3072);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "high.bin";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, payloadLen);
    writeLe64(img, attr + 56, payloadLen);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe64(img, attr + 16, 0);
    writeLe64(img, attr + 24, 0);
    writeLe16(img, attr + 32, 0x40);
    writeLe64(img, attr + 40, static_cast<uint64_t>(spc) * ss);
    writeLe64(img, attr + 48, payloadLen);
    writeLe64(img, attr + 56, payloadLen);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = static_cast<uint8_t>(dataCluster);
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec2 = rec0 + 2048;
    std::memcpy(img.data() + rec2, "FILE", 4);
    writeLe16(img, rec2 + 0x14, 0x38);
    writeLe16(img, rec2 + 0x16, 0x01);
    writeLe32(img, rec2 + 0x18, 256);
    writeLe32(img, rec2 + 0x1C, 1024);
    attr = rec2 + 0x38;
    const char* lowName = "low.txt";
    const size_t lowLen = 7;
    const size_t lowFn = 66 + lowLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + lowFn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(lowFn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(lowLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < lowLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(lowName[i]));
    attr += 16 + 8 + lowFn;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    std::memcpy(img.data() + static_cast<size_t>(dataSector) * ss, payload, payloadLen);
    return img;
}

// Directory $INDEX_ROOT slack names gone.txt at reused MFT 2 (alive.bin).
inline std::vector<uint8_t> buildNtfsIndexRootReuseVolume() {
    constexpr uint32_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, ss);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 3072);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x03);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* dirName = "Dir";
    const size_t dirLen = 3;
    const size_t dirFn = 66 + dirLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + dirFn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(dirFn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(dirLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < dirLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(dirName[i]));
    attr += 16 + 8 + dirFn;

    std::vector<uint8_t> root(32, 0);
    root[0] = 0x30;
    root[16] = 16;
    const size_t lastAt = root.size();
    root.resize(lastAt + 16, 0);
    root[lastAt + 8] = 16;
    root[lastAt + 12] = 0x02;
    const uint32_t usedEnd = static_cast<uint32_t>(root.size() - 16);
    const char* slackName = "gone.txt";
    const size_t slackLen = 8;
    const uint16_t keyLen = static_cast<uint16_t>(66 + slackLen * 2);
    const uint16_t entryLen = static_cast<uint16_t>(16 + keyLen);
    const size_t slackAt = root.size();
    root.resize(slackAt + entryLen, 0);
    root[slackAt] = 2;
    root[slackAt + 8] = static_cast<uint8_t>(entryLen & 0xFF);
    root[slackAt + 9] = static_cast<uint8_t>((entryLen >> 8) & 0xFF);
    root[slackAt + 10] = static_cast<uint8_t>(keyLen & 0xFF);
    root[slackAt + 11] = static_cast<uint8_t>((keyLen >> 8) & 0xFF);
    root[slackAt + 16] = 5;
    root[slackAt + 16 + 64] = static_cast<uint8_t>(slackLen);
    root[slackAt + 16 + 65] = 1;
    for (size_t i = 0; i < slackLen; ++i)
        root[slackAt + 16 + 66 + i * 2] = static_cast<uint8_t>(slackName[i]);
    const uint32_t allocRel = static_cast<uint32_t>(root.size() - 16);
    for (int i = 0; i < 4; ++i) {
        root[20 + i] = static_cast<uint8_t>((usedEnd >> (8 * i)) & 0xFF);
        root[24 + i] = static_cast<uint8_t>((allocRel >> (8 * i)) & 0xFF);
    }
    uint32_t valueLen = static_cast<uint32_t>(root.size());
    uint32_t attrLen = 24 + valueLen;
    while (attrLen % 8) ++attrLen;
    writeLe32(img, attr + 0, 0x90);
    writeLe32(img, attr + 4, attrLen);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, valueLen);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, root.data(), root.size());
    writeLe32(img, attr + attrLen, 0xFFFFFFFF);

    const size_t rec2 = rec0 + 2048;
    std::memcpy(img.data() + rec2, "FILE", 4);
    writeLe16(img, rec2 + 0x14, 0x38);
    writeLe16(img, rec2 + 0x16, 0x01);
    writeLe32(img, rec2 + 0x18, 256);
    writeLe32(img, rec2 + 0x1C, 1024);
    attr = rec2 + 0x38;
    const char* liveName = "alive.bin";
    const size_t liveLen = 9;
    const size_t liveFn = 66 + liveLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + liveFn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(liveFn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 4);
    writeLe64(img, attr + 56, 4);
    img[attr + 24 + 64] = static_cast<uint8_t>(liveLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < liveLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(liveName[i]));
    attr += 16 + 8 + liveFn;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 4);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "live", 4);
    attr += 32;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

// Directory $INDEX_ALLOCATION ($I30) INDX names indx_only.txt (MFT 99, no FILE).
inline std::vector<uint8_t> buildNtfsIndexAllocationVolume() {
    constexpr uint32_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, ss);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 3072);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x03);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* dirName = "Dir";
    const size_t dirLen = 3;
    const size_t dirFn = 66 + dirLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + dirFn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(dirFn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(dirLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < dirLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(dirName[i]));
    attr += 16 + 8 + dirFn;

    writeLe32(img, attr + 0, 0x90);
    writeLe32(img, attr + 4, 80);
    img[attr + 8] = 0;
    img[attr + 9] = 4;
    writeLe16(img, attr + 10, 24);
    writeLe32(img, attr + 16, 48);
    writeLe16(img, attr + 20, 32);
    writeLe16(img, attr + 24, '$');
    writeLe16(img, attr + 26, 'I');
    writeLe16(img, attr + 28, '3');
    writeLe16(img, attr + 30, '0');
    writeLe32(img, attr + 32, 0x30);
    writeLe32(img, attr + 36, 1);
    writeLe32(img, attr + 40, 4096);
    img[attr + 44] = 1;
    writeLe32(img, attr + 48, 16);
    writeLe32(img, attr + 52, 32);
    writeLe32(img, attr + 56, 32);
    img[attr + 64 + 8] = 16;
    img[attr + 64 + 12] = 0x02;
    attr += 80;

    writeLe32(img, attr + 0, 0xA0);
    writeLe32(img, attr + 4, 80);
    img[attr + 8] = 1;
    img[attr + 9] = 4;
    writeLe16(img, attr + 10, 0x40);
    writeLe16(img, attr + 0x20, 0x48);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    writeLe16(img, attr + 0x40, '$');
    writeLe16(img, attr + 0x42, 'I');
    writeLe16(img, attr + 0x44, '3');
    writeLe16(img, attr + 0x46, '0');
    img[attr + 0x48] = 0x11;
    img[attr + 0x49] = 0x01;
    img[attr + 0x4A] = 0x03;
    img[attr + 0x4B] = 0x00;
    writeLe32(img, attr + 80, 0xFFFFFFFF);

    const size_t indxOff = 3 * 4096;
    std::vector<uint8_t> indx(0x40, 0);
    std::memcpy(indx.data(), "INDX", 4);
    writeLe16(indx, 4, 0x28);
    writeLe32(indx, 0x18, 0x28);
    const char* n = "indx_only.txt";
    const size_t nlen = std::strlen(n);
    const uint16_t keyLen = static_cast<uint16_t>(66 + nlen * 2);
    const uint16_t entryLen = static_cast<uint16_t>(16 + keyLen);
    const size_t start = indx.size();
    indx.resize(start + entryLen, 0);
    indx[start] = 99;
    indx[start + 8] = static_cast<uint8_t>(entryLen & 0xFF);
    indx[start + 9] = static_cast<uint8_t>((entryLen >> 8) & 0xFF);
    indx[start + 10] = static_cast<uint8_t>(keyLen & 0xFF);
    indx[start + 11] = static_cast<uint8_t>((keyLen >> 8) & 0xFF);
    indx[start + 16] = 5;
    indx[start + 16 + 64] = static_cast<uint8_t>(nlen);
    indx[start + 16 + 65] = 1;
    for (size_t i = 0; i < nlen; ++i)
        indx[start + 16 + 66 + i * 2] = static_cast<uint8_t>(n[i]);
    const size_t lastAt = indx.size();
    indx.resize(lastAt + 16, 0);
    indx[lastAt + 8] = 16;
    indx[lastAt + 12] = 0x02;
    const uint32_t usedRel = static_cast<uint32_t>(indx.size() - 0x18);
    writeLe32(indx, 0x1C, usedRel);
    writeLe32(indx, 0x20, usedRel);
    indx.resize(4096, 0);
    std::memcpy(img.data() + indxOff, indx.data(), indx.size());
    return img;
}

// SOI + DQT + SOS + payload + EOI. payloadLen must push size past one 512-byte
// FAT cluster so a first-cluster-only undelete fails MD5.
inline std::vector<uint8_t> minimalValidJpeg(uint8_t tag, size_t payloadLen = 580) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x03, 0x00,
                                 0xFF, 0xDA, 0x00, 0x02};
    jpeg.insert(jpeg.end(), payloadLen, tag);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

// Deleted FAT16 JPEG split across cluster 2 and 10. Cluster 3 holds a second
// deleted JPEG (poison). FAT1 file entries stay 0. When writeBackupChain,
// FAT2 keeps 2→10 EOC so a mirror walk reconstructs the original.
inline std::vector<uint8_t> buildFat16DeletedFragmentedJpegVolume(bool writeBackupChain) {
    constexpr uint32_t ss = 512;
    constexpr uint32_t fatSectors = 16;
    constexpr uint32_t totalSectors = 4120;
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

    const uint32_t fat1 = bpb.reservedSectors;
    const uint32_t fat2 = fat1 + fatSectors;
    const uint32_t rootStart = fat1 + bpb.numFats * fatSectors;
    const uint32_t dataStart = rootStart + 1;

    auto writeFat16 = [&](uint32_t fatBase, uint32_t clus, uint16_t val) {
        writeLe16(img, static_cast<size_t>(fatBase) * ss + static_cast<size_t>(clus) * 2, val);
    };
    writeFat16(fat1, 0, 0xFFF8);
    writeFat16(fat1, 1, 0xFFFF);
    writeFat16(fat2, 0, 0xFFF8);
    writeFat16(fat2, 1, 0xFFFF);

    const auto jpegA = minimalValidJpeg(0x11);
    const auto jpegB = minimalValidJpeg(0x22);

    size_t de = rootStart * ss;
    std::memcpy(img.data() + de, "PHOTO1  JPG", 11);
    img[de] = 0xE5;
    img[de + 11] = 0x20;
    writeLe16(img, de + 26, 2);
    writeLe32(img, de + 28, static_cast<uint32_t>(jpegA.size()));

    de += 32;
    std::memcpy(img.data() + de, "PHOTO2  JPG", 11);
    img[de] = 0xE5;
    img[de + 11] = 0x20;
    writeLe16(img, de + 26, 3);
    writeLe32(img, de + 28, static_cast<uint32_t>(jpegB.size()));

    const size_t clus2 = static_cast<size_t>(dataStart) * ss;
    const size_t clus3 = static_cast<size_t>(dataStart + 1) * ss;
    const size_t clus10 = static_cast<size_t>(dataStart + 8) * ss;
    std::memcpy(img.data() + clus2, jpegA.data(), ss);
    std::memcpy(img.data() + clus10, jpegA.data() + ss, jpegA.size() - ss);
    std::memcpy(img.data() + clus3, jpegB.data(), jpegB.size());

    if (writeBackupChain) {
        writeFat16(fat2, 2, 10);
        writeFat16(fat2, 10, 0xFFFF);
        writeFat16(fat2, 3, 4);
        writeFat16(fat2, 4, 0xFFFF);
    }
    return img;
}

inline std::vector<uint8_t> buildIndxNamed(const char* n, uint8_t childMftLo, uint64_t realSize = 0) {
    std::vector<uint8_t> indx(0x40, 0);
    std::memcpy(indx.data(), "INDX", 4);
    writeLe16(indx, 4, 0x28);
    writeLe32(indx, 0x18, 0x28);
    const size_t nlen = std::strlen(n);
    const uint16_t keyLen = static_cast<uint16_t>(66 + nlen * 2);
    const uint16_t entryLen = static_cast<uint16_t>(16 + keyLen);
    const size_t start = indx.size();
    indx.resize(start + entryLen, 0);
    indx[start] = childMftLo;
    indx[start + 8] = static_cast<uint8_t>(entryLen & 0xFF);
    indx[start + 9] = static_cast<uint8_t>((entryLen >> 8) & 0xFF);
    indx[start + 10] = static_cast<uint8_t>(keyLen & 0xFF);
    indx[start + 11] = static_cast<uint8_t>((keyLen >> 8) & 0xFF);
    indx[start + 16] = 5;
    if (realSize > 0) {
        writeLe64(indx, start + 16 + 40, (realSize + 4095ull) & ~4095ull);
        writeLe64(indx, start + 16 + 48, realSize);
    }
    indx[start + 16 + 64] = static_cast<uint8_t>(nlen);
    indx[start + 16 + 65] = 1;
    for (size_t i = 0; i < nlen; ++i)
        indx[start + 16 + 66 + i * 2] = static_cast<uint8_t>(n[i]);
    const size_t lastAt = indx.size();
    indx.resize(lastAt + 16, 0);
    indx[lastAt + 8] = 16;
    indx[lastAt + 12] = 0x02;
    const uint32_t usedRel = static_cast<uint32_t>(indx.size() - 0x18);
    writeLe32(indx, 0x1C, usedRel);
    writeLe32(indx, 0x20, usedRel);
    indx.resize(4096, 0);
    return indx;
}

// FAT32 superfloppy: 5 deleted JPEGs, FAT clusters freed, payload still on disk.
// countOfClusters >= 65525 so parseFAT takes the FAT32 directory walk.
inline std::vector<uint8_t> buildFat32DeletedJpegVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t reserved = 32;
    constexpr uint32_t fatSectors = 512;
    constexpr uint32_t dataClusters = 65525;
    const uint32_t totalSectors = reserved + 2 * fatSectors + dataClusters;
    std::vector<uint8_t> img(static_cast<size_t>(totalSectors) * ss, 0);

    img[0] = 0xEB;
    img[1] = 0x58;
    img[2] = 0x90;
    std::memcpy(img.data() + 3, "MSWIN4.1", 8);
    writeLe16(img, 11, static_cast<uint16_t>(ss));
    img[13] = 1;
    writeLe16(img, 14, static_cast<uint16_t>(reserved));
    img[16] = 2;
    img[21] = 0xF8;
    writeLe32(img, 32, totalSectors);
    writeLe32(img, 36, fatSectors);
    writeLe32(img, 44, 2);
    writeLe16(img, 48, 1);
    writeLe16(img, 50, 6);
    img[64] = 0x80;
    img[66] = 0x29;
    std::memcpy(img.data() + 82, "FAT32   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;

    const uint32_t fatStart = reserved;
    auto writeFat32 = [&](uint32_t fatBase, uint32_t clus, uint32_t val) {
        writeLe32(img, static_cast<size_t>(fatBase) * ss + static_cast<size_t>(clus) * 4, val);
    };
    writeFat32(fatStart, 0, 0x0FFFFFF8);
    writeFat32(fatStart, 1, 0x0FFFFFFF);
    writeFat32(fatStart, 2, 0x0FFFFFF8);
    writeFat32(fatStart + fatSectors, 0, 0x0FFFFFF8);
    writeFat32(fatStart + fatSectors, 1, 0x0FFFFFFF);
    writeFat32(fatStart + fatSectors, 2, 0x0FFFFFF8);

    const uint32_t dataStart = reserved + 2 * fatSectors;
    const size_t rootOff = static_cast<size_t>(dataStart) * ss;
    const uint8_t tags[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    for (int i = 0; i < 5; ++i) {
        auto jpeg = minimalValidJpeg(tags[i]);
        const uint32_t firstClus = static_cast<uint32_t>(3 + i * 2);
        size_t de = rootOff + static_cast<size_t>(i) * 32;
        std::memcpy(img.data() + de, "PHOTO1  JPG", 11);
        img[de + 5] = static_cast<uint8_t>('1' + i);
        img[de] = 0xE5;
        img[de + 11] = 0x20;
        writeLe16(img, de + 26, static_cast<uint16_t>(firstClus));
        writeLe32(img, de + 28, static_cast<uint32_t>(jpeg.size()));
        const size_t dataOff = static_cast<size_t>(dataStart + (firstClus - 2)) * ss;
        std::memcpy(img.data() + dataOff, jpeg.data(), jpeg.size());
    }
    return img;
}

// exFAT: in-use dir "pics" / deleted "shot.jpg". FAT cluster for the file freed.
inline std::vector<uint8_t> buildExFatDeletedPreservePathsVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t fatOff = 24;
    constexpr uint32_t heapOff = 25;
    constexpr uint32_t totalSectors = 64;
    std::vector<uint8_t> img(totalSectors * ss, 0);

    std::memcpy(img.data() + 3, "EXFAT   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;
    img[108] = 9;
    img[109] = 0;
    img[110] = 1;
    writeLe32(img, 80, fatOff);
    writeLe32(img, 84, 1);
    writeLe32(img, 88, heapOff);
    writeLe32(img, 92, 8);
    writeLe32(img, 96, 2);

    writeLe32(img, fatOff * ss + 8, 0xFFFFFFFF);  // cluster 2 root EOC
    writeLe32(img, fatOff * ss + 12, 0xFFFFFFFF); // cluster 3 pics EOC
    writeLe32(img, fatOff * ss + 16, 0);          // cluster 4 file freed

    const uint16_t dirChk = 0xBEEF;
    size_t de = heapOff * ss;
    img[de] = 0x85;
    img[de + 1] = 2;
    writeLe16(img, de + 2, dirChk);
    writeLe16(img, de + 4, 0x10);
    de += 32;
    img[de] = 0xC0;
    img[de + 3] = 4;
    writeLe32(img, de + 20, 3);
    de += 32;
    img[de] = 0xC1;
    static const uint8_t picsName[] = {'p', 0, 'i', 0, 'c', 0, 's', 0};
    std::memcpy(img.data() + de + 2, picsName, sizeof(picsName));

    auto jpeg = minimalValidJpeg(0xAB, 20);
    const uint16_t fileChk = 0xAABB;
    de = (heapOff + 1) * ss;
    img[de] = 0x05;
    img[de + 1] = 2;
    writeLe16(img, de + 2, fileChk);
    de += 32;
    img[de] = 0x40;
    img[de + 3] = 8;
    writeLe32(img, de + 20, 4);
    writeLe64(img, de + 24, jpeg.size());
    de += 32;
    img[de] = 0x41;
    static const uint8_t shotName[] = {'s', 0, 'h', 0, 'o', 0, 't', 0, '.', 0, 'j', 0, 'p', 0, 'g', 0};
    std::memcpy(img.data() + de + 2, shotName, sizeof(shotName));
    std::memcpy(img.data() + (heapOff + 2) * ss, jpeg.data(), jpeg.size());
    return img;
}

// Index-allocation volume plus a valid INDX in free cluster 10 (not in $I30 runs).
inline std::vector<uint8_t> buildNtfsUnallocIndxVolume() {
    auto img = buildNtfsIndexAllocationVolume();
    constexpr uint32_t ss = 512;
    img.resize(ss * 128, 0);
    auto indx = buildIndxNamed("unalloc_only.txt", 88);
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    return img;
}

// Unalloc INDX names gone.jpg (MFT 88 missing). JPEG sits in free cluster 11,
// or only inside live MFT $DATA at LCN 12 when jpegOnLiveMftData.
inline std::vector<uint8_t> buildNtfsUnallocIndxJpegVolume(bool jpegOnLiveMftData = false) {
    constexpr uint32_t ss = 512;
    auto img = buildNtfsIndexAllocationVolume();
    img.resize(ss * 128, 0);
    const auto jpeg = minimalValidJpeg(0xAB);
    auto indx = buildIndxNamed("gone.jpg", 88, jpeg.size());
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    if (jpegOnLiveMftData) {
        const size_t rec2 = 8 * ss + 2048;
        std::memcpy(img.data() + rec2, "FILE", 4);
        writeLe16(img, rec2 + 0x14, 0x38);
        writeLe16(img, rec2 + 0x16, 0x01);
        writeLe32(img, rec2 + 0x18, 256);
        writeLe32(img, rec2 + 0x1C, 1024);
        const size_t attr = rec2 + 0x38;
        writeLe32(img, attr + 0, 0x80);
        writeLe32(img, attr + 4, 72);
        img[attr + 8] = 1;
        writeLe16(img, attr + 0x20, 0x40);
        writeLe64(img, attr + 0x28, 4096);
        writeLe64(img, attr + 0x30, jpeg.size());
        img[attr + 0x40] = 0x11;
        img[attr + 0x41] = 0x01;
        img[attr + 0x42] = 0x0C;
        writeLe32(img, attr + 72, 0xFFFFFFFF);
        std::memcpy(img.data() + 12 * 4096, jpeg.data(), jpeg.size());
    } else {
        std::memcpy(img.data() + 11 * 4096, jpeg.data(), jpeg.size());
    }
    return img;
}

inline std::vector<uint8_t> buildNtfsUnallocIndxPngVolume() {
    constexpr uint32_t ss = 512;
    auto img = buildNtfsIndexAllocationVolume();
    img.resize(ss * 128, 0);
    const auto png = buildMinimalValidPng();
    auto indx = buildIndxNamed("gone.png", 88, png.size());
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    std::memcpy(img.data() + 11 * 4096, png.data(), png.size());
    return img;
}

inline std::vector<uint8_t> buildMinimalPdf() {
    const char* s = "%PDF-1.4\n1 0 obj\n<<>>\nendobj\n%%EOF";
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

inline std::vector<uint8_t> buildNtfsUnallocIndxPdfVolume() {
    constexpr uint32_t ss = 512;
    auto img = buildNtfsIndexAllocationVolume();
    img.resize(ss * 128, 0);
    const auto pdf = buildMinimalPdf();
    auto indx = buildIndxNamed("gone.pdf", 88, pdf.size());
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    std::memcpy(img.data() + 11 * 4096, pdf.data(), pdf.size());
    return img;
}

inline std::vector<uint8_t> buildMinimalZip() {
    std::vector<uint8_t> b;
    const uint8_t local[] = {'P', 'K', 0x03, 0x04};
    b.insert(b.end(), local, local + 4);
    b.insert(b.end(), 26, 0);
    const uint8_t central[] = {'P', 'K', 0x01, 0x02};
    b.insert(b.end(), central, central + 4);
    const uint8_t eocd[] = {'P', 'K', 0x05, 0x06};
    b.insert(b.end(), eocd, eocd + 4);
    auto le16 = [&](uint16_t x) {
        b.push_back(static_cast<uint8_t>(x & 0xFF));
        b.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    };
    auto le32 = [&](uint32_t x) {
        le16(static_cast<uint16_t>(x & 0xFFFF));
        le16(static_cast<uint16_t>(x >> 16));
    };
    le16(0);
    le16(0);
    le16(1);
    le16(1);
    le32(30);
    le32(30);
    le16(0);
    return b;
}

inline std::vector<uint8_t> buildNtfsUnallocIndxZipVolume() {
    constexpr uint32_t ss = 512;
    auto img = buildNtfsIndexAllocationVolume();
    img.resize(ss * 128, 0);
    const auto zip = buildMinimalZip();
    auto indx = buildIndxNamed("gone.zip", 88, zip.size());
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    std::memcpy(img.data() + 11 * 4096, zip.data(), zip.size());
    return img;
}

} // namespace byteback::testfix
