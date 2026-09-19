#include "fs/ldm_parser.h"
#include "fs/partition_scanner.h"
#include "fs/raid_layout.h"
#include "fs/virtual_raid.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include "scan_coordinator.h"
#include "fixtures/volume_fixtures.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void wrBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void wrBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
void wrBe64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}

void putVnum(uint8_t* p, uint64_t v, uint8_t width) {
    p[0] = width;
    for (uint8_t i = 0; i < width; ++i)
        p[1 + i] = static_cast<uint8_t>(v >> (8 * (width - 1 - i)));
}

void plantPrt3Vblk(uint8_t* buf, uint64_t start, uint64_t size, uint64_t volOff = 0,
                   uint64_t diskId = 1, uint64_t parentId = 2) {
    std::memset(buf, 0, 128);
    std::memcpy(buf, "VBLK", 4);
    wrBe32(buf + 4, 1);
    wrBe32(buf + 8, 1);
    wrBe16(buf + 0x0C, 0);
    wrBe16(buf + 0x0E, 1);
    buf[0x12] = 0;
    buf[0x13] = 0x33;
    buf[0x18] = 1;
    buf[0x19] = 1;
    buf[0x1A] = 2;
    buf[0x1B] = 'P';
    buf[0x1C] = '1';
    constexpr int rName = 5;
    wrBe64(buf + 0x24 + rName, start);
    wrBe64(buf + 0x2C + rName, volOff);
    const uint8_t sw = (size > 0xff) ? 2 : 1;
    putVnum(buf + 0x34 + rName, size, sw);
    const int rSize = sw + rName + 1;
    const uint8_t pw = (parentId > 0xff) ? 2 : 1;
    putVnum(buf + 0x34 + rSize, parentId, pw);
    const int rParent = pw + rSize + 1;
    const uint8_t dw = (diskId > 0xff) ? 2 : 1;
    putVnum(buf + 0x34 + rParent, diskId, dw);
    const int rDisk = dw + rParent + 1;
    wrBe32(buf + 0x14, static_cast<uint32_t>(rDisk + 28));
}

void plantDsk3Vblk(uint8_t* buf, uint8_t objId, const char* guid) {
    std::memset(buf, 0, 128);
    std::memcpy(buf, "VBLK", 4);
    wrBe32(buf + 4, 1);
    wrBe32(buf + 8, 1);
    wrBe16(buf + 0x0C, 0);
    wrBe16(buf + 0x0E, 1);
    buf[0x12] = 0;
    buf[0x13] = 0x34;
    buf[0x18] = 1;
    buf[0x19] = objId;
    buf[0x1A] = 1;
    buf[0x1B] = 'D';
    constexpr int rName = 4;
    const size_t glen = std::strlen(guid);
    buf[0x18 + rName] = static_cast<uint8_t>(glen);
    std::memcpy(buf + 0x18 + rName + 1, guid, glen);
    wrBe32(buf + 0x14, static_cast<uint32_t>(rName + glen + 12));
}

void plantDsk4Vblk(uint8_t* buf, uint8_t objId, uint8_t guidByte) {
    std::memset(buf, 0, 128);
    std::memcpy(buf, "VBLK", 4);
    wrBe32(buf + 4, 1);
    wrBe32(buf + 8, 1);
    wrBe16(buf + 0x0C, 0);
    wrBe16(buf + 0x0E, 1);
    buf[0x12] = 0;
    buf[0x13] = 0x44;
    buf[0x18] = 1;
    buf[0x19] = objId;
    buf[0x1A] = 1;
    buf[0x1B] = 'D';
    constexpr int rName = 4;
    std::memset(buf + 0x18 + rName, guidByte, 16);
    wrBe32(buf + 0x14, static_cast<uint32_t>(rName + 16 + 45));
}

void plantCmp3Stripe(uint8_t* buf, uint8_t objId, uint8_t parentId, uint8_t children,
                     uint8_t chunkSectors, uint8_t type = 1) {
    std::memset(buf, 0, 128);
    std::memcpy(buf, "VBLK", 4);
    wrBe32(buf + 4, 1);
    wrBe32(buf + 8, 1);
    wrBe16(buf + 0x0C, 0);
    wrBe16(buf + 0x0E, 1);
    buf[0x12] = 0x10;
    buf[0x13] = 0x32;
    buf[0x18] = 1;
    buf[0x19] = objId;
    buf[0x1A] = 1;
    buf[0x1B] = 'C';
    buf[0x1C] = 1;
    buf[0x1D] = 'A';
    buf[0x1E] = type;
    buf[0x23] = 1;
    buf[0x24] = children;
    buf[0x35] = 1;
    buf[0x36] = parentId;
    buf[0x38] = 1;
    buf[0x39] = chunkSectors;
    buf[0x3A] = 1;
    buf[0x3B] = children;
    wrBe32(buf + 0x14, 36);
}

void plantPrivhead(uint8_t* sec, uint64_t pubStart, uint64_t pubSize, uint64_t cfgStart,
                   uint64_t cfgSize, const char* diskGuid = nullptr) {
    std::memset(sec, 0, 512);
    std::memcpy(sec, "PRIVHEAD", 8);
    wrBe16(sec + 0x0C, 2);
    wrBe16(sec + 0x0E, 12);
    if (diskGuid) {
        const size_t n = std::min(std::strlen(diskGuid), size_t{63});
        std::memcpy(sec + 0x30, diskGuid, n);
    }
    wrBe64(sec + 0x11B, pubStart);
    wrBe64(sec + 0x123, pubSize);
    wrBe64(sec + 0x12B, cfgStart);
    wrBe64(sec + 0x133, cfgSize);
}

std::vector<uint8_t> buildLdmFatDisk() {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t pub = 128;
    const uint64_t fatSec = (fat.size() + ss - 1) / ss;
    const uint64_t cfgSize = 2048;
    const uint64_t cfgStart = pub + fatSec;
    const uint64_t diskSec = cfgStart + cfgSize;
    std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[0x1BE + 4] = 0x42;
    uint32_t start = 1;
    uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
    std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
    std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
    plantPrivhead(disk.data() + 6 * ss, pub, fatSec, cfgStart, cfgSize);
    std::memcpy(disk.data() + pub * ss, fat.data(), fat.size());
    return disk;
}

} // namespace

TEST(Ldm, ParsePrivheadReadsPublicStart) {
    uint8_t sec[512];
    plantPrivhead(sec, 128, 400, 528, 2048);
    LdmPrivhead ph;
    ASSERT_TRUE(parseLdmPrivhead(sec, sizeof(sec), ph));
    EXPECT_EQ(ph.verMajor, 2);
    EXPECT_EQ(ph.verMinor, 12);
    EXPECT_EQ(ph.logicalDiskStart, 128u);
    EXPECT_EQ(ph.logicalDiskSize, 400u);
    EXPECT_EQ(ph.configStart, 528u);
    EXPECT_EQ(ph.configSize, 2048u);
}

TEST(Ldm, ParsePrivheadRejectsBadMagic) {
    uint8_t sec[512];
    plantPrivhead(sec, 128, 400, 528, 2048);
    sec[0] = 'X';
    LdmPrivhead ph;
    EXPECT_FALSE(parseLdmPrivhead(sec, sizeof(sec), ph));
}

TEST(Ldm, BackupPrivheadAtLastSector) {
    auto disk = buildLdmFatDisk();
    constexpr uint32_t ss = 512;
    std::memset(disk.data() + 6 * ss, 0, ss);
    const uint64_t last = disk.size() / ss - 1;
    auto fat = testfix::buildFat16Volume();
    const uint64_t pub = 128;
    const uint64_t fatSec = (fat.size() + ss - 1) / ss;
    const uint64_t cfgSize = 2048;
    const uint64_t cfgStart = pub + fatSec;
    plantPrivhead(disk.data() + last * ss, pub, fatSec, cfgStart, cfgSize);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    LdmPrivhead ph;
    ASSERT_TRUE(readLdmPrivhead(reader, ph));
    EXPECT_EQ(ph.logicalDiskStart, pub);
}

TEST(Ldm, GptTypeIsLdmData) {
    constexpr uint32_t ss = 512;
    constexpr uint32_t partStart = 64;
    std::vector<uint8_t> disk((partStart + 8) * ss, 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[ss] = 'E';
    disk[ss + 1] = 'F';
    disk[ss + 2] = 'I';
    disk[ss + 3] = ' ';
    disk[ss + 4] = 'P';
    disk[ss + 5] = 'A';
    disk[ss + 6] = 'R';
    disk[ss + 7] = 'T';
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(disk.data() + ss + 72, &entryLba, 8);
    std::memcpy(disk.data() + ss + 80, &num, 4);
    std::memcpy(disk.data() + ss + 84, &esize, 4);
    uint8_t* e = disk.data() + 2 * ss;
    uint32_t guid1 = 0xAF9B60A0;
    std::memcpy(e, &guid1, 4);
    uint64_t start = partStart, end = partStart + 7;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    PartitionScanner scan(&reader);
    auto gpt = scan.parseGPT();
    ASSERT_FALSE(gpt.empty());
    bool ldm = false;
    for (const auto& p : gpt)
        if (p.type.find("LDM") != std::string::npos) ldm = true;
    EXPECT_TRUE(ldm);
}

TEST(Ldm, MbrType42IsLdm) {
    auto disk = buildLdmFatDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    PartitionScanner scan(&reader);
    auto mbr = scan.parseMBR();
    ASSERT_FALSE(mbr.empty());
    bool ldm = false;
    for (const auto& p : mbr)
        if (p.type.find("LDM") != std::string::npos) ldm = true;
    EXPECT_TRUE(ldm);
}

TEST(ScanCoordinator, QuickScanFindsFatOnLdmPublicRegion) {
    auto disk = buildLdmFatDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    bool found = false;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    EXPECT_TRUE(found) << "LDM public region wrapping FAT16 must list TEST.TXT";
}

TEST(ScanCoordinator, QuickScanFindsFatOnLdmWhenBoundsSelectMbr42) {
    auto disk = buildLdmFatDisk();
    const uint64_t diskSec = disk.size() / 512;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    bool found = false;
    std::atomic<bool> running{true};
    ScanBounds bounds;
    bounds.startSector = 1;
    bounds.sizeInSectors = diskSec > 1 ? diskSec - 1 : 0;
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, true, bounds);
    EXPECT_TRUE(found) << "Selecting MBR 0x42 must still scan LDM public region";
}

TEST(Spaces, LooksLikeSpacedb) {
    uint8_t sec[512]{};
    std::memcpy(sec, "SPACEDB ", 8);
    EXPECT_TRUE(looksLikeSpacedb(sec, sizeof(sec)));
    sec[0] = 'X';
    EXPECT_FALSE(looksLikeSpacedb(sec, sizeof(sec)));
}

TEST(ScanCoordinator, SpacesDetectsHeader) {
    constexpr uint32_t ss = 512;
    constexpr uint32_t partStart = 64;
    std::vector<uint8_t> disk((partStart + 8) * ss, 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[ss] = 'E';
    disk[ss + 1] = 'F';
    disk[ss + 2] = 'I';
    disk[ss + 3] = ' ';
    disk[ss + 4] = 'P';
    disk[ss + 5] = 'A';
    disk[ss + 6] = 'R';
    disk[ss + 7] = 'T';
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(disk.data() + ss + 72, &entryLba, 8);
    std::memcpy(disk.data() + ss + 80, &num, 4);
    std::memcpy(disk.data() + ss + 84, &esize, 4);
    uint8_t* e = disk.data() + 2 * ss;
    uint32_t guid1 = 0xE75CAF8F;
    std::memcpy(e, &guid1, 4);
    uint64_t start = partStart, end = partStart + 7;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
    std::memcpy(disk.data() + partStart * ss, "SPACEDB ", 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    bool found = false;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "spaces_detect") found = true;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    EXPECT_TRUE(found) << "SPACEDB partition must emit spaces_detect";
}

TEST(Ldm, ParsePrt3ReadsStartSize) {
    uint8_t vblk[128];
    plantPrt3Vblk(vblk, 64, 400);
    LdmPrt3 p;
    ASSERT_TRUE(parseLdmPrt3(vblk, sizeof(vblk), p));
    EXPECT_EQ(p.start, 64u);
    EXPECT_EQ(p.size, 400u);
    EXPECT_EQ(p.volumeOffset, 0u);
    EXPECT_EQ(p.parentId, 2u);
    EXPECT_EQ(p.diskId, 1u);
}

TEST(ScanCoordinator, QuickScanFindsFatOnLdmPrt3Offset) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t pub = 128;
    constexpr uint64_t gap = 64;
    const uint64_t fatSec = (fat.size() + ss - 1) / ss;
    const uint64_t cfgSize = 2048;
    const uint64_t cfgStart = pub + gap + fatSec;
    const uint64_t diskSec = cfgStart + cfgSize;
    std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[0x1BE + 4] = 0x42;
    uint32_t start = 1;
    uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
    std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
    std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
    plantPrivhead(disk.data() + 6 * ss, pub, gap + fatSec, cfgStart, cfgSize);
    std::memcpy(disk.data() + (pub + gap) * ss, fat.data(), fat.size());
    uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
    std::memcpy(vmdb, "VMDB", 4);
    wrBe32(vmdb + 4, 8);
    wrBe32(vmdb + 8, 128);
    wrBe32(vmdb + 0x0C, 512);
    wrBe16(vmdb + 0x12, 4);
    wrBe16(vmdb + 0x14, 10);
    plantPrt3Vblk(disk.data() + (cfgStart + 18) * ss, gap, fatSec);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    bool found = false;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    EXPECT_TRUE(found) << "LDM PRT3 start offset must list FAT16 TEST.TXT";
}

// Same-disk spanned: boot+FAT+root in PRT3a, TEST.TXT cluster in PRT3b after a
// gap. Separate PRT3 scans either miss payload (gap zeros) or miss BPB.
std::vector<uint8_t> buildLdmSpannedFatDisk() {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t pub = 128;
    constexpr uint64_t gap = 64;
    constexpr uint64_t headSec = 34;
    const uint64_t fatSec = (fat.size() + ss - 1) / ss;
    const uint64_t tailSec = fatSec - headSec;
    const uint64_t tailStart = headSec + gap;
    const uint64_t cfgSize = 2048;
    const uint64_t cfgStart = pub + tailStart + tailSec;
    const uint64_t diskSec = cfgStart + cfgSize;
    std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[0x1BE + 4] = 0x42;
    uint32_t start = 1;
    uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
    std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
    std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
    plantPrivhead(disk.data() + 6 * ss, pub, tailStart + tailSec, cfgStart, cfgSize);
    std::memcpy(disk.data() + pub * ss, fat.data(), static_cast<size_t>(headSec * ss));
    std::memcpy(disk.data() + (pub + tailStart) * ss, fat.data() + headSec * ss,
                static_cast<size_t>(tailSec * ss));
    uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
    std::memcpy(vmdb, "VMDB", 4);
    wrBe32(vmdb + 4, 8);
    wrBe32(vmdb + 8, 128);
    wrBe32(vmdb + 0x0C, 512);
    wrBe16(vmdb + 0x12, 4);
    wrBe16(vmdb + 0x14, 10);
    uint8_t* vsec = disk.data() + (cfgStart + 18) * ss;
    plantPrt3Vblk(vsec, 0, headSec, 0);
    plantPrt3Vblk(vsec + 128, tailStart, tailSec, headSec);
    return disk;
}

std::vector<uint8_t> buildLdmDiskFatSlice(const uint8_t* slice, size_t sliceBytes, uint64_t volOff) {
    constexpr uint32_t ss = 512;
    constexpr uint64_t pub = 128;
    const uint64_t sliceSec = (sliceBytes + ss - 1) / ss;
    const uint64_t cfgSize = 2048;
    const uint64_t cfgStart = pub + sliceSec;
    const uint64_t diskSec = cfgStart + cfgSize;
    std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
    disk[510] = 0x55;
    disk[511] = 0xAA;
    disk[0x1BE + 4] = 0x42;
    uint32_t start = 1;
    uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
    std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
    std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
    plantPrivhead(disk.data() + 6 * ss, pub, sliceSec, cfgStart, cfgSize);
    std::memcpy(disk.data() + pub * ss, slice, sliceBytes);
    uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
    std::memcpy(vmdb, "VMDB", 4);
    wrBe32(vmdb + 4, 8);
    wrBe32(vmdb + 8, 128);
    wrBe32(vmdb + 0x0C, 512);
    wrBe16(vmdb + 0x12, 4);
    wrBe16(vmdb + 0x14, 10);
    plantPrt3Vblk(disk.data() + (cfgStart + 18) * ss, 0, sliceSec, volOff);
    return disk;
}

TEST(ScanCoordinator, QuickScanRecoversFatOnLdmSpannedPrt3) {
    auto disk = buildLdmSpannedFatDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty()) << "spanned LDM must list TEST.TXT";
    const auto dir = bytebackTestTemp("bb_ldm_span_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}

TEST(Ldm, AssemblesSpannedFromTwoDisksRecoversFat16) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t headSec = 34;
    const uint64_t fatSec = fat.size() / ss;
    ASSERT_GT(fatSec, headSec);
    auto imgA = buildLdmDiskFatSlice(fat.data(), static_cast<size_t>(headSec * ss), 0);
    auto imgB = buildLdmDiskFatSlice(fat.data() + headSec * ss,
                                     static_cast<size_t>((fatSec - headSec) * ss), headSec);
    DiskReader a, b;
    a.attachMemoryVolume(std::move(imgA));
    b.attachMemoryVolume(std::move(imgB));
    auto raid = assembleLdmFromOpenDisks({&a, &b});
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_ldm_2disk_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(lvR, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}

TEST(Ldm, AssembleLdmFromEvidencePathsRejectsDevice) {
    EXPECT_EQ(assembleLdmFromEvidencePaths({"\\\\.\\PhysicalDrive0", "C:\\cases\\disk.img"}), nullptr);
    EXPECT_EQ(assembleLdmFromEvidencePaths({"/dev/sda", "/tmp/a.img"}), nullptr);
}

TEST(Ldm, AssemblesSpannedFromTwoEvidenceFiles) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t headSec = 34;
    const uint64_t fatSec = fat.size() / ss;
    auto imgA = buildLdmDiskFatSlice(fat.data(), static_cast<size_t>(headSec * ss), 0);
    auto imgB = buildLdmDiskFatSlice(fat.data() + headSec * ss,
                                     static_cast<size_t>((fatSec - headSec) * ss), headSec);
    const auto p0 = bytebackTestTemp("bb_ldm_d0", ".img");
    const auto p1 = bytebackTestTemp("bb_ldm_d1", ".img");
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
    {
        std::ofstream a(p0, std::ios::binary | std::ios::trunc);
        a.write(reinterpret_cast<const char*>(imgA.data()), static_cast<std::streamsize>(imgA.size()));
        std::ofstream b(p1, std::ios::binary | std::ios::trunc);
        b.write(reinterpret_cast<const char*>(imgB.data()), static_cast<std::streamsize>(imgB.size()));
        ASSERT_TRUE(a.good());
        ASSERT_TRUE(b.good());
    }
    FileRecord hit;
    {
        auto raid = assembleLdmFromEvidencePaths({p0.u8string(), p1.u8string()});
        ASSERT_NE(raid, nullptr);
        DiskReader lvR;
        lvR.setRaidBackend(raid);
        std::atomic<bool> running{true};
        runQuickScan(lvR, [&](const FileRecord& fr) {
            if (fr.name.find("TEST") != std::string::npos) hit = fr;
        }, [&](uint64_t, uint64_t) {}, &running, nullptr);
        ASSERT_FALSE(hit.name.empty());
        const auto dir = bytebackTestTemp("bb_ldm_2disk_ev_dir");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        RecoveryEngine engine;
        auto res = engine.recoverFile(lvR, hit, dir.string());
        ASSERT_TRUE(res.success) << res.error;
        std::string got;
        {
            std::ifstream in(res.destPath, std::ios::binary);
            got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        EXPECT_EQ(got, std::string("Hello FAT16"));
        std::filesystem::remove_all(dir);
    }
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
}

TEST(Ldm, AssemblesReplicatedVblkBindsByDsk3Guid) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t headSec = 34;
    const uint64_t fatSec = fat.size() / ss;
    const uint64_t tailSec = fatSec - headSec;
    const char* guidA = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const char* guidB = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    auto makeDisk = [&](const uint8_t* slice, size_t sliceBytes, const char* guid) {
        constexpr uint64_t pub = 128;
        const uint64_t sliceSec = (sliceBytes + ss - 1) / ss;
        const uint64_t cfgSize = 2048;
        const uint64_t cfgStart = pub + sliceSec;
        const uint64_t diskSec = cfgStart + cfgSize;
        std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
        disk[510] = 0x55;
        disk[511] = 0xAA;
        disk[0x1BE + 4] = 0x42;
        uint32_t start = 1;
        uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
        std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
        std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
        plantPrivhead(disk.data() + 6 * ss, pub, sliceSec, cfgStart, cfgSize, guid);
        std::memcpy(disk.data() + pub * ss, slice, sliceBytes);
        uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
        std::memcpy(vmdb, "VMDB", 4);
        wrBe32(vmdb + 4, 8);
        wrBe32(vmdb + 8, 128);
        wrBe32(vmdb + 0x0C, 512);
        wrBe16(vmdb + 0x12, 4);
        wrBe16(vmdb + 0x14, 10);
        uint8_t* slot = disk.data() + (cfgStart + 18) * ss;
        plantDsk3Vblk(slot, 10, guidA);
        plantDsk3Vblk(slot + 128, 11, guidB);
        plantPrt3Vblk(slot + 256, 0, headSec, 0, 10);
        plantPrt3Vblk(slot + 384, 0, tailSec, headSec, 11);
        return disk;
    };
    auto imgA = makeDisk(fat.data(), static_cast<size_t>(headSec * ss), guidA);
    auto imgB = makeDisk(fat.data() + headSec * ss, static_cast<size_t>(tailSec * ss), guidB);
    DiskReader a, b;
    a.attachMemoryVolume(std::move(imgA));
    b.attachMemoryVolume(std::move(imgB));
    auto raid = assembleLdmFromOpenDisks({&a, &b});
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_ldm_repl_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(lvR, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}

TEST(Ldm, ParseCmp3ReadsStripeChunk) {
    uint8_t vblk[128];
    plantCmp3Stripe(vblk, 3, 2, 2, 128);
    LdmCmp3 c;
    ASSERT_TRUE(parseLdmCmp3(vblk, sizeof(vblk), c));
    EXPECT_EQ(c.objId, 3u);
    EXPECT_EQ(c.type, 1u);
    EXPECT_EQ(c.children, 2u);
    EXPECT_EQ(c.parentId, 2u);
    EXPECT_EQ(c.chunkSectors, 128u);
}

TEST(Ldm, AssemblesStripedFromTwoDisksRecoversFat16) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t stripeSec = 128;
    const size_t stripeB = static_cast<size_t>(stripeSec * ss);
    size_t stripes = (fat.size() + stripeB - 1) / stripeB;
    if (stripes % 2) ++stripes;
    std::vector<uint8_t> padded(stripes * stripeB, 0);
    std::memcpy(padded.data(), fat.data(), fat.size());
    std::vector<uint8_t> colA, colB;
    for (size_t i = 0; i < stripes; ++i) {
        auto& col = (i % 2 == 0) ? colA : colB;
        col.insert(col.end(), padded.begin() + i * stripeB, padded.begin() + (i + 1) * stripeB);
    }
    const char* guidA = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const char* guidB = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    const uint64_t colSec = colA.size() / ss;
    auto makeDisk = [&](const std::vector<uint8_t>& col, const char* guid) {
        constexpr uint64_t pub = 128;
        const uint64_t cfgSize = 2048;
        const uint64_t cfgStart = pub + colSec;
        const uint64_t diskSec = cfgStart + cfgSize;
        std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
        disk[510] = 0x55;
        disk[511] = 0xAA;
        disk[0x1BE + 4] = 0x42;
        uint32_t start = 1;
        uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
        std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
        std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
        plantPrivhead(disk.data() + 6 * ss, pub, colSec, cfgStart, cfgSize, guid);
        std::memcpy(disk.data() + pub * ss, col.data(), col.size());
        uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
        std::memcpy(vmdb, "VMDB", 4);
        wrBe32(vmdb + 4, 16);
        wrBe32(vmdb + 8, 128);
        wrBe32(vmdb + 0x0C, 512);
        wrBe16(vmdb + 0x12, 4);
        wrBe16(vmdb + 0x14, 10);
        uint8_t* slot = disk.data() + (cfgStart + 18) * ss;
        plantCmp3Stripe(slot, 3, 2, 2, static_cast<uint8_t>(stripeSec));
        plantDsk3Vblk(slot + 128, 10, guidA);
        plantDsk3Vblk(slot + 256, 11, guidB);
        plantPrt3Vblk(slot + 384, 0, colSec, 0, 10, 3);
        plantPrt3Vblk(slot + 512, 0, colSec, 1, 11, 3);
        return disk;
    };
    auto imgA = makeDisk(colA, guidA);
    auto imgB = makeDisk(colB, guidB);
    DiskReader a, b;
    a.attachMemoryVolume(std::move(imgA));
    b.attachMemoryVolume(std::move(imgB));
    auto raid = assembleLdmFromOpenDisks({&a, &b});
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_ldm_stripe_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(lvR, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}

TEST(Ldm, AssemblesRaid5FromThreeDisksRecoversFat16) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint32_t nDisks = 3;
    constexpr uint64_t stripeSec = 128;
    const size_t stripeB = static_cast<size_t>(stripeSec * ss);
    const uint32_t dataPer = nDisks - 1;
    size_t rows = (fat.size() + stripeB * dataPer - 1) / (stripeB * dataPer);
    std::vector<std::vector<uint8_t>> cols(nDisks);
    size_t srcOff = 0;
    for (size_t row = 0; row < rows; ++row) {
        const uint32_t p = raid_layout::raid5ParityDisk(row, nDisks);
        std::vector<std::vector<uint8_t>> stripe(nDisks, std::vector<uint8_t>(stripeB, 0));
        for (uint32_t b = 0; b < dataPer; ++b) {
            const uint32_t d = raid_layout::raid5DataDisk(row, b, nDisks);
            if (srcOff < fat.size()) {
                const size_t n = std::min(stripeB, fat.size() - srcOff);
                std::memcpy(stripe[d].data(), fat.data() + srcOff, n);
                srcOff += n;
            }
        }
        for (uint32_t d = 0; d < nDisks; ++d) {
            if (d == p) continue;
            for (size_t i = 0; i < stripeB; ++i) stripe[p][i] ^= stripe[d][i];
        }
        for (uint32_t d = 0; d < nDisks; ++d) {
            cols[d].insert(cols[d].end(), stripe[d].begin(), stripe[d].end());
        }
    }
    const char* guids[3] = {
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
        "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
        "cccccccc-cccc-cccc-cccc-cccccccccccc",
    };
    const uint64_t colSec = cols[0].size() / ss;
    auto makeDisk = [&](size_t idx) {
        constexpr uint64_t pub = 128;
        const uint64_t cfgSize = 2048;
        const uint64_t cfgStart = pub + colSec;
        const uint64_t diskSec = cfgStart + cfgSize;
        std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
        disk[510] = 0x55;
        disk[511] = 0xAA;
        disk[0x1BE + 4] = 0x42;
        uint32_t start = 1;
        uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
        std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
        std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
        plantPrivhead(disk.data() + 6 * ss, pub, colSec, cfgStart, cfgSize, guids[idx]);
        std::memcpy(disk.data() + pub * ss, cols[idx].data(), cols[idx].size());
        uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
        std::memcpy(vmdb, "VMDB", 4);
        wrBe32(vmdb + 4, 16);
        wrBe32(vmdb + 8, 128);
        wrBe32(vmdb + 0x0C, 512);
        wrBe16(vmdb + 0x12, 4);
        wrBe16(vmdb + 0x14, 10);
        uint8_t* slot = disk.data() + (cfgStart + 18) * ss;
        plantCmp3Stripe(slot, 3, 2, 3, static_cast<uint8_t>(stripeSec), 3);
        plantDsk3Vblk(slot + 128, 10, guids[0]);
        plantDsk3Vblk(slot + 256, 11, guids[1]);
        plantDsk3Vblk(slot + 384, 12, guids[2]);
        plantPrt3Vblk(slot + 512, 0, colSec, 0, 10, 3);
        plantPrt3Vblk(slot + 640, 0, colSec, 1, 11, 3);
        plantPrt3Vblk(slot + 768, 0, colSec, 2, 12, 3);
        return disk;
    };
    auto img0 = makeDisk(0);
    auto img1 = makeDisk(1);
    auto img2 = makeDisk(2);
    DiskReader a, b, c;
    a.attachMemoryVolume(std::move(img0));
    b.attachMemoryVolume(std::move(img1));
    c.attachMemoryVolume(std::move(img2));
    auto raid = assembleLdmFromOpenDisks({&a, &b, &c});
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_ldm_raid5_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(lvR, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}

TEST(Ldm, AssemblesReplicatedVblkBindsByDsk4Guid) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint64_t headSec = 34;
    const uint64_t fatSec = fat.size() / ss;
    const uint64_t tailSec = fatSec - headSec;
    const char* guidA = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const char* guidB = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    auto makeDisk = [&](const uint8_t* slice, size_t sliceBytes, const char* guid) {
        constexpr uint64_t pub = 128;
        const uint64_t sliceSec = (sliceBytes + ss - 1) / ss;
        const uint64_t cfgSize = 2048;
        const uint64_t cfgStart = pub + sliceSec;
        const uint64_t diskSec = cfgStart + cfgSize;
        std::vector<uint8_t> disk(static_cast<size_t>(diskSec * ss), 0);
        disk[510] = 0x55;
        disk[511] = 0xAA;
        disk[0x1BE + 4] = 0x42;
        uint32_t start = 1;
        uint32_t nsec = static_cast<uint32_t>(diskSec - 1);
        std::memcpy(disk.data() + 0x1BE + 8, &start, 4);
        std::memcpy(disk.data() + 0x1BE + 12, &nsec, 4);
        plantPrivhead(disk.data() + 6 * ss, pub, sliceSec, cfgStart, cfgSize, guid);
        std::memcpy(disk.data() + pub * ss, slice, sliceBytes);
        uint8_t* vmdb = disk.data() + (cfgStart + 17) * ss;
        std::memcpy(vmdb, "VMDB", 4);
        wrBe32(vmdb + 4, 8);
        wrBe32(vmdb + 8, 128);
        wrBe32(vmdb + 0x0C, 512);
        wrBe16(vmdb + 0x12, 4);
        wrBe16(vmdb + 0x14, 10);
        uint8_t* slot = disk.data() + (cfgStart + 18) * ss;
        plantDsk4Vblk(slot, 10, 0xAA);
        plantDsk4Vblk(slot + 128, 11, 0xBB);
        plantPrt3Vblk(slot + 256, 0, headSec, 0, 10);
        plantPrt3Vblk(slot + 384, 0, tailSec, headSec, 11);
        return disk;
    };
    auto imgA = makeDisk(fat.data(), static_cast<size_t>(headSec * ss), guidA);
    auto imgB = makeDisk(fat.data() + headSec * ss, static_cast<size_t>(tailSec * ss), guidB);
    DiskReader a, b;
    a.attachMemoryVolume(std::move(imgA));
    b.attachMemoryVolume(std::move(imgB));
    auto raid = assembleLdmFromOpenDisks({&a, &b});
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    FileRecord hit;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_ldm_dsk4_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(lvR, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("Hello FAT16"));
    std::filesystem::remove_all(dir);
}
