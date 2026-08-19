#include "scan_coordinator.h"
#include "wolf_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace wolf;

TEST(ScanCoordinator, QuickScanFindsFatOnMbrPartition) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    auto disk = wolf::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, BitLockerDetectsFveHeader) {
    std::vector<uint8_t> disk(512 * 8, 0);
    std::memcpy(disk.data() + 3, "-FVEF-SYS-", 10);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    bool bitlocker = false;
    for (const auto& s : sources) if (s == "bitlocker_detect") bitlocker = true;
    EXPECT_TRUE(bitlocker);
}

TEST(ScanCoordinator, DeepScanCarvesPng) {
    auto img = wolf::testfix::buildPngCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    bool carved = false;
    for (const auto& s : sources) {
        if (s == "carver" || s == "carver_bgc") carved = true;
    }
    EXPECT_TRUE(carved);
}
