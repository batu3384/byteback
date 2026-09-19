#include "fs/partition_scanner.h"
#include "fs/volume_identity.h"
#include "fixtures/volume_fixtures.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

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

static void writeRefsBoot(std::vector<uint8_t>& img, size_t sector = 0, uint32_t ss = 512) {
    std::memcpy(img.data() + sector * ss + 3, "ReFS\x00\x00\x00\x00", 8);
    std::memcpy(img.data() + sector * ss + 16, "FSRS", 4);
    img[sector * ss + 32] = 0x00; img[sector * ss + 33] = 0x02; // 512
    img[sector * ss + 36] = 8; // sectors per cluster
}

TEST(PartitionProbe, RefsAtOffset) {
    auto img = makeImage(64);
    writeRefsBoot(img, 2);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 2 * 512, 512), VolumeFsKind::Refs);
}

TEST(PartitionProbe, UnknownOnEmpty) {
    auto img = makeImage(8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Unknown);
}

TEST(PartitionProbe, Lvm2LabelAtPvStart) {
    auto img = makeImage(16);
    uint8_t* lab = img.data() + 512;
    std::memcpy(lab, "LABELONE", 8);
    uint64_t labSec = 1;
    std::memcpy(lab + 8, &labSec, 8);
    std::memcpy(lab + 24, "LVM2 001", 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Lvm);
}

TEST(PartitionProbe, FaultedBootIsUnreadNotUnknown) {
    auto img = makeImage(8);
    writeNtfsBoot(img, 0);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(0, 1);
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Unread);
}

TEST(PartitionProbe, PaddedBootIsUnreadNotUnknown) {
    // Past-EOF zero-pad used to look like a complete empty boot (Unknown).
    std::vector<uint8_t> img(256, 0);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Unread);
}

static void writeMbrNtfsEntry(std::vector<uint8_t>& img, uint32_t startLba, uint32_t sizeLba) {
    img[510] = 0x55;
    img[511] = 0xAA;
    img[0x1BE + 4] = 0x07;
    std::memcpy(img.data() + 0x1BE + 8, &startLba, 4);
    std::memcpy(img.data() + 0x1BE + 12, &sizeLba, 4);
}

TEST(PartitionScan, MbrZerosAreEmptyNotUnread) {
    auto img = makeImage(8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseMBR().empty());
    EXPECT_FALSE(scanner.tableUnread());
}

TEST(PartitionScan, MbrFaultIsUnreadNotMissingTable) {
    auto img = makeImage(64);
    writeMbrNtfsEntry(img, 2048, 100);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(0, 1);
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseMBR().empty());
    EXPECT_TRUE(scanner.tableUnread());
}

static void writeGptHeader(std::vector<uint8_t>& img, uint32_t ss = 512) {
    uint8_t* h = img.data() + ss;
    std::memcpy(h, "EFI PART", 8);
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
}

static void writeGptDataEntry(std::vector<uint8_t>& img, uint32_t ss = 512) {
    uint8_t* e = img.data() + 2 * ss;
    uint32_t guid1 = 0xEBD0A0A2;
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = 2048, end = 4095;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
}

TEST(PartitionScan, GptZerosAreEmptyNotUnread) {
    auto img = makeImage(64);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseGPT().empty());
    EXPECT_FALSE(scanner.tableUnread());
}

TEST(PartitionScan, GptHeaderFaultIsUnreadNotMissingTable) {
    auto img = makeImage(64);
    writeGptHeader(img);
    writeGptDataEntry(img);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(1, 1);
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseGPT().empty());
    EXPECT_TRUE(scanner.tableUnread());
}

TEST(PartitionScan, GptEntryPadIsUnreadNotEmptyGuids) {
    // Header fits; 128 GPT entries need 32 sectors from LBA 2 — 8-sector image pads.
    auto img = makeImage(8);
    writeGptHeader(img);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseGPT().empty());
    EXPECT_TRUE(scanner.tableUnread());
}

TEST(PartitionScan, GptEntryFaultKeepsCompletePrefixAndFlagsUnread) {
    auto img = makeImage(64);
    writeGptHeader(img);
    writeGptDataEntry(img);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(3, 1); // first entry sector (LBA 2) still readable
    PartitionScanner scanner(&reader);
    auto gpt = scanner.parseGPT();
    ASSERT_EQ(gpt.size(), 1u);
    EXPECT_EQ(gpt[0].startSector, 2048u);
    EXPECT_TRUE(scanner.tableUnread());
}

static void writeGptHeaderAt(std::vector<uint8_t>& img, uint64_t headerLba, uint64_t entryLba,
                             uint32_t num, uint32_t esize, uint32_t ss = 512) {
    uint8_t* h = img.data() + headerLba * ss;
    std::memcpy(h, "EFI PART", 8);
    std::memcpy(h + 24, &headerLba, 8); // MyLBA
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
}

TEST(PartitionScan, BackupGptUsedWhenPrimaryWiped) {
    // UEFI: backup header at last LBA; array immediately before it.
    // Primary LBA 1 stays zeros — TestDisk/R-Studio still list the volume.
    constexpr uint32_t ss = 512;
    auto img = makeImage(96);
    writeGptHeaderAt(img, 95, 94, 1, 128, ss);
    uint8_t* e = img.data() + 94 * ss;
    uint32_t guid1 = 0xEBD0A0A2;
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = 40, end = 50;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    auto gpt = scanner.parseGPT();
    ASSERT_EQ(gpt.size(), 1u);
    EXPECT_EQ(gpt[0].startSector, 40u);
    EXPECT_EQ(gpt[0].sizeInSectors, 11u);
    EXPECT_FALSE(scanner.tableUnread());
}

TEST(PartitionScan, BackupGptUsedWhenPrimaryUnread) {
    constexpr uint32_t ss = 512;
    auto img = makeImage(96);
    writeGptHeaderAt(img, 95, 94, 1, 128, ss);
    uint8_t* e = img.data() + 94 * ss;
    uint32_t guid1 = 0xEBD0A0A2;
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = 40, end = 50;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(1, 1); // primary LBA 1
    PartitionScanner scanner(&reader);
    auto gpt = scanner.parseGPT();
    ASSERT_EQ(gpt.size(), 1u);
    EXPECT_EQ(gpt[0].startSector, 40u);
    EXPECT_TRUE(scanner.tableUnread());
}

TEST(PartitionScan, FindsLvm2LabelOne) {
    auto img = makeImage(32);
    uint8_t* lab = img.data() + 9 * 512;
    std::memcpy(lab, "LABELONE", 8);
    uint64_t labSec = 1;
    std::memcpy(lab + 8, &labSec, 8);
    std::memcpy(lab + 24, "LVM2 001", 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    auto lost = scanner.scanForPartitions(1);
    bool hit = false;
    for (const auto& p : lost) {
        if (p.type == "LVM2 PV" && p.startSector == 8) hit = true;
    }
    EXPECT_TRUE(hit);
}

TEST(PartitionScan, GptLinuxLvmGuid) {
    auto img = makeImage(64);
    writeGptHeader(img);
    uint8_t* e = img.data() + 2 * 512;
    uint32_t guid1 = 0xE6D6D379; // Linux LVM GPT type (LE first dword)
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = 2048, end = 4095;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    PartitionScanner scanner(&reader);
    auto gpt = scanner.parseGPT();
    ASSERT_EQ(gpt.size(), 1u);
    EXPECT_EQ(gpt[0].type, "Linux LVM");
    EXPECT_EQ(gpt[0].startSector, 2048u);
}

TEST(VolumeIdentity, SerialSizeMismatchRejected) {
    VolumeIdentity ev;
    ev.serial = 0xAABBCCDD;
    ev.sizeBytes = 1000;
    EXPECT_TRUE(volumeIdentityMatches(ev, 0xAABBCCDD, 1000));
    EXPECT_TRUE(volumeIdentityMatches(ev, 0xAABBCCDD, 0));
    EXPECT_FALSE(volumeIdentityMatches(ev, 0xAABBCCDD, 2000));
    EXPECT_FALSE(volumeIdentityMatches(ev, 0x1, 1000));
}

// P0-2: the lost-partition whole-disk search must find a volume whose boot
// sector sits at an arbitrary aligned offset (TestDisk-style search).
TEST(PartitionScan, FindsVolumeAtOddAlignedOffset) {
    constexpr uint64_t kOffsetSector = 2048; // 1 MiB
    auto fat = byteback::testfix::buildFat16Volume();
    std::vector<uint8_t> disk((kOffsetSector + fat.size() / 512 + 64) * 512, 0);
    std::memcpy(disk.data() + kOffsetSector * 512, fat.data(), fat.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    PartitionScanner scanner(&reader);
    auto found = scanner.scanForPartitions(64); // 32 KiB steps for test speed
    ASSERT_FALSE(found.empty());
    bool hit = false;
    for (const auto& p : found) {
        if (p.startSector == kOffsetSector) hit = true;
    }
    EXPECT_TRUE(hit);
    EXPECT_FALSE(scanner.scanUnread());
}

TEST(PartitionScan, UnreadStepIsNotCleanEmptySearch) {
    constexpr uint64_t kOffsetSector = 2048;
    auto fat = byteback::testfix::buildFat16Volume();
    std::vector<uint8_t> disk((kOffsetSector + fat.size() / 512 + 64) * 512, 0);
    std::memcpy(disk.data() + kOffsetSector * 512, fat.data(), fat.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    reader.setMemoryFaultRange(0, 1); // first step unread; volume at 2048 stays readable
    PartitionScanner scanner(&reader);
    auto found = scanner.scanForPartitions(64);
    bool hit = false;
    for (const auto& p : found) {
        if (p.startSector == kOffsetSector) hit = true;
    }
    EXPECT_TRUE(hit) << "unread step 0 must not hide a readable FAT boot at 2048";
    EXPECT_TRUE(scanner.scanUnread());
}

TEST(PartitionScan, UnreadBootIsNotNoLostPartitions) {
    constexpr uint64_t kOffsetSector = 2048;
    auto fat = byteback::testfix::buildFat16Volume();
    std::vector<uint8_t> disk((kOffsetSector + fat.size() / 512 + 64) * 512, 0);
    std::memcpy(disk.data() + kOffsetSector * 512, fat.data(), fat.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    reader.setMemoryFaultRange(kOffsetSector, 1);
    PartitionScanner scanner(&reader);
    auto found = scanner.scanForPartitions(64);
    bool hit = false;
    for (const auto& p : found) {
        if (p.startSector == kOffsetSector) hit = true;
    }
    EXPECT_FALSE(hit) << "unread FAT boot must not parse zeros as a volume";
    EXPECT_TRUE(scanner.scanUnread())
        << "empty lost-partition list after unread I/O is not “no partitions”";
}

TEST(PartitionScan, UnreadExtSuperblockIsNotSilentSkip) {
    auto img = makeImage(128);
    writeExt4Superblock(img, 0);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(2, 1); // EXT magic at +1024; 512-byte step 0 still reads
    PartitionScanner scanner(&reader);
    auto found = scanner.scanForPartitions(64);
    bool hitExt = false;
    for (const auto& p : found) {
        if (p.type == "EXT") hitExt = true;
    }
    EXPECT_FALSE(hitExt) << "unread EXT superblock must not parse as no-EXT";
    EXPECT_TRUE(scanner.scanUnread());
}

TEST(SelectedPartition, PrefersGptAndRejectsOob) {
    PartitionInfo mbr{};
    mbr.startSector = 63;
    mbr.sizeInSectors = 100;
    PartitionInfo gpt0{};
    gpt0.startSector = 2048;
    gpt0.sizeInSectors = 4096;
    PartitionInfo gpt1{};
    gpt1.startSector = 8192;
    gpt1.sizeInSectors = 1024;
    const std::vector<PartitionInfo> m{mbr};
    const std::vector<PartitionInfo> g{gpt0, gpt1};
    const PartitionInfo* p = selectedPartition(m, g, 1);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->startSector, 8192u);
    EXPECT_EQ(p->sizeInSectors, 1024u);
    EXPECT_EQ(selectedPartition(m, g, 2), nullptr);
    EXPECT_EQ(selectedPartition(m, g, -1), nullptr);
    const PartitionInfo* mbrHit = selectedPartition(m, {}, 0);
    ASSERT_NE(mbrHit, nullptr);
    EXPECT_EQ(mbrHit->startSector, 63u);
    EXPECT_EQ(selectedPartition({}, {}, 0), nullptr);
}

TEST(PartitionProbe, ParseApmReadsHfsStart) {
    auto fatVol = testfix::buildFat16Volume();
    auto disk = testfix::buildApmDiskWithFatPartition(fatVol, 64);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    PartitionScanner scanner(&reader);
    EXPECT_TRUE(scanner.parseMBR().empty());
    EXPECT_TRUE(scanner.parseGPT().empty());
    auto apm = scanner.parseAPM();
    ASSERT_EQ(apm.size(), 1u);
    EXPECT_EQ(apm[0].startSector, 64u);
    EXPECT_EQ(apm[0].type, "Apple_HFS");
    EXPECT_EQ(probeVolumeAt(reader, 64ull * 512, 512), VolumeFsKind::Fat);
}

TEST(PartitionProbe, ParseApm2048ReadsHfsStart) {
    auto fatVol = testfix::buildFat16Volume();
    auto disk = testfix::buildApmDiskWithFatPartition(fatVol, 64, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    PartitionScanner scanner(&reader);
    auto apm = scanner.parseAPM();
    ASSERT_EQ(apm.size(), 1u);
    EXPECT_EQ(apm[0].startSector, 64u);
    EXPECT_EQ(apm[0].type, "Apple_HFS");
    EXPECT_EQ(probeVolumeAt(reader, 64ull * 512, 512), VolumeFsKind::Fat);
}
