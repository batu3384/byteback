#include "scan_coordinator.h"
#include "fs/virtual_raid.h"
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
