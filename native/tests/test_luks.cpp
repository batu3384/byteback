#include "crypto/byteback_sha256.h"
#include "fs/luks.h"
#include "fs/partition_scanner.h"
#include "io/volume_mapper_win.h"
#include "byteback_fs.h"
#include "byteback_io.h"
#include "scan_coordinator.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace byteback;

namespace {

std::string toHex(const uint8_t* p, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = kHex[p[i] >> 4];
        s[i * 2 + 1] = kHex[p[i] & 0x0f];
    }
    return s;
}

} // namespace

TEST(Sha256, Fips180Abc) {
    uint8_t out[32];
    const char abc[] = "abc";
    crypto::sha256(reinterpret_cast<const uint8_t*>(abc), 3, out);
    EXPECT_EQ(toHex(out, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HmacSha256, Rfc4231Case1) {
    uint8_t key[20];
    std::memset(key, 0x0b, 20);
    const char* data = "Hi There";
    uint8_t out[32];
    crypto::hmacSha256(key, 20, reinterpret_cast<const uint8_t*>(data), 8, out);
    EXPECT_EQ(toHex(out, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(Pbkdf2HmacSha256, PasswordSaltIter1Dk32) {
    const char* pass = "password";
    const char* salt = "salt";
    uint8_t out[32];
    crypto::pbkdf2HmacSha256(reinterpret_cast<const uint8_t*>(pass), 8,
                             reinterpret_cast<const uint8_t*>(salt), 4, 1, out, 32);
    EXPECT_EQ(toHex(out, 32), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
}

TEST(Luks, ProbeFindsLuksMagic) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret");
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Luks);
    EXPECT_STREQ(volumeFsKindLabel(VolumeFsKind::Luks), "luks");
}

TEST(Luks, WrongPasswordDoesNotUnlock) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret");
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    auto unlocked = unlockLuks1WithPassword(reader, "wrong");
    EXPECT_FALSE(unlocked.success);
    EXPECT_FALSE(unlocked.error.empty());
}

TEST(Luks, UnlockPasswordListsFat16TestTxt) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret");
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));

    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running, unlocked.payloadOffsetBytes));
    EXPECT_TRUE(found) << "LUKS payload must expose FAT16 TEST.TXT";
}

TEST(Luks, UnlockPasswordOnMbrPartition) {
    auto luks = wrapLuks1(testfix::buildFat16Volume(), "secret");
    auto disk = testfix::buildMbrDiskWithFatPartition(luks, 2048);
    disk[450] = 0x83;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));
    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running, unlocked.payloadOffsetBytes));
    EXPECT_TRUE(found) << "LUKS in MBR partition must expose FAT16 TEST.TXT";
}

TEST(Luks, UnlockPasswordOnGptPartition) {
    auto luks = wrapLuks1(testfix::buildFat16Volume(), "secret");
    constexpr uint32_t ss = 512;
    constexpr uint32_t partStart = 2048;
    const uint32_t partSectors = static_cast<uint32_t>((luks.size() + ss - 1) / ss);
    const uint32_t diskSectors = partStart + partSectors;
    std::vector<uint8_t> disk(diskSectors * ss, 0);
    std::memcpy(disk.data() + partStart * ss, luks.data(), luks.size());
    uint8_t* h = disk.data() + ss;
    std::memcpy(h, "EFI PART", 8);
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
    uint8_t* e = disk.data() + 2 * ss;
    uint32_t guid1 = 0xEBD0A0A2;
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = partStart, end = partStart + partSectors - 1;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));
    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running, unlocked.payloadOffsetBytes));
    EXPECT_TRUE(found) << "LUKS in GPT partition must expose FAT16 TEST.TXT";
}

TEST(Luks, UnlockPasswordOnApmPartition) {
    auto luks = wrapLuks1(testfix::buildFat16Volume(), "secret");
    auto disk = testfix::buildApmDiskWithFatPartition(luks, 64);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));
    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running, unlocked.payloadOffsetBytes));
    EXPECT_TRUE(found) << "LUKS in APM partition must expose FAT16 TEST.TXT";
}

TEST(ScanCoordinator, LuksDetectsHeader) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret");
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    bool luks = false;
    for (const auto& s : sources) if (s == "luks_detect") luks = true;
    EXPECT_TRUE(luks);
}

TEST(ScanCoordinator, LuksUnlockQuickScanFindsTestTxt) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret");
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));

    bool found = false;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    EXPECT_TRUE(found) << "unlocked LUKS quick scan must list FAT16 TEST.TXT";
}

TEST(Luks, UnlockPasswordAes256XtsListsFat16TestTxt) {
    auto img = wrapLuks1(testfix::buildFat16Volume(), "secret", 64);
    ASSERT_GE(img.size(), 112u);
    uint32_t kb = 0;
    std::memcpy(&kb, img.data() + 108, 4);
    kb = (kb >> 24) | ((kb >> 8) & 0xff00) | ((kb << 8) & 0xff0000) | (kb << 24);
    EXPECT_EQ(kb, 64u);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    auto unlocked = unlockLuks1WithPassword(reader, "secret");
    ASSERT_TRUE(unlocked.success) << unlocked.error;
    EXPECT_EQ(unlocked.masterKey.size(), 64u);
    ASSERT_TRUE(applyLuksMasterKey(reader, unlocked));
    bool found = false;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running, unlocked.payloadOffsetBytes));
    EXPECT_TRUE(found) << "LUKS AES-256-XTS payload must expose FAT16 TEST.TXT";
}
