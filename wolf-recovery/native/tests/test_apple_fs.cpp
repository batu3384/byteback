#include "fs/apfs_container.h"
#include "fs/hfs_catalog.h"
#include "wolf_io.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace wolf;

namespace {

void writeBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
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

void writeFork(uint8_t* dst, uint32_t startBlock, uint32_t blockCount, uint64_t logicalSize = 0) {
    writeBe64(dst, logicalSize);
    writeBe32(dst + 16, startBlock);
    writeBe32(dst + 20, blockCount);
}

size_t blockOffset(uint32_t blockIndex, uint32_t blockSize) {
    return static_cast<size_t>(blockIndex) * blockSize;
}

void writeHfsVolumeHeader(std::vector<uint8_t>& img, uint32_t catalogBlock, uint32_t extentsBlock,
                          uint32_t blockSize = 4096) {
    const size_t hdrOff = 1024;
    if (img.size() < hdrOff + 512) img.resize(hdrOff + 512, 0);
    img[hdrOff] = 0x48;
    img[hdrOff + 1] = 0x2B;
    writeBe32(img.data() + hdrOff + 40, blockSize);
    writeFork(img.data() + hdrOff + 192, extentsBlock, 1);
    writeFork(img.data() + hdrOff + 272, catalogBlock, 1);
}

void writeExtentOverflowLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t fileId,
                             uint32_t extraStart, uint32_t extraCount, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    node[0] = 0xFF;
    writeBe16(node + 4, 1);
    const uint16_t recStart = 14;
    uint8_t* rec = node + recStart;
    writeBe16(rec, 10);
    writeBe16(rec + 2, 0);
    writeBe32(rec + 4, fileId);
    uint8_t* val = rec + 12;
    writeBe32(val, extraStart);
    writeBe32(val + 4, extraCount);
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 12 + 96);
    const size_t offTable = blockSize - 4;
    writeBe16(node + offTable, recStart);
    writeBe16(node + offTable + 2, recEnd);
}

void writeCatalogFileLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t fileId,
                          uint64_t logicalSize, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    node[0] = 0xFF;
    writeBe16(node + 4, 1);
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
    writeBe32(val + 8, fileId);
    writeBe64(val + 0x50, logicalSize);
    for (int i = 0; i < 8; ++i) {
        writeBe32(val + 0x50 + 16 + i * 12, 10 + static_cast<uint32_t>(i));
        writeBe32(val + 0x50 + 16 + i * 12 + 4, 1);
    }
    const uint16_t recEnd = static_cast<uint16_t>(recStart + 2 + keyLen + 0xA0);
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

void writeCatalogTwoFileLeaf(std::vector<uint8_t>& img, uint32_t blockIndex, uint32_t blockSize = 4096) {
    const size_t off = blockOffset(blockIndex, blockSize);
    if (img.size() < off + blockSize) img.resize(off + blockSize, 0);
    uint8_t* node = img.data() + off;
    node[0] = 0xFF;
    writeBe16(node + 4, 2);

    auto writeFileRec = [&](uint8_t* rec, const uint8_t* nameUtf16, uint16_t nameBytes, uint32_t fileId) -> uint16_t {
        const uint16_t keyLen = static_cast<uint16_t>(4 + 2 + nameBytes);
        writeBe16(rec, keyLen);
        writeBe32(rec + 2, 2);
        writeBe16(rec + 6, nameBytes);
        std::memcpy(rec + 8, nameUtf16, nameBytes);
        uint8_t* val = rec + 2 + keyLen;
        writeBe16(val, 2);
        writeBe32(val + 8, fileId);
        writeBe64(val + 0x50, blockSize);
        writeBe32(val + 0x50 + 16, 20);
        writeBe32(val + 0x50 + 20, 1);
        return static_cast<uint16_t>(2 + keyLen + 0xA0);
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
