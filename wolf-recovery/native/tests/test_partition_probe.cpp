#include "fs/partition_scanner.h"
#include "wolf_io.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace wolf;

static std::vector<uint8_t> makeImage(size_t sectors, uint32_t sectorSize = 512) {
    return std::vector<uint8_t>(sectors * sectorSize, 0);
}

static void writeNtfsBoot(std::vector<uint8_t>& img, size_t sector = 0, uint32_t ss = 512) {
    std::memcpy(img.data() + sector * ss + 3, "NTFS    ", 8);
    img[sector * ss + 510] = 0x55; img[sector * ss + 511] = 0xAA;
}

static void writeFat32Boot(std::vector<uint8_t>& img, size_t sector = 0, uint32_t ss = 512) {
    std::memcpy(img.data() + sector * ss + 82, "FAT32   ", 8);
    img[sector * ss + 510] = 0x55; img[sector * ss + 511] = 0xAA;
}

static void writeExFatBoot(std::vector<uint8_t>& img, size_t sector = 0, uint32_t ss = 512) {
    std::memcpy(img.data() + sector * ss + 3, "EXFAT   ", 8);
    img[sector * ss + 510] = 0x55; img[sector * ss + 511] = 0xAA;
}

static void writeExt4Superblock(std::vector<uint8_t>& img, size_t partitionSector = 0, uint32_t ss = 512) {
    size_t off = partitionSector * ss + 1024 + 0x38;
    img[off] = 0x53;
    img[off + 1] = 0xEF;
}

TEST(PartitionProbe, NtfsAtOffset) {
    auto img = makeImage(64);
    writeNtfsBoot(img, 10);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 10 * 512, 512), VolumeFsKind::Ntfs);
}

TEST(PartitionProbe, Fat32AtOffset) {
    auto img = makeImage(64);
    writeFat32Boot(img, 5);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 5 * 512, 512), VolumeFsKind::Fat);
}

TEST(PartitionProbe, ExFatRequiresBootSignature) {
    auto img = makeImage(64);
    std::memcpy(img.data() + 3, "EXFAT   ", 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Unknown);

    auto img2 = makeImage(64);
    writeExFatBoot(img2, 0);
    DiskReader reader2;
    reader2.attachMemoryVolume(std::move(img2));
    EXPECT_EQ(probeVolumeAt(reader2, 0, 512), VolumeFsKind::ExFat);
}

TEST(PartitionProbe, Ext4Magic) {
    auto img = makeImage(128);
    writeExt4Superblock(img, 0);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Ext4);
}

TEST(PartitionProbe, ApfsNxsbMagic) {
    auto img = makeImage(16);
    std::memcpy(img.data() + 32, "NXSB", 4);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Apfs);
}

TEST(PartitionProbe, HfsPlusMagic) {
    auto img = makeImage(16);
    img[1024] = 0x48;
    img[1025] = 0x2B;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Hfs);
}

TEST(PartitionProbe, UnknownOnEmpty) {
    auto img = makeImage(8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Unknown);
}
