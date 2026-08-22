#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/ntfs_util.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) img[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

std::vector<uint8_t> buildNtfsMftCarveDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = 8;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t mft = 8 * ss;
    std::memcpy(img.data() + mft, "FILE", 4);
    writeLe16(img, mft + 0x14, 0x38);
    writeLe16(img, mft + 0x16, 0x01);
    writeLe32(img, mft + 0x18, 256);
    writeLe32(img, mft + 0x1C, 1024);

    size_t attr = mft + 0x38;
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
    for (size_t i = 0; i < nameLen; ++i) {
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    }

    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);

    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    writeLe32(img, attr + 4, 0);
    return img;
}

std::vector<uint8_t> buildNtfsBootMftWalkDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
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
    writeLe16(img, rec1 + 0x16, 0x01);
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

    const size_t orphan = 40 * ss;
    std::memcpy(img.data() + orphan, "FILE", 4);
    writeLe16(img, orphan + 0x14, 0x38);
    writeLe16(img, orphan + 0x16, 0x01);
    writeLe32(img, orphan + 0x18, 256);
    writeLe32(img, orphan + 0x1C, 1024);
    attr = orphan + 0x38;
    const char* oname = "orphan.bin";
    const size_t onameLen = 10;
    const size_t ofn = 66 + onameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + ofn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(ofn));
    writeLe16(img, attr + 20, 24);
    img[attr + 24 + 64] = static_cast<uint8_t>(onameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < onameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(oname[i]));
    attr += 16 + 8 + ofn;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

std::vector<uint8_t> buildNtfsFragmentedMftDisk() {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr size_t clusterBytes = ss * spc;
    constexpr uint32_t mftRecBytes = 1024;
    std::vector<uint8_t> img(ss * 256, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = clusterBytes;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, mftRecBytes);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes * 2);
    writeLe64(img, attr + 0x30, mftRecBytes * 8);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    img[attr + 0x43] = 0x11;
    img[attr + 0x44] = 0x01;
    img[attr + 0x45] = 0x13;
    img[attr + 0x46] = 0x00;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    auto writeMftFile = [&](size_t base, const char* name, uint64_t parent) {
        std::memcpy(img.data() + base, "FILE", 4);
        writeLe16(img, base + 0x14, 0x38);
        writeLe16(img, base + 0x16, 0x01);
        writeLe32(img, base + 0x18, 256);
        writeLe32(img, base + 0x1C, mftRecBytes);
        size_t a = base + 0x38;
        const size_t nameLen = std::strlen(name);
        const size_t fnValueLen = 66 + nameLen * 2;
        writeLe32(img, a + 0, 0x30);
        writeLe32(img, a + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
        writeLe32(img, a + 16, static_cast<uint32_t>(fnValueLen));
        writeLe16(img, a + 20, 24);
        writeLe64(img, a + 24, parent);
        img[a + 24 + 64] = static_cast<uint8_t>(nameLen);
        img[a + 24 + 65] = 1;
        for (size_t i = 0; i < nameLen; ++i)
            writeLe16(img, a + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
        a += 16 + 8 + fnValueLen;
        writeLe32(img, a + 0, 0xFFFFFFFF);
    };

    writeMftFile(rec0 + mftRecBytes, "doc.txt", 5);
    writeMftFile(20 * clusterBytes, "frag.txt", 5);
    return img;
}

std::vector<uint8_t> buildNtfsPathRebuildDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img = buildNtfsBootMftWalkDisk();
    const size_t rec2 = 8 * ss + 2 * 1024;
    std::memcpy(img.data() + rec2, "FILE", 4);
    writeLe16(img, rec2 + 0x14, 0x38);
    writeLe16(img, rec2 + 0x16, 0x01);
    writeLe32(img, rec2 + 0x18, 256);
    writeLe32(img, rec2 + 0x1C, 1024);
    writeLe16(img, rec2 + 0x1A, 0x02);
    size_t attr = rec2 + 0x38;
    const char* dir = "Users";
    const size_t dirLen = 5;
    const size_t fnLen = 66 + dirLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnLen));
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(dirLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < dirLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(dir[i]));
    attr += 16 + 8 + fnLen;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    const size_t rec3 = rec2 + 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    attr = rec3 + 0x38;
    const char* file = "bar.jpg";
    const size_t fileLen = 7;
    const size_t ffn = 66 + fileLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + ffn));
    writeLe32(img, attr + 16, static_cast<uint32_t>(ffn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 2);
    img[attr + 24 + 64] = static_cast<uint8_t>(fileLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < fileLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(file[i]));
    attr += 16 + 8 + ffn;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

std::vector<uint8_t> buildNtfsMftLogfileBoostDisk() {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr uint32_t mftSize = 1024;
    const size_t mftBase = spc * ss;
    const size_t userMft = mftBase + mftSize;
    const size_t logMft = mftBase + 2 * mftSize;

    std::vector<uint8_t> img(logMft + mftSize, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 44, 1);
    img[60] = static_cast<uint8_t>(0xF6);
    img[510] = 0x55;
    img[511] = 0xAA;

    // MFT record 1: lost.doc (matches LogFile hint)
    std::memcpy(img.data() + userMft, "FILE", 4);
    writeLe16(img, userMft + 0x14, 0x38);
    writeLe16(img, userMft + 0x16, 0x01);
    writeLe32(img, userMft + 0x18, 256);
    writeLe32(img, userMft + 0x1C, mftSize);
    size_t attr = userMft + 0x38;
    const char* name = "lost.doc";
    const size_t nameLen = 8;
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

    // MFT record 2: $LogFile with RCRD hint for lost.doc
    std::memcpy(img.data() + logMft, "FILE", 4);
    writeLe16(img, logMft + 0x14, 0x38);
    writeLe32(img, logMft + 0x18, 256);
    writeLe32(img, logMft + 0x1C, mftSize);
    std::vector<uint8_t> logPayload(128, 0);
    std::memcpy(logPayload.data(), "RCRD", 4);
    const uint16_t clientOff = 32;
    const uint16_t clientLen = 18;
    writeLe16(logPayload, 0x10, clientLen);
    writeLe16(logPayload, 0x12, clientOff);
    const wchar_t* wname = L"lost.doc";
    for (size_t i = 0; wname[i] != 0; ++i)
        writeLe16(logPayload, clientOff + i * 2, static_cast<uint16_t>(wname[i]));
    attr = logMft + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + logPayload.size()));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(logPayload.size()));
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, logPayload.data(), logPayload.size());
    attr += 16 + 8 + logPayload.size();
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

} // namespace

TEST(NtfsParser, CarvesMftFileRecord) {
    auto img = buildNtfsMftCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n == "doc.txt") found = true;
    }
    EXPECT_TRUE(found);
}

TEST(NtfsParser, ExtractsResidentDataBytes) {
    auto img = buildNtfsMftCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<uint8_t> payload;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") payload = fr.residentData;
    }, &running));

    ASSERT_EQ(payload.size(), 5u);
    EXPECT_EQ(std::string(payload.begin(), payload.end()), "hello");
}

TEST(NtfsParser, WalksMftFromBootLcnIgnoresOrphan) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, false));
    bool doc = false, orphan = false;
    for (const auto& n : names) {
        if (n == "doc.txt") doc = true;
        if (n == "orphan.bin") orphan = true;
    }
    EXPECT_TRUE(doc);
    EXPECT_FALSE(orphan);
}

TEST(NtfsParser, CarveOrphansFindsFileOutsideMftRuns) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, true));
    bool orphan = false;
    for (const auto& n : names) {
        if (n == "orphan.bin") orphan = true;
    }
    EXPECT_TRUE(orphan);
}

TEST(NtfsParser, FragmentedMftExtentFindsSecondRun) {
    auto img = buildNtfsFragmentedMftDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, false));
    bool doc = false, frag = false;
    for (const auto& n : names) {
        if (n == "doc.txt") doc = true;
        if (n == "frag.txt") frag = true;
    }
    EXPECT_TRUE(doc);
    EXPECT_TRUE(frag);
}

TEST(NtfsParser, RebuildsDeletedFilePath) {
    auto img = buildNtfsPathRebuildDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::string path;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "bar.jpg") path = fr.path;
    }, &running, 0, 0, false));
    EXPECT_EQ(path, "/Users/bar.jpg");
}

TEST(NtfsParser, LogfileHintBoostsMatchingMftRecord) {
    auto img = buildNtfsMftLogfileBoostDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord boosted{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "lost.doc") boosted = fr;
    }, &running));
    EXPECT_EQ(boosted.source, "ntfs_mft_logfile");
    EXPECT_GE(boosted.confidence, 12);
}

TEST(NtfsParser, IndexRootSlackSurvivesMftReuse) {
    auto img = byteback::testfix::buildNtfsIndexRootReuseVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) hits.push_back(fr);
    }, &running, 0, 0, false));

    bool alive = false, gone = false, goneIsI30 = false;
    for (const auto& fr : hits) {
        if (fr.name == "alive.bin" && fr.source == "ntfs_mft") alive = true;
        if (fr.name == "gone.txt") {
            gone = true;
            goneIsI30 = fr.source == "ntfs_i30" && fr.status == 0;
        }
    }
    EXPECT_TRUE(alive);
    EXPECT_TRUE(gone);
    EXPECT_TRUE(goneIsI30);
}

TEST(NtfsParser, OrphanIndxMagicDoesNotEmitI30) {
    auto img = buildNtfsBootMftWalkDisk();
    const size_t indx = 50 * 512;
    std::memcpy(img.data() + indx, "INDX", 4);
    img[indx + 0x18] = 16;
    img[indx + 0x1C] = 16;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool i30 = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_i30") i30 = true;
    }, &running, 0, 0, true));
    EXPECT_FALSE(i30);
}

TEST(NtfsParser, AdsSurvivesParentDedup) {
    auto img = byteback::testfix::buildNtfsDeletedResidentVolume();
    const size_t rec1 = 8 * 512 + 1024;
    const size_t fnValueLen = 66 + 7 * 2;
    size_t attr = rec1 + 0x38 + 16 + 8 + fnValueLen + 29;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 40);
    img[attr + 8] = 0;
    img[attr + 9] = 1;
    writeLe16(img, attr + 10, 24);
    writeLe32(img, attr + 16, 1);
    writeLe16(img, attr + 20, 26);
    img[attr + 24] = 'Z';
    img[attr + 25] = 0;
    img[attr + 26] = 'x';
    writeLe32(img, attr + 40, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool parent = false, ads = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") parent = true;
        if (fr.source == "ntfs_ads" && fr.name.find(":Z") != std::string::npos) ads = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(parent);
    EXPECT_TRUE(ads);
}
