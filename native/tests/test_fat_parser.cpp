#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace byteback;

TEST(FatParser, ListsRootFileOnFat16Superfloppy) {
    auto img = byteback::testfix::buildFat16Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected TEST.TXT in FAT16 root";
}

TEST(FatParser, ScanAtPartitionOffset) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    auto disk = byteback::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running, 2048ull * 512));

    EXPECT_FALSE(names.empty());
}

namespace {

void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    if (off + 2 > img.size()) img.resize(off + 2, 0);
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>(v >> 8);
}

void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    if (off + 4 > img.size()) img.resize(off + 4, 0);
    img[off] = static_cast<uint8_t>(v);
    img[off + 1] = static_cast<uint8_t>(v >> 8);
    img[off + 2] = static_cast<uint8_t>(v >> 16);
    img[off + 3] = static_cast<uint8_t>(v >> 24);
}

void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        if (off + static_cast<size_t>(i) >= img.size()) img.resize(off + 8, 0);
        img[off + i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

std::vector<uint8_t> buildExFatWithGappedDeletedSet() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t fatOff = 24;
    constexpr uint32_t heapOff = 25;
    constexpr uint32_t totalSectors = 64;
    std::vector<uint8_t> img(totalSectors * ss, 0);

    std::memcpy(img.data() + 3, "EXFAT   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;
    img[108] = 9;  // bytesPerSectorShift
    img[109] = 0;  // sectorsPerClusterShift
    img[110] = 1;  // numFats
    writeLe32(img, 80, fatOff);
    writeLe32(img, 84, 1);
    writeLe32(img, 88, heapOff);
    writeLe32(img, 92, 8);
    writeLe32(img, 96, 2); // root cluster

    writeLe32(img, fatOff * ss + 8, 0xFFFFFFFF);
    writeLe32(img, fatOff * ss + 12, 0xFFFFFFFF);

    const uint16_t chk = 0xBEEF;
    size_t de = heapOff * ss;
    img[de] = 0x85; // file in use
    img[de + 1] = 2; // stream + 1 name
    writeLe16(img, de + 2, chk);
    img[de + 4] = 0x00; // not directory

    de += 32;
    img[de] = 0x05; // unrelated deleted file set starter
    img[de + 1] = 1;
    writeLe16(img, de + 2, 0x0001);

    de += 32;
    img[de] = 0xC0; // stream (spec entry type; secondaries carry no checksum)
    img[de + 3] = 8; // name length (LOST.DAT)
    writeLe32(img, de + 20, 3); // first cluster
    writeLe64(img, de + 24, 17); // data length (spec offset 24)

    de += 32;
    img[de] = 0xC1; // name
    static const uint8_t lostName[] = {'L',0,'O',0,'S',0,'T',0,'.',0,'D',0,'A',0,'T',0};
    std::memcpy(img.data() + de + 2, lostName, sizeof(lostName)); // chars start at byte 2

    const char payload[] = "recovered!";
    std::memcpy(img.data() + (heapOff + 1) * ss, payload, sizeof(payload) - 1);
    return img;
}

std::vector<uint8_t> buildExFatPartialDeletedChain() {
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
    writeLe32(img, fatOff * ss + 3 * 4, 0xFFFFFFFF); // cluster 3 EOC — single cluster chain

    const uint16_t chk = 0xCAFE;
    size_t de = heapOff * ss;
    img[de] = 0x05; // deleted file set
    img[de + 1] = 2;
    writeLe16(img, de + 2, chk);

    de += 32;
    img[de] = 0x40; // deleted stream (0xC0 minus the in-use bit)
    writeLe32(img, de + 20, 3);
    writeLe64(img, de + 24, 50000); // logical size >> one cluster (spec offset 24)

    de += 32;
    img[de] = 0xC1;
    static const uint8_t name[] = {'B',0,'I',0,'G',0,'.',0,'D',0,'A',0,'T',0};
    std::memcpy(img.data() + de + 2, name, sizeof(name));
    return img;
}

} // namespace

TEST(FatParser, ExFatToleratesGappedEntrySet) {
    auto img = buildExFatWithGappedDeletedSet();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "LOST.DAT") found = true;
    }, &running));
    EXPECT_TRUE(found);
}

TEST(FatParser, ExFatPartialDeletedChainCapsConfidence) {
    auto img = buildExFatPartialDeletedChain();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord hit{};
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "BIG.DAT") hit = fr;
    }, &running));
    EXPECT_EQ(hit.status, 0);
    EXPECT_LE(hit.confidence, 35);
    EXPECT_GT(hit.sizeBytes, hit.runs.size() ? hit.runs[0].sectorCount * 512 : 0u);
}

TEST(FatParser, ExFatSpecStreamEntryYieldsRealSize) {
    // Regression: the stream dentry must be recognized by its spec type
    // (0xC0) with FileNameLength at byte 3 and DataLength at 24..31.
    auto img = buildExFatWithGappedDeletedSet();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord hit{};
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "LOST.DAT") hit = fr;
    }, &running));
    ASSERT_FALSE(hit.runs.empty());
    EXPECT_EQ(hit.sizeBytes, 17u);
    EXPECT_EQ(hit.runs[0].startSector, 25u + 1u); // cluster 3 in heap starting at sector 25
}

TEST(FatParser, ZeroSectorsPerClusterReturnsCleanly) {
    // Regression: sectorsPerCluster == 0 used to divide by zero.
    auto img = byteback::testfix::buildFat16Volume();
    img[0x0D] = 0; // BPB_SecPerClus = 0
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::atomic<bool> running{true};
    FATParser fat;
    EXPECT_TRUE(fat.scan(reader, [](const FileRecord&) {}, &running));
}

namespace {

// FAT32 with a cyclic directory chain: FAT[2] = 3 and FAT[3] = 2.
std::vector<uint8_t> buildFat32CyclicRootChainVolume() {
    constexpr uint32_t ss = 512;
    constexpr uint32_t reserved = 1;
    constexpr uint32_t fatSectors = 2;
    constexpr uint32_t numFats = 1;
    // countOfClusters >= 65525 => FAT32. spc=1 keeps the image at ~32 MB.
    const uint32_t dataSectors = 65530;
    const uint32_t totalSectors = reserved + numFats * fatSectors + dataSectors;

    std::vector<uint8_t> img(static_cast<size_t>(totalSectors) * ss, 0);
    const uint32_t fatStart = reserved;
    const uint32_t dataStart = fatStart + numFats * fatSectors;

    img[0] = 0xEB; img[1] = 0x3C; img[2] = 0x90;
    writeLe16(img, 0x0B, static_cast<uint16_t>(ss));
    img[0x0D] = 1;               // sectors per cluster
    writeLe16(img, 0x0E, static_cast<uint16_t>(reserved));
    img[0x10] = static_cast<uint8_t>(numFats);
    writeLe16(img, 0x11, 0);     // rootEntryCount (FAT32: 0)
    writeLe16(img, 0x16, static_cast<uint16_t>(fatSectors)); // FAT size 16 (0 => 32)
    writeLe32(img, 0x20, totalSectors);
    writeLe32(img, 0x24, fatSectors);  // FAT size 32
    writeLe32(img, 0x2C, 2);           // root cluster
    std::memcpy(img.data() + 0x52, "FAT32   ", 8);

    // Cyclic chain: cluster 2 -> 3 -> 2.
    writeLe32(img, static_cast<size_t>(fatStart) * ss + 2 * 4, 3);
    writeLe32(img, static_cast<size_t>(fatStart) * ss + 3 * 4, 2);
    (void)dataStart;
    return img;
}

} // namespace

TEST(FatParser, CyclicFat32RootChainTerminates) {
    // Regression: a cyclic FAT chain used to spin the FAT32 directory walk
    // forever; the scan must return.
    auto img = buildFat32CyclicRootChainVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::atomic<bool> running{true};
    FATParser fat;
    EXPECT_TRUE(fat.scan(reader, [](const FileRecord&) {}, &running));
}
