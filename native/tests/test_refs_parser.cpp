#include "fs/refs_parser.h"
#include "fs/refs_integrity.h"
#include "fs/partition_scanner.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include "recovery/validation.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace byteback;

namespace {

void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    if (off + 2 > img.size()) img.resize(off + 2, 0);
    img[off] = static_cast<uint8_t>(v);
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

void writeRefsBoot(std::vector<uint8_t>& img) {
    std::memcpy(img.data() + 3, "ReFS\x00\x00\x00\x00", 8);
    std::memcpy(img.data() + 16, "FSRS", 4);
    writeLe32(img, 32, 512);
    writeLe32(img, 36, 8); // 4096-byte clusters
    img[510] = 0x55;
    img[511] = 0xAA;
}

void embedUtf16Name(uint8_t* page, size_t off, const char* ascii) {
    size_t i = 0;
    for (; ascii[i] != '\0'; ++i) {
        page[off + i * 2] = static_cast<uint8_t>(ascii[i]);
        page[off + i * 2 + 1] = 0;
    }
    page[off + i * 2] = 0;
    page[off + i * 2 + 1] = 0;
}

} // namespace

TEST(RefsParser, ProbeAndListFileFromMetadataPage) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Refs);

    FileRecord found;
    bool sawFile = false;
    std::string volPath;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs_volume") volPath = fr.path;
        if (fr.name == "report.docx") {
            sawFile = true;
            found = fr;
        }
    }, &running));
    EXPECT_TRUE(sawFile);
    EXPECT_EQ(volPath, "/refs-probe/");
    EXPECT_EQ(found.source, "refs_volume");
    EXPECT_TRUE(found.residentData.empty());

    const auto dest = (std::filesystem::temp_directory_path() / "byteback_refs_nameonly_out").string();
    std::filesystem::remove_all(dest);
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, found, dest);
    EXPECT_FALSE(res.success);
    EXPECT_NE(res.error.find("discovery"), std::string::npos);
    std::filesystem::remove_all(dest);
}

TEST(RefsParser, RejectsNonRefsBoot) {
    std::vector<uint8_t> img(8192, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    uint32_t bps = 0;
    uint32_t spc = 0;
    EXPECT_FALSE(probeRefsBoot(img.data(), img.size(), bps, spc));
}

TEST(RefsParser, IntegrityResidentFileAndRecoverValidation) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);
    const uint64_t supbCrc = refsCrc64Ecma(supb, 48);
    writeLe64(img, superOff + 48, supbCrc);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "integrity.txt");
    const char payload[] = "refs integrity payload";
    const size_t payloadLen = sizeof(payload) - 1;
    const uint64_t fileCrc = refsCrc64Ecma(reinterpret_cast<const uint8_t*>(payload), payloadLen);
    writeLe64(img, cluster + 128, fileCrc);
    writeLe16(img, cluster + 136, static_cast<uint16_t>(payloadLen));
    std::memcpy(page + 144, payload, payloadLen);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord found;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs" && fr.name == "integrity.txt") found = fr;
    }, &running));
    ASSERT_EQ(found.name, "integrity.txt");
    EXPECT_EQ(found.confidence, 92);
    EXPECT_EQ(found.integrityChecksum, fileCrc);
    ASSERT_EQ(found.residentData.size(), payloadLen);

    const auto dest = (std::filesystem::temp_directory_path() / "byteback_refs_integrity_out").string();
    std::filesystem::remove_all(dest);
    std::filesystem::create_directories(dest);

    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, found, dest);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.validationScore, 90);

    std::filesystem::remove_all(dest);
}

TEST(RefsParser, SupbChecksumFailureLowersVolumeConfidence) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster, 0);
    writeRefsBoot(img);
    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe64(img, superOff + 48, 0xDEADBEEFCAFEBABEULL);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int volConf = -1;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs_volume") volConf = fr.confidence;
    }, &running));
    EXPECT_EQ(volConf, 35);
}

TEST(RefsParser, LinearProbeCapLabelsCappedPath) {
    g_refsLinearProbeClusters.store(2);
    struct Reset {
        ~Reset() { g_refsLinearProbeClusters.store(0); }
    } reset;

    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);
    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::string volPath;
    int volConf = -1;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs_volume") {
            volPath = fr.path;
            volConf = fr.confidence;
        }
    }, &running));
    EXPECT_EQ(volPath, "/refs-probe-capped/");
    EXPECT_EQ(volConf, 35);
}

TEST(RefsParser, UnreadSupbIsSentinelNotMissingVolume) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Cluster 30 = 30 * 4096 / 512 = sector 240, 8 sectors. Boot (sector 0)
    // stays readable so probeVolumeAt still says ReFS.
    reader.setMemoryFaultRange(240, 8);

    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Refs);

    bool sawFile = false;
    bool sawUnread = false;
    std::string volPath;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs_volume") volPath = fr.path;
        if (fr.name == "report.docx") sawFile = true;
        if (fr.source == kRefsSupbUnreadSource) sawUnread = true;
    }, &running));
    EXPECT_TRUE(sawFile) << "unread ReFS SUPB must not hide a readable metadata page at cluster 1";
    EXPECT_TRUE(sawUnread);
    EXPECT_FALSE(volPath.empty());
}

TEST(RefsParser, BackupSupbUsedWhenPrimaryWiped) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    const uint64_t backupOff = img.size() - cluster;
    std::memcpy(img.data() + backupOff, img.data() + superOff, cluster);
    std::memset(img.data() + superOff, 0, cluster);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawFile = false;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "report.docx") sawFile = true;
    }, &running));
    EXPECT_TRUE(sawFile) << "ReFS backup SUPB at last cluster must restore listing";
}

TEST(RefsParser, BackupSupbUsedWhenPrimaryAndLastWiped) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 3, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    const uint64_t lastOff = img.size() - cluster;
    const uint64_t last1Off = lastOff - cluster;
    std::memcpy(img.data() + last1Off, img.data() + superOff, cluster);
    std::memset(img.data() + superOff, 0, cluster);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawFile = false;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "report.docx") sawFile = true;
    }, &running));
    EXPECT_TRUE(sawFile) << "ReFS backup SUPB at last-1 cluster must restore listing when last is empty";
}

TEST(RefsParser, UnreadMetadataPageIsSentinelNotEmptyListing) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 2, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Cluster 1 = sector 8, 8 sectors. SUPB at cluster 30 stays readable.
    reader.setMemoryFaultRange(8, 8);

    bool sawFile = false;
    bool sawUnread = false;
    bool sawVolume = false;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "refs_volume" && fr.name == "ReFS_Volume") sawVolume = true;
        if (fr.name == "report.docx") sawFile = true;
        if (fr.source == kRefsPageUnreadSource) sawUnread = true;
    }, &running));
    EXPECT_TRUE(sawVolume);
    EXPECT_FALSE(sawFile) << "unread ministore page must not parse zeros as a missing report.docx";
    EXPECT_TRUE(sawUnread);
}

TEST(RefsParser, UnreadProbeDataClusterIsNotSentinelWhenPageParsed) {
    constexpr uint32_t cluster = 4096;
    constexpr uint64_t superOff = 30ull * cluster;
    std::vector<uint8_t> img(superOff + cluster * 3, 0);
    writeRefsBoot(img);

    uint8_t* supb = img.data() + superOff;
    std::memcpy(supb, "SUPB", 4);
    writeLe32(img, superOff + 32, 0);

    uint8_t* page = img.data() + cluster;
    page[0] = 0x30;
    page[1] = 0x00;
    page[2] = 0x01;
    page[3] = 0x00;
    embedUtf16Name(page, 4, "report.docx");

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Cluster 2 = sector 16. Cluster 1 (report.docx) stays readable.
    reader.setMemoryFaultRange(16, 8);

    bool sawFile = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    RefsParser refs;
    ASSERT_TRUE(refs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "report.docx") sawFile = true;
        if (fr.source == kRefsPageUnreadSource) sawUnread = true;
    }, &running));
    EXPECT_TRUE(sawFile);
    EXPECT_FALSE(sawUnread) << "unread non-metadata cluster after a parsed page is not an empty listing";
}
