#include "fs/apfs_container.h"
#include "fs/hfs_catalog.h"
#include "byteback_fs.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

using namespace byteback;

namespace {

void writeBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void writeBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void writeBe64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

void writeHfsLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void writeHfsLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void writeHfsLe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

void writeFork(uint8_t* dst, uint32_t startBlock, uint32_t blockCount, uint64_t logicalSize = 0) {
    writeBe64(dst, logicalSize);
    writeBe32(dst + 16, startBlock);
    writeBe32(dst + 20, blockCount);
}

size_t blockOffset(uint32_t blockIndex, uint32_t blockSize) {
    return static_cast<size_t>(blockIndex) * blockSize;
}

void writeHfsVolumeHeader(std::vector<uint8_t>& img, uint32_t catalogBlock, uint32_t extentsBlock,
                          uint32_t blockSize = 4096, uint32_t journalInfoBlock = 0) {
    const size_t hdrOff = 1024;
    if (img.size() < hdrOff + 512) img.resize(hdrOff + 512, 0);
    img[hdrOff] = 0x48;
    img[hdrOff + 1] = 0x2B;
    writeBe32(img.data() + hdrOff + 40, blockSize);
    writeFork(img.data() + hdrOff + 192, extentsBlock, 1);
    writeFork(img.data() + hdrOff + 272, catalogBlock, 1);
    if (journalInfoBlock != 0) {
        writeBe32(img.data() + hdrOff + 4, 0x00002000); // kHFSVolumeJournaledMask
        writeBe32(img.data() + hdrOff + 12, journalInfoBlock);
    }
}

// BTNodeDescriptor is 14 bytes: forwardLink(4) backLink(4) kind(1) height(1)
// numRecords(2) reserved(2). Leaf records start after it, at offset 14.
void writeNodeHeader(uint8_t* node, uint16_t numRecords) {
    node[8] = 0xFF; // kind = leaf
    node[10] = static_cast<uint8_t>(numRecords >> 8);
    node[11] = static_cast<uint8_t>(numRecords);
}

// HFSPlusCatalogFile: fileID u32 at value+12, dataFork (HFSPlusForkData, 80
// bytes) at value+92; fork extents are 8-byte descriptors.
const uint16_t kCatalogFileValueLen = 92 + 80;

// XNU journal.c style rolling checksum; checksum field must be 0 while hashing.
uint32_t appleJournalCksum(const uint8_t* p, int n) {
    uint32_t cksum = 0;
    for (int i = 0; i < n; ++i) {
        cksum = (cksum << 8) ^ (cksum + p[i]);
    }
    return ~cksum;
}

void writeCatalogLeafNamed(uint8_t* node, uint32_t blockSize, const uint8_t* nameUtf16,
                           uint16_t nameBytes, uint32_t fileId, uint64_t logicalSize,
                           uint32_t startBlock) {
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, fileId);
    writeBe64(val + 92, logicalSize);
    writeBe32(val + 92 + 16, startBlock);
    writeBe32(val + 92 + 20, 1);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + kCatalogFileValueLen);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

// JournalInfoBlock in-FS + one committed txn whose data block is a catalog leaf
// for gone.dat (not present in the live B-tree).
void writeHfsJournalCatalogLeaf(std::vector<uint8_t>& img, uint32_t jibBlock, uint32_t journalBlock,
                                uint32_t blockSize, uint32_t fileId, const uint8_t* nameUtf16,
                                uint16_t nameBytes, uint32_t startBlock, bool emptyTxn = false,
                                bool littleEndian = false) {
    const uint64_t jOff = static_cast<uint64_t>(journalBlock) * blockSize;
    const uint32_t jhdr = 512;
    const uint32_t blhdr = 512;
    const uint64_t jSize = 8192;
    const size_t jibOff = blockOffset(jibBlock, blockSize);
    if (img.size() < jibOff + 512) img.resize(jibOff + 512, 0);
    writeBe32(img.data() + jibOff, 1); // kJIJournalInFSMask
    writeBe64(img.data() + jibOff + 36, jOff);
    writeBe64(img.data() + jibOff + 44, jSize);

    auto w16 = [&](uint8_t* p, uint16_t v) {
        if (littleEndian) writeHfsLe16(p, v);
        else writeBe16(p, v);
    };
    auto w32 = [&](uint8_t* p, uint32_t v) {
        if (littleEndian) writeHfsLe32(p, v);
        else writeBe32(p, v);
    };
    auto w64 = [&](uint8_t* p, uint64_t v) {
        if (littleEndian) writeHfsLe64(p, v);
        else writeBe64(p, v);
    };

    if (img.size() < static_cast<size_t>(jOff + jSize)) img.resize(static_cast<size_t>(jOff + jSize), 0);
    uint8_t* j = img.data() + jOff;
    w32(j + 0, 0x4A4E4C78);
    w32(j + 4, 0x12345678);
    w64(j + 8, jhdr);
    w64(j + 16, emptyTxn ? jhdr : jhdr + blhdr + blockSize);
    w64(j + 24, jSize);
    w32(j + 32, blhdr);
    w32(j + 36, 0);
    w32(j + 40, jhdr);
    w32(j + 36, appleJournalCksum(j, static_cast<int>(jhdr)));
    if (emptyTxn) return;

    uint8_t* bl = j + jhdr;
    w16(bl + 0, 2);
    w16(bl + 2, 2);
    w32(bl + 4, blhdr + blockSize);
    w32(bl + 8, 0);
    w32(bl + 12, 0);
    w64(bl + 16, 0);
    w32(bl + 24, blhdr);
    w32(bl + 28, 0);
    w64(bl + 32, 99);
    w32(bl + 40, blockSize);
    w32(bl + 44, 0);
    w32(bl + 8, appleJournalCksum(bl, static_cast<int>(blhdr)));
    writeCatalogLeafNamed(j + jhdr + blhdr, blockSize, nameUtf16, nameBytes, fileId,
                          blockSize, startBlock);
}

void writeHfsJournalWrappedCatalogLeaf(std::vector<uint8_t>& img, uint32_t jibBlock, uint32_t journalBlock,
                                       uint32_t blockSize, uint32_t fileId, const uint8_t* nameUtf16,
                                       uint16_t nameBytes, uint32_t startBlock) {
    const uint64_t jOff = static_cast<uint64_t>(journalBlock) * blockSize;
    const uint32_t jhdr = 512;
    const uint32_t blhdr = 512;
    const uint64_t jSize = 8192;
    const uint64_t start = jSize - blhdr;
    const uint64_t end = static_cast<uint64_t>(jhdr) + blockSize;
    const size_t jibOff = blockOffset(jibBlock, blockSize);
    if (img.size() < jibOff + 512) img.resize(jibOff + 512, 0);
    writeBe32(img.data() + jibOff, 1);
    writeBe64(img.data() + jibOff + 36, jOff);
    writeBe64(img.data() + jibOff + 44, jSize);
    if (img.size() < static_cast<size_t>(jOff + jSize)) img.resize(static_cast<size_t>(jOff + jSize), 0);
    uint8_t* j = img.data() + jOff;
    writeBe32(j + 0, 0x4A4E4C78);
    writeBe32(j + 4, 0x12345678);
    writeBe64(j + 8, start);
    writeBe64(j + 16, end);
    writeBe64(j + 24, jSize);
    writeBe32(j + 32, blhdr);
    writeBe32(j + 36, 0);
    writeBe32(j + 40, jhdr);
    writeBe32(j + 36, appleJournalCksum(j, static_cast<int>(jhdr)));
    uint8_t* bl = j + start;
    writeBe16(bl + 0, 2);
    writeBe16(bl + 2, 2);
    writeBe32(bl + 4, blhdr + blockSize);
    writeBe32(bl + 8, 0);
    writeBe32(bl + 12, 0);
    writeBe64(bl + 16, 0);
    writeBe32(bl + 24, blhdr);
    writeBe32(bl + 28, 0);
    writeBe64(bl + 32, 99);
    writeBe32(bl + 40, blockSize);
    writeBe32(bl + 44, 0);
    writeBe32(bl + 8, appleJournalCksum(bl, static_cast<int>(blhdr)));
    writeCatalogLeafNamed(j + jhdr, blockSize, nameUtf16, nameBytes, fileId, blockSize, startBlock);
}

void writeExtentOverflowLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t fileId,
                             uint32_t extraStart, uint32_t extraCount, uint32_t blockSize = 4096,
                             uint8_t forkType = 0) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    writeBe16(rec, 10);        // keyLength: forkType(1) pad(1) fileID(4) startBlock(4)
    rec[2] = forkType;
    rec[3] = 0;                // pad
    writeBe32(rec + 4, fileId);
    uint8_t* val = rec + 12;   // HFSPlusExtentRecord: 8 descriptors x 8 bytes
    writeBe32(val, extraStart);
    writeBe32(val + 4, extraCount);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 12 + 64);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

void writeCatalogFileLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t fileId,
                          uint64_t logicalSize, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t nameUtf16[] = {
        0x00, 'b', 0x00, 'i', 0x00, 'g', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, 'n',
    };
    const uint16_t nameBytes = sizeof(nameUtf16);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, fileId);
    writeBe64(val + 92, logicalSize);
    for (int i = 0; i < 8; ++i) {
        writeBe32(val + 92 + 16 + i * 8, 10 + static_cast<uint32_t>(i));
        writeBe32(val + 92 + 16 + i * 8 + 4, 1);
    }
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + kCatalogFileValueLen);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

void writeCatalogFileLeafWithResource(std::vector<uint8_t>& img, uint32_t blockIndex,
                                      uint32_t fileId, uint64_t dataSize, uint32_t dataStart,
                                      uint64_t rsrcSize, uint32_t rsrcStart,
                                      uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t nameUtf16[] = {
        0x00, 'b', 0x00, 'i', 0x00, 'g', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, 'n',
    };
    const uint16_t nameBytes = sizeof(nameUtf16);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, fileId);
    writeBe64(val + 92, dataSize);
    writeBe32(val + 92 + 16, dataStart);
    writeBe32(val + 92 + 20, 1);
    writeBe64(val + 172, rsrcSize);
    writeBe32(val + 172 + 16, rsrcStart);
    writeBe32(val + 172 + 20, 1);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + 92 + 80 + 80);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

// Live offset table lists only big.bin; gone.dat bytes remain in node free space
// (HFS+ unused catalog slot after delete+compact of the B-tree record count).
void writeCatalogLeafWithUnusedGone(std::vector<uint8_t>& img, uint32_t blockIndex,
                                    uint32_t blockSize = 4096) {
    writeCatalogFileLeaf(img, blockIndex, 100, 12 * blockSize, blockSize);
    const size_t off = blockOffset(blockIndex, blockSize);
    uint8_t* node = img.data() + off;
    const uint16_t liveEnd = readBe16(node + blockSize - 2);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    const uint16_t nameBytes = sizeof(goneName);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    uint8_t* rec = node + liveEnd;
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, goneName, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, 200);
    writeBe64(val + 92, blockSize);
    writeBe32(val + 92 + 16, 30);
    writeBe32(val + 92 + 20, 1);
}

void writeCatalogEmptyLeafWithUnusedGone(std::vector<uint8_t>& img, uint32_t blockIndex,
                                         uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 0);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    const uint16_t nameBytes = sizeof(goneName);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, goneName, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, 200);
    writeBe64(val + 92, blockSize);
    writeBe32(val + 92 + 16, 30);
    writeBe32(val + 92 + 20, 1);
    writeBe16(node + blockSize - 2, recStart);
}

void writeCatalogFileLeafNoExtents(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t fileId,
                                   uint64_t logicalSize, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t nameUtf16[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    const uint16_t nameBytes = sizeof(nameUtf16);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, fileId);
    writeBe64(val + 92, logicalSize);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + kCatalogFileValueLen);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

void writeCatalogRootFolderLeaf(std::vector<uint8_t>& img, uint32_t blockIndex,
                                uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t nameUtf16[] = {
        0x00, 'B', 0x00, 'Y', 0x00, 'T', 0x00, 'E', 0x00, 'B', 0x00, 'A', 0x00, 'C', 0x00, 'K',
    };
    const uint16_t nameBytes = sizeof(nameUtf16);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 1);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 4);
    writeBe32(val + 12, 2);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + 16);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

} // namespace

TEST(ApfsContainer, WalkFindsEmbeddedVolume) {
    std::vector<uint8_t> img(32 * 4096, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeBe64(img.data() + 40, 4096);
    writeBe64(img.data() + 48, 32);
    std::memcpy(img.data() + 4096 + 32, "APSB", 4);
    std::memcpy(img.data() + 4096 + 72, "Macintosh HD", 12);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool sawContainer = false;
    bool sawVolume = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_container") sawContainer = true;
        if (fr.source == "apfs_volume" && fr.name == "Macintosh HD") sawVolume = true;
    }, &running);

    EXPECT_TRUE(sawContainer);
    EXPECT_TRUE(sawVolume);
}

TEST(ApfsContainer, BackupNxsbUsedWhenPrimaryWiped) {
    std::vector<uint8_t> img(32 * 4096, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeBe64(img.data() + 40, 4096);
    writeBe64(img.data() + 48, 32);
    std::memcpy(img.data() + 4096 + 32, "APSB", 4);
    std::memcpy(img.data() + 4096 + 72, "Macintosh HD", 12);
    std::memcpy(img.data() + 31 * 4096, img.data(), 4096);
    std::memset(img.data() + 32, 0, 4);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawContainer = false;
    bool sawVolume = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_container") sawContainer = true;
        if (fr.source == "apfs_volume" && fr.name == "Macintosh HD") sawVolume = true;
    }, &running);
    EXPECT_TRUE(sawContainer) << "APFS backup NXSB at last block must restore container";
    EXPECT_TRUE(sawVolume);
}

TEST(ApfsContainer, UnreadNxsbTailIsSentinelNotMissingVolume) {
    std::vector<uint8_t> img(32 * 4096, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeBe64(img.data() + 40, 4096);
    writeBe64(img.data() + 48, 32);
    std::memcpy(img.data() + 4096 + 32, "APSB", 4);
    std::memcpy(img.data() + 4096 + 72, "Macintosh HD", 12);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Probe reads 512 bytes (sector 0). Walk used a single 4096-byte NXSB read,
    // so a fault in sectors 1–7 dropped the whole container as “not APFS”.
    reader.setMemoryFaultRange(1, 7);

    bool sawVolume = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    ASSERT_TRUE(walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_volume" && fr.name == "Macintosh HD") sawVolume = true;
        if (fr.source == kApfsNxsbUnreadSource) sawUnread = true;
    }, &running));

    EXPECT_TRUE(sawVolume) << "unread NXSB tail must not hide a readable APSB at block 1";
    EXPECT_TRUE(sawUnread);
}

TEST(ApfsContainer, UnreadApsbBlockIsSentinelNotEmptyContainer) {
    std::vector<uint8_t> img(32 * 4096, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeBe64(img.data() + 40, 4096);
    writeBe64(img.data() + 48, 32);
    std::memcpy(img.data() + 4096 + 32, "APSB", 4);
    std::memcpy(img.data() + 4096 + 72, "Macintosh HD", 12);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // APSB at block 1 = 4096/512 = sector 8, 8 sectors. NXSB at sector 0 stays readable.
    reader.setMemoryFaultRange(8, 8);

    bool sawContainer = false;
    bool sawVolume = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    ASSERT_TRUE(walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_container") sawContainer = true;
        if (fr.source == "apfs_volume" && fr.name == "Macintosh HD") sawVolume = true;
        if (fr.source == kApfsBlockUnreadSource) sawUnread = true;
    }, &running));

    EXPECT_TRUE(sawContainer);
    EXPECT_FALSE(sawVolume) << "unread APSB must not parse zeros as a missing Macintosh HD";
    EXPECT_TRUE(sawUnread);
}

TEST(ApfsContainer, UnreadSprayDataBlockIsNotSentinelWhenVolumeFound) {
    std::vector<uint8_t> img(32 * 4096, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeBe64(img.data() + 40, 4096);
    writeBe64(img.data() + 48, 32);
    std::memcpy(img.data() + 4096 + 32, "APSB", 4);
    std::memcpy(img.data() + 4096 + 72, "Macintosh HD", 12);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(10 * 8, 8);

    bool sawVolume = false;
    bool sawUnread = false;
    bool sawCatalogUnread = false;
    std::atomic<bool> running{true};
    ASSERT_TRUE(walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_volume" && fr.name == "Macintosh HD") sawVolume = true;
        if (fr.source == kApfsBlockUnreadSource) sawUnread = true;
        if (fr.source == kApfsCatalogUnreadSource) sawCatalogUnread = true;
    }, &running));
    EXPECT_TRUE(sawVolume);
    EXPECT_FALSE(sawUnread) << "unread non-APSB spray cluster is not a missing volume";
    EXPECT_TRUE(sawCatalogUnread);
}

TEST(HfsCatalog, OverflowExtentsMergedIntoRuns) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeExtentOverflowLeaf(img, 5, 100, 40, 4, bs);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].name, "big.bin");
    ASSERT_GE(hits[0].runs.size(), 2u);
    bool hasOverflow = false;
    for (const auto& r : hits[0].runs) {
        if (r.startSector == (40 * bs) / 512) hasOverflow = true;
    }
    EXPECT_TRUE(hasOverflow);
}

TEST(HfsCatalog, ResourceForkEmittedAsSibling) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeafWithResource(img, 4, 100, bs, 12, bs, 20, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    bool sawData = false;
    bool sawRsrc = false;
    for (const auto& h : hits) {
        if (h.name == "big.bin") {
            sawData = true;
            EXPECT_EQ(h.sizeBytes, bs);
        }
        if (h.name == "big.bin.rsrc") {
            sawRsrc = true;
            EXPECT_EQ(h.sizeBytes, bs);
            ASSERT_FALSE(h.runs.empty());
            EXPECT_EQ(h.runs[0].startSector, (20ull * bs) / 512);
        }
    }
    EXPECT_TRUE(sawData);
    EXPECT_TRUE(sawRsrc) << "HFS+ resource fork must emit as recoverable sibling";
}

TEST(HfsCatalog, ResourceForkOverflowExtentsMergedIntoRuns) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeExtentOverflowLeaf(img, 5, 100, 40, 1, bs, 0xFF);

    const size_t off = blockOffset(4, bs);
    if (img.size() < off + bs) img.resize(off + bs, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    const uint8_t nameUtf16[] = {
        0x00, 'b', 0x00, 'i', 0x00, 'g', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, 'n',
    };
    const uint16_t nameBytes = sizeof(nameUtf16);
    const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
    writeBe16(rec, keyLen);
    writeBe32(rec + 2, 2);
    writeBe16(rec + 6, nameBytes);
    std::memcpy(rec + 8, nameUtf16, nameBytes);
    uint8_t* val = rec + 2 + keyLen;
    writeBe16(val, 2);
    writeBe32(val + 12, 100);
    writeBe64(val + 92, bs);
    writeBe32(val + 92 + 16, 12);
    writeBe32(val + 92 + 20, 1);
    writeBe64(val + 172, 9ull * bs);
    for (int i = 0; i < 8; ++i) {
        writeBe32(val + 172 + 16 + i * 8, 20 + static_cast<uint32_t>(i));
        writeBe32(val + 172 + 16 + i * 8 + 4, 1);
    }
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + 92 + 80 + 80);
    const size_t offTable = bs - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    bool sawOverflow = false;
    for (const auto& h : hits) {
        if (h.name != "big.bin.rsrc") continue;
        EXPECT_EQ(h.sizeBytes, 9ull * bs);
        ASSERT_GE(h.runs.size(), 2u);
        for (const auto& r : h.runs) {
            if (r.startSector == (40ull * bs) / 512) sawOverflow = true;
        }
    }
    EXPECT_TRUE(sawOverflow) << "HFS+ resource fork overflow extents (forkType 0xFF) must merge into runs";
}

TEST(HfsCatalog, CatalogDatesSetModifiedAt) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeaf(img, 4, 100, bs, bs);
    const size_t off = blockOffset(4, bs);
    uint8_t* rec = img.data() + off + 14;
    const uint16_t keyLen = static_cast<uint16_t>((rec[0] << 8) | rec[1]);
    uint8_t* val = rec + 2 + keyLen;
    writeBe32(val + 16, 3660768000u);
    writeBe32(val + 20, 3660768000u);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    int64_t got = 0;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.name == "big.bin") got = fr.modifiedAt;
    }, &running);
    EXPECT_EQ(got, 1577923200) << "HFS+ contentModDate (1904 epoch) must set modifiedAt";
}

TEST(HfsCatalog, RootFolderNameIsVolumeDiscovery) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogRootFolderLeaf(img, 4, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    ASSERT_TRUE(scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "hfs_vol_name" && fr.name == "BYTEBACK") found = true;
    }, &running));
    EXPECT_TRUE(found) << "HFS+ root folder (parent CNID 1) is the volume name";
}

TEST(HfsCatalog, UnreadRootNodeIsSentinelNotEmptyCatalog) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Catalog root is allocation block 4 → 4*4096/512 = sector 32, 8 sectors.
    reader.setMemoryFaultRange(32, 8);

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    ASSERT_TRUE(scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawFile = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "big.bin") sawFile = true;
    }
    for (const auto& s : sources) {
        if (s == kHfsCatalogUnreadSource) sawUnread = true;
    }
    EXPECT_FALSE(sawFile) << "unread HFS catalog node must not parse zeros as an empty listing of big.bin";
    EXPECT_TRUE(sawUnread);
}

TEST(HfsCatalog, EmptyRunsMarkedDeletedWithLowConfidence) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeafNoExtents(img, 4, 200, 8192, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord hit{};
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.name == "gone.dat") hit = fr;
    }, &running);

    EXPECT_EQ(hit.name, "gone.dat");
    EXPECT_EQ(hit.status, 0);
    EXPECT_EQ(hit.confidence, 30);
    EXPECT_TRUE(hit.runs.empty());
    EXPECT_GT(hit.sizeBytes, 0u);
}

void writeCatalogTwoFileLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 2);

    auto writeFileRec = [&](uint8_t* rec, const uint8_t* nameUtf16, uint16_t nameBytes, uint32_t fileId) -> uint16_t {
        const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
        writeBe16(rec, keyLen);
        writeBe32(rec + 2, 2);
        writeBe16(rec + 6, nameBytes);
        std::memcpy(rec + 8, nameUtf16, nameBytes);
        uint8_t* val = rec + 2 + keyLen;
        writeBe16(val, 2);
        writeBe32(val + 12, fileId);
        writeBe64(val + 92, blockSize);
        writeBe32(val + 92 + 16, 20);
        writeBe32(val + 92 + 20, 1);
        return static_cast<uint16_t>(2 + keyLen + kCatalogFileValueLen);
    };

    const uint8_t nameA[] = { 0x00, 'a', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, 'n' };
    const uint8_t nameB[] = { 0x00, 'b', 0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't' };
    const uint16_t recStart0 = 14;
    const uint16_t len0 = writeFileRec(node + recStart0, nameA, sizeof(nameA), 101);
    const uint16_t recStart1 = recStart0 + len0;
    const uint16_t len1 = writeFileRec(node + recStart1, nameB, sizeof(nameB), 102);
    const uint16_t recEnd1 = recStart1 + len1;
    const size_t offTable = blockSize - 6;
    writeBe16(node + offTable, recStart0);
    writeBe16(node + offTable + 2, recStart1);
    writeBe16(node + offTable + 4, recEnd1);
}

TEST(HfsCatalog, EmitsSentinelWhenHittingMaxFiles) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogTwoFileLeaf(img, 4, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running, 1);

    ASSERT_GE(hits.size(), 2u);
    EXPECT_EQ(hits[0].source, "hfs_catalog");
    bool sawLimit = false;
    for (const auto& h : hits) {
        if (h.source == "hfs_limit") sawLimit = true;
    }
    EXPECT_TRUE(sawLimit);
    EXPECT_EQ(std::count_if(hits.begin(), hits.end(),
                            [](const FileRecord& r) { return r.source == "hfs_catalog"; }), 1);
}

TEST(HfsCatalog, UnlimitedDefaultEmitsBothFiles) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogTwoFileLeaf(img, 4, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    EXPECT_EQ(std::count_if(hits.begin(), hits.end(),
                            [](const FileRecord& r) { return r.source == "hfs_catalog"; }), 2);
    EXPECT_FALSE(std::any_of(hits.begin(), hits.end(),
                             [](const FileRecord& r) { return r.source == "hfs_limit"; }));
}

TEST(HfsCatalog, UnusedLeafSlotEmitsDeletedFile) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogLeafWithUnusedGone(img, 4, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    auto gone = std::find_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "gone.dat";
    });
    ASSERT_NE(gone, hits.end()) << "deleted leftover catalog file in leaf free space must emit";
    EXPECT_EQ(gone->source, "hfs_catalog_unused");
    EXPECT_EQ(gone->status, 0);
    EXPECT_GE(gone->confidence, 40);
    EXPECT_LE(gone->confidence, 55);
    EXPECT_FALSE(gone->runs.empty()) << "leftover extents stay recoverable, not discovery-only";
    EXPECT_TRUE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "big.bin" && r.source == "hfs_catalog";
    }));
}

TEST(HfsCatalog, JournalCatalogLeafEmitsDeletedFile) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    writeHfsJournalCatalogLeaf(img, 6, 7, bs, 200, goneName, sizeof(goneName), 30);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    auto gone = std::find_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "gone.dat";
    });
    ASSERT_NE(gone, hits.end()) << "committed HFS+ journal catalog leaf must emit";
    EXPECT_EQ(gone->source, "hfs_journal");
    EXPECT_EQ(gone->status, 0);
    EXPECT_GE(gone->confidence, 40);
    EXPECT_LE(gone->confidence, 55);
    EXPECT_FALSE(gone->runs.empty()) << "journal extents stay recoverable, not discovery-only";
    EXPECT_TRUE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "big.bin" && r.source == "hfs_catalog";
    }));
}

TEST(HfsCatalog, EmptyJournalDoesNotInventFiles) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    writeHfsJournalCatalogLeaf(img, 6, 7, bs, 200, goneName, sizeof(goneName), 30, true);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    EXPECT_FALSE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.source == "hfs_journal" || r.name == "gone.dat";
    }));
    EXPECT_TRUE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "big.bin" && r.source == "hfs_catalog";
    }));
}

TEST(HfsCatalog, JournalDoesNotDuplicateLiveFileId) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t liveName[] = {
        0x00, 'b', 0x00, 'i', 0x00, 'g', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, 'n',
    };
    writeHfsJournalCatalogLeaf(img, 6, 7, bs, 100, liveName, sizeof(liveName), 10);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    EXPECT_EQ(std::count_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "big.bin";
    }), 1);
    EXPECT_FALSE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.source == "hfs_journal";
    }));
}

TEST(HfsCatalog, JournalLittleEndianCatalogLeafEmitsDeletedFile) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    writeHfsJournalCatalogLeaf(img, 6, 7, bs, 200, goneName, sizeof(goneName), 30, false, true);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    auto gone = std::find_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "gone.dat" && r.source == "hfs_journal";
    });
    ASSERT_NE(gone, hits.end()) << "Intel Mac LE journal catalog leaf must emit";
    EXPECT_FALSE(gone->runs.empty());
}

TEST(HfsCatalog, BadJournalChecksumDoesNotEmit) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    writeHfsJournalCatalogLeaf(img, 6, 7, bs, 200, goneName, sizeof(goneName), 30);
    const uint64_t jOff = static_cast<uint64_t>(7) * bs;
    writeBe32(img.data() + jOff + 36, 0xFFFFFFFFu);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    EXPECT_FALSE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.source == "hfs_journal" || r.name == "gone.dat";
    }));
}

TEST(HfsCatalog, JournalWrapAroundCatalogLeafEmitsDeletedFile) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs, 6);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const uint8_t goneName[] = {
        0x00, 'g', 0x00, 'o', 0x00, 'n', 0x00, 'e', 0x00, '.', 0x00, 'd', 0x00, 'a', 0x00, 't',
    };
    writeHfsJournalWrappedCatalogLeaf(img, 6, 7, bs, 200, goneName, sizeof(goneName), 30);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    auto gone = std::find_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "gone.dat" && r.source == "hfs_journal";
    });
    ASSERT_NE(gone, hits.end()) << "circular journal wrap must still emit catalog leaf";
}

TEST(HfsCatalog, BackupVolumeHeaderUsedWhenPrimaryWiped) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    std::memcpy(img.data() + img.size() - 1024, img.data() + 1024, 512);
    std::memset(img.data() + 1024, 0, 512);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    EXPECT_TRUE(std::any_of(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "big.bin" && r.source == "hfs_catalog";
    })) << "TN1150 backup volume header at volume-end-1024 must recover catalog";
}

TEST(HfsCatalog, EmptyLeafUnusedSlotEmitsDeletedFile) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogEmptyLeafWithUnusedGone(img, 4, bs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) hits.push_back(fr);
    }, &running);

    auto gone = std::find_if(hits.begin(), hits.end(), [](const FileRecord& r) {
        return r.name == "gone.dat" && r.source == "hfs_catalog_unused";
    });
    ASSERT_NE(gone, hits.end()) << "numRecords=0 leaf must still scan leftover catalog file bytes";
    EXPECT_EQ(gone->status, 0);
    EXPECT_GE(gone->confidence, 40);
    EXPECT_LE(gone->confidence, 55);
}

TEST(HfsCatalog, UnusedSlotSkipsDuplicateLiveFileId) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);
    writeCatalogFileLeaf(img, 4, 100, 12 * bs, bs);
    const size_t off = blockOffset(4, bs);
    uint8_t* node = img.data() + off;
    const uint16_t liveEnd = readBe16(node + bs - 2);
    std::memcpy(node + liveEnd, node + 14, static_cast<size_t>(liveEnd - 14));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.name == "big.bin") hits.push_back(fr);
    }, &running);

    EXPECT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].source, "hfs_catalog");
}

void writeLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void writeLe64At(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

TEST(ApfsContainer, CatalogEmitsHashedDirRec) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(8 * bs, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, 8);

    uint8_t* node = img.data() + 2 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 1);
    const uint64_t hdr = (9ull << 48) | 7ull;
    writeLe64At(node + 56, hdr);
    const char* name = "note.txt";
    const uint32_t nlen = 9;
    writeLe32(node + 64, nlen);
    std::memcpy(node + 68, name, 8);
    node[68 + 8] = 0;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawFile = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "note.txt") sawFile = true;
    }, &running);
    EXPECT_TRUE(sawFile);
}

TEST(ApfsContainer, InodeModTimeSetsModifiedAt) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(8 * bs, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, 8);

    uint8_t* node = img.data() + 2 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 2);
    const uint64_t dirHdr = (9ull << 48) | 7ull;
    writeLe64At(node + 56, dirHdr);
    const char* name = "note.txt";
    const uint32_t nlen = 9;
    writeLe32(node + 64, nlen);
    std::memcpy(node + 68, name, 8);
    node[68 + 8] = 0;
    const uint64_t inoHdr = (3ull << 48) | 7ull;
    writeLe64At(node + 88, inoHdr);
    writeLe64At(node + 88 + 8 + 24, 1577923200ull * 1000000000ull);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    int64_t got = 0;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "note.txt") got = fr.modifiedAt;
    }, &running);
    EXPECT_EQ(got, 1577923200) << "APFS j_inode_val mod_time ns must fill modifiedAt seconds";
}

TEST(ApfsContainer, InodeCreateTimeSetsCreatedAt) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(8 * bs, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, 8);

    uint8_t* node = img.data() + 2 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 2);
    const uint64_t dirHdr = (9ull << 48) | 7ull;
    writeLe64At(node + 56, dirHdr);
    const char* name = "note.txt";
    const uint32_t nlen = 9;
    writeLe32(node + 64, nlen);
    std::memcpy(node + 68, name, 8);
    node[68 + 8] = 0;
    const uint64_t inoHdr = (3ull << 48) | 7ull;
    writeLe64At(node + 88, inoHdr);
    writeLe64At(node + 88 + 8 + 16, 1577923200ull * 1000000000ull);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    int64_t got = 0;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "note.txt") got = fr.createdAt;
    }, &running);
    EXPECT_EQ(got, 1577923200) << "APFS j_inode_val create_time ns must fill createdAt seconds";
}

TEST(ApfsContainer, DirRecWithExtentAttachesRuns) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(8 * bs, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, 8);

    uint8_t* node = img.data() + 2 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 2);
    const uint64_t dirHdr = (9ull << 48) | 42ull;
    writeLe64At(node + 56, dirHdr);
    const char* name = "data.bin";
    writeLe32(node + 64, 9);
    std::memcpy(node + 68, name, 8);
    node[68 + 8] = 0;
    const uint64_t extHdr = (8ull << 48) | 42ull;
    writeLe64At(node + 88, extHdr);
    writeLe64At(node + 96, 8192);
    writeLe64At(node + 104, 4);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord hit;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "data.bin") hit = fr;
    }, &running);
    EXPECT_EQ(hit.source, "apfs_file");
    ASSERT_EQ(hit.runs.size(), 1u);
    EXPECT_EQ(hit.runs[0].startSector, (4ull * bs) / 512);
    EXPECT_GE(hit.confidence, 80);
}

TEST(ApfsContainer, FileExtentEmitsRecoverableRuns) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(8 * bs, 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, 8);

    uint8_t* node = img.data() + 2 * bs;
    writeLe32(node + 24, 2);
    writeLe32(node + 36, 1);
    const uint64_t hdr = (8ull << 48) | 1ull;
    writeLe64At(node + 56, hdr);
    writeLe64At(node + 64, 4096);
    writeLe64At(node + 72, 3);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord extent;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_extent") extent = fr;
    }, &running);
    EXPECT_EQ(extent.source, "apfs_extent");
    ASSERT_EQ(extent.runs.size(), 1u);
    EXPECT_EQ(extent.runs[0].startSector, (3ull * bs) / 512);
}

TEST(ApfsContainer, OmapFollowsPaddrPast256Probe) {
    const uint32_t bs = 4096;
    const uint64_t blocks = 320;
    std::vector<uint8_t> img(static_cast<size_t>(blocks * bs), 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, blocks);
    writeLe64At(img.data() + 0xA0, 5);

    uint8_t* omap = img.data() + 5 * bs;
    writeLe64At(omap + 48, 6);

    uint8_t* tree = img.data() + 6 * bs;
    writeLe32(tree + 24, 2);
    writeLe32(tree + 36, 1);
    writeLe64At(tree + 56, 1);
    writeLe64At(tree + 64, 1);
    writeLe64At(tree + 72, 300);

    uint8_t* node = img.data() + 300 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 1);
    const uint64_t hdr = (9ull << 48) | 7ull;
    writeLe64At(node + 56, hdr);
    const char* name = "far.txt";
    const uint32_t nlen = 8;
    writeLe32(node + 64, nlen);
    std::memcpy(node + 68, name, 7);
    node[68 + 7] = 0;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawFile = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "far.txt") sawFile = true;
    }, &running);
    EXPECT_TRUE(sawFile);
}

TEST(HfsCatalog, OffsetTableFitsRejectsUnderflow) {
    EXPECT_TRUE(hfsOffsetTableFits(4096, 1));
    EXPECT_TRUE(hfsOffsetTableFits(14, 0));
    EXPECT_FALSE(hfsOffsetTableFits(13, 0));
    EXPECT_FALSE(hfsOffsetTableFits(4096, 65535));
    EXPECT_FALSE(hfsOffsetTableFits(16, 8));
}

// Regression: folder records (recType 4) were skipped entirely, so files
// key'd under a folder resolved their path against "/" instead of the folder.
TEST(HfsCatalog, FolderRecordBuildsNestedFilePath) {
    const uint32_t bs = 4096;
    std::vector<uint8_t> img(64 * bs, 0);
    writeHfsVolumeHeader(img, 4, 5, bs);

    const size_t off = blockOffset(4, bs);
    uint8_t* node = img.data() + off;
    writeNodeHeader(node, 2);
    const uint16_t recStart0 = 14;

    // Folder record: key parent=2 (root), name "Sub"; value fileID = 20.
    const uint8_t dirName[] = { 0x00, 'S', 0x00, 'u', 0x00, 'b' };
    const uint16_t dirKeyLen = static_cast<uint16_t>(4 + 2 + sizeof(dirName));
    uint8_t* rec0 = node + recStart0;
    writeBe16(rec0, dirKeyLen);
    writeBe32(rec0 + 2, 2);
    writeBe16(rec0 + 6, static_cast<uint16_t>(sizeof(dirName)));
    std::memcpy(rec0 + 8, dirName, sizeof(dirName));
    uint8_t* val0 = rec0 + 2 + dirKeyLen;
    writeBe16(val0, 4); // folder record
    writeBe32(val0 + 12, 20);
    const uint16_t len0 = static_cast<uint16_t>(2 + dirKeyLen + 16);

    // File record: key parent=20, name "nested.txt"; fork extent 30..30.
    const uint8_t fileName[] = {
        0x00, 'n', 0x00, 'e', 0x00, 's', 0x00, 't', 0x00, 'e', 0x00, 'd',
        0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't',
    };
    const uint16_t fileKeyLen = static_cast<uint16_t>(4 + 2 + sizeof(fileName));
    const uint16_t recStart1 = static_cast<uint16_t>(recStart0 + len0);
    uint8_t* rec1 = node + recStart1;
    writeBe16(rec1, fileKeyLen);
    writeBe32(rec1 + 2, 20);
    writeBe16(rec1 + 6, static_cast<uint16_t>(sizeof(fileName)));
    std::memcpy(rec1 + 8, fileName, sizeof(fileName));
    uint8_t* val1 = rec1 + 2 + fileKeyLen;
    writeBe16(val1, 2); // file record
    writeBe32(val1 + 12, 21);
    writeBe64(val1 + 92, 16);
    writeBe32(val1 + 92 + 16, 30);
    writeBe32(val1 + 92 + 16 + 4, 1);
    const uint16_t len1 = static_cast<uint16_t>(2 + fileKeyLen + kCatalogFileValueLen);
    const uint16_t recEnd1 = static_cast<uint16_t>(recStart1 + len1);

    const size_t offTable = bs - 6;
    writeBe16(node + offTable, recStart0);
    writeBe16(node + offTable + 2, recStart1);
    writeBe16(node + offTable + 4, recEnd1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::string path;
    std::atomic<bool> running{true};
    scanHfsPlusCatalog(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.name == "nested.txt") path = fr.path;
    }, &running);
    EXPECT_EQ(path, "/Sub/nested.txt");
}

// Regression: NXSB block count and omap oid must be read from their spec
// offsets (nx_max_file_system_blocks @48, nx_omap_oid @64), not the legacy
// probe offsets.
TEST(ApfsContainer, SpecOffsetsResolveOmapAndVolumes) {
    const uint32_t bs = 4096;
    const uint64_t blocks = 320;
    std::vector<uint8_t> img(static_cast<size_t>(blocks * bs), 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);       // nx_blocksize (valid, no legacy fallback)
    writeLe64At(img.data() + 48, blocks); // nx_max_file_system_blocks (spec)
    writeLe64At(img.data() + 0x40, 5);    // nx_omap_oid (spec)

    uint8_t* omap = img.data() + 5 * bs;
    writeLe64At(omap + 48, 6); // b-tree root block

    uint8_t* tree = img.data() + 6 * bs;
    writeLe32(tree + 24, 2);  // object type: B-tree node
    writeLe32(tree + 36, 1);  // 1 key
    writeLe64At(tree + 56, 1);
    writeLe64At(tree + 64, 1);
    writeLe64At(tree + 72, 300); // leaf paddr past the 256-block probe

    uint8_t* node = img.data() + 300 * bs;
    writeLe32(node + 24, 2);
    node[32] = 2;
    writeLe32(node + 36, 1);
    const uint64_t hdr = (9ull << 48) | 7ull;
    writeLe64At(node + 56, hdr);
    const char* name = "far.txt";
    const uint32_t nlen = 8;
    writeLe32(node + 64, nlen);
    std::memcpy(node + 68, name, 7);
    node[68 + 7] = 0;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawFile = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.source == "apfs_file" && fr.name == "far.txt") sawFile = true;
    }, &running);
    EXPECT_TRUE(sawFile);
}

TEST(HFSParser, UnreadSiblingSectorDoesNotHideVolumeHeader) {
    std::vector<uint8_t> img(64 * 1024, 0);
    writeHfsVolumeHeader(img, 4, 3);
    std::memset(img.data() + 1024 + 272, 0, 80); // empty catalog fork → linear fallback
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(0, 1); // blinds a 4 MiB linear chunk; VH lives at sector 2
    HFSParser hfs;
    bool sawVh = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    hfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "hfs_vh") sawVh = true;
        if (fr.source == "hfs_linear_unread") sawUnread = true;
    }, &running, 0, 0);
    EXPECT_TRUE(sawVh) << "one unread sector must not hide a readable HFS volume header in the same 4 MiB window";
    EXPECT_TRUE(sawUnread);
}

TEST(HFSParser, UnreadLinearChunkIsSentinelNotEmptyVolume) {
    std::vector<uint8_t> img(64 * 1024, 0);
    writeHfsVolumeHeader(img, 4, 3);
    std::memset(img.data() + 1024 + 272, 0, 80);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(2, 1); // VH sector unread
    HFSParser hfs;
    bool sawVh = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    hfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "hfs_vh") sawVh = true;
        if (fr.source == "hfs_linear_unread") sawUnread = true;
    }, &running, 0, 0);
    EXPECT_FALSE(sawVh) << "unread VH must not parse as a volume";
    EXPECT_TRUE(sawUnread) << "empty HFS linear scan after unread I/O is not “no HFS”";
}

TEST(APFSParser, UnreadSiblingSectorDoesNotHideNxsb) {
    std::vector<uint8_t> img(32 * 1024, 0);
    std::memcpy(img.data() + 8192 + 32, "NXSB", 4); // after walk's 4096 prefix so linear runs
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(8, 1); // in the 4 MiB linear window; NXSB at sector 16
    APFSParser apfs;
    bool sawNxsb = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    apfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "apfs_nxsb") sawNxsb = true;
        if (fr.source == "apfs_linear_unread") sawUnread = true;
    }, &running, 0, 0);
    EXPECT_TRUE(sawNxsb) << "one unread sector must not hide a readable NXSB in the same 4 MiB window";
    EXPECT_TRUE(sawUnread);
}

TEST(ApfsContainer, CheckpointOmapEmitsDeletedFile) {
    const uint32_t bs = 4096;
    const uint64_t blocks = 320;
    std::vector<uint8_t> img(static_cast<size_t>(blocks * bs), 0);
    std::memcpy(img.data() + 32, "NXSB", 4);
    writeLe32(img.data() + 36, bs);
    writeLe64At(img.data() + 40, blocks);
    writeLe32(img.data() + 0x68, 1);     // nx_xp_desc_blocks
    writeLe64At(img.data() + 0x70, 8);   // nx_xp_desc_base
    writeLe64At(img.data() + 0xA0, 5);   // live nx_omap_oid

    uint8_t* liveOmap = img.data() + 5 * bs;
    writeLe64At(liveOmap + 48, 6);
    uint8_t* liveTree = img.data() + 6 * bs;
    writeLe32(liveTree + 24, 2);
    writeLe32(liveTree + 36, 1);
    writeLe64At(liveTree + 56, 1);
    writeLe64At(liveTree + 64, 1);
    writeLe64At(liveTree + 72, 300);

    uint8_t* liveNode = img.data() + 300 * bs;
    writeLe32(liveNode + 24, 2);
    liveNode[32] = 2;
    writeLe32(liveNode + 36, 1);
    writeLe64At(liveNode + 56, (9ull << 48) | 7ull);
    writeLe32(liveNode + 64, 9);
    std::memcpy(liveNode + 68, "live.txt", 8);
    liveNode[76] = 0;

    uint8_t* xp = img.data() + 8 * bs;
    std::memcpy(xp + 32, "NXSB", 4);
    writeLe32(xp + 36, bs);
    writeLe64At(xp + 40, blocks);
    writeLe64At(xp + 0xA0, 9);

    uint8_t* staleOmap = img.data() + 9 * bs;
    writeLe64At(staleOmap + 48, 11);
    uint8_t* staleTree = img.data() + 11 * bs;
    writeLe32(staleTree + 24, 2);
    writeLe32(staleTree + 36, 1);
    writeLe64At(staleTree + 56, 1);
    writeLe64At(staleTree + 64, 1);
    writeLe64At(staleTree + 72, 301);

    uint8_t* staleNode = img.data() + 301 * bs;
    writeLe32(staleNode + 24, 2);
    staleNode[32] = 2;
    writeLe32(staleNode + 36, 1);
    writeLe64At(staleNode + 56, (9ull << 48) | 8ull);
    writeLe32(staleNode + 64, 9);
    std::memcpy(staleNode + 68, "gone.dat", 8);
    staleNode[76] = 0;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord gone{};
    bool sawLive = false;
    std::atomic<bool> running{true};
    walkApfsContainer(reader, 0, 0, [&](const FileRecord& fr) {
        if (fr.name == "live.txt") sawLive = true;
        if (fr.name == "gone.dat") gone = fr;
    }, &running);
    EXPECT_TRUE(sawLive);
    ASSERT_EQ(gone.name, "gone.dat") << "checkpoint omap catalog leftover must emit";
    EXPECT_EQ(gone.source, "apfs_omap_deleted");
    EXPECT_EQ(gone.status, 0);
    EXPECT_GE(gone.confidence, 40);
    EXPECT_LE(gone.confidence, 55);
}
