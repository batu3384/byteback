#include "scan_coordinator.h"
#include "fs/virtual_raid.h"
#include "fs/ntfs_logfile.h"
#include "fs/ntfs_util.h"
#include "fs/partition_scanner.h"
#include "fs/xfs_parser.h"
#include "fs/hfs_catalog.h"
#include "fs/apfs_container.h"
#include "fs/refs_parser.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <filesystem>

using namespace byteback;

TEST(RaidScan, TagsSourceWhenRaidBackendActive) {
    FileRecord fr;
    fr.source = "ntfs_mft";
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, "raid_ntfs_mft");
}

TEST(RaidScan, DoesNotPrefixLogfileUnreadHonestySource) {
    FileRecord fr;
    fr.source = kNtfsLogfileUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kNtfsLogfileUnreadSource);
}

TEST(RaidScan, DoesNotPrefixUsnUnreadHonestySource) {
    FileRecord fr;
    fr.source = kUsnUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kUsnUnreadSource);
}

TEST(RaidScan, DoesNotPrefixMftUnreadHonestySource) {
    FileRecord fr;
    fr.source = kNtfsMftUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kNtfsMftUnreadSource);
}

TEST(RaidScan, DoesNotPrefixProbeUnreadHonestySource) {
    FileRecord fr;
    fr.source = kProbeUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kProbeUnreadSource);
}

TEST(RaidScan, DoesNotPrefixRefsPageUnreadHonestySource) {
    FileRecord fr;
    fr.source = kRefsPageUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kRefsPageUnreadSource);
}

TEST(RaidScan, DoesNotPrefixRefsSupbUnreadHonestySource) {
    FileRecord fr;
    fr.source = kRefsSupbUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kRefsSupbUnreadSource);
}

TEST(RaidScan, DoesNotPrefixApfsNxsbUnreadHonestySource) {
    FileRecord fr;
    fr.source = kApfsNxsbUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kApfsNxsbUnreadSource);
}

TEST(RaidScan, DoesNotPrefixHfsCatalogUnreadHonestySource) {
    FileRecord fr;
    fr.source = kHfsCatalogUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kHfsCatalogUnreadSource);
}

TEST(RaidScan, DoesNotPrefixXfsDirUnreadHonestySource) {
    FileRecord fr;
    fr.source = "xfs_dir_unread";
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, "xfs_dir_unread");
}

TEST(RaidScan, DoesNotPrefixXfsInodeUnreadHonestySource) {
    FileRecord fr;
    fr.source = kXfsInodeUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kXfsInodeUnreadSource);
}

TEST(RaidScan, DoesNotPrefixXfsSbUnreadHonestySource) {
    FileRecord fr;
    fr.source = kXfsSbUnreadSource;
    DiskReader reader;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0,
                                {std::vector<uint8_t>(65536, 0), std::vector<uint8_t>(65536, 0)},
                                65536));
    reader.setRaidBackend(raid);
    tagRaidScanSource(fr, reader);
    EXPECT_EQ(fr.source, kXfsSbUnreadSource);
}

TEST(RecoveryEngine, BatchRecoverMultipleFiles) {
    std::vector<uint8_t> img(512 * 10, 0);
    const char payload[] = "HELLO_BYTEBACK_RECOVERY";
    std::memcpy(img.data() + 512 * 2, payload, sizeof(payload));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "hello.txt";
    rec.sizeBytes = sizeof(payload) - 1;
    rec.startSector = 2;

    RecoveryEngine engine;
    auto summary = engine.recoverFilesBatch(reader, {rec, rec},
        (std::filesystem::temp_directory_path() / "byteback_batch_recover").string());
    EXPECT_EQ(summary.succeeded, 2);
    EXPECT_EQ(summary.failed, 0);
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "byteback_batch_recover");
}
