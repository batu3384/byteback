#include "scan_coordinator.h"
#include "scan_progress.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace byteback;

TEST(ScanCoordinator, QuickScanFindsFatOnMbrPartition) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    auto disk = byteback::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
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

TEST(ScanCoordinator, QuickScanPartitionScopeFindsFat) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    constexpr uint32_t partStart = 2048;
    auto disk = byteback::testfix::buildMbrDiskWithFatPartition(fatVol, partStart);
    uint32_t partSectors = static_cast<uint32_t>(fatVol.size() / 512);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    ScanBounds bounds;
    bounds.startSector = partStart;
    bounds.sizeInSectors = partSectors;

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, false, bounds);

    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, QuickScanWrongPartitionMissesFat) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    auto disk = byteback::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    ScanBounds bounds;
    bounds.startSector = 0;
    bounds.sizeInSectors = 2048;

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, false, bounds);

    for (const auto& n : names) {
        EXPECT_EQ(n.find("TEST"), std::string::npos);
    }
}

TEST(ScanCoordinator, BitLockerDetectsFveHeader) {
    std::vector<uint8_t> disk(512 * 8, 0);
    std::memcpy(disk.data() + 3, "-FVE-FS-", 8);
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

TEST(ScanCoordinator, BitLockerIgnoresFakeTenByteOem) {
    std::vector<uint8_t> disk(512 * 8, 0);
    std::memcpy(disk.data() + 3, "-FVEF-SYS-", 10);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    for (const auto& s : sources) EXPECT_NE(s, "bitlocker_detect");
}

TEST(ScanCoordinator, InvalidDriveStillCallsOnFinished) {
    ScanCoordinator coord;
    std::atomic<int> finished{-1};
    coord.startScan("not-a-number", "quick", [](const FileRecord&) {},
                    [](uint64_t, uint64_t) {}, nullptr, nullptr,
                    [&](int s) { finished = s; });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (finished.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(finished.load(), 3);
}

TEST(ScanCoordinator, StartScanTwiceJoinsPreviousThread) {
    ScanCoordinator coord;
    auto onFile = [](const FileRecord&) {};
    auto onProg = [](uint64_t, uint64_t) {};
    coord.startScan("not-a-number", "quick", onFile, onProg);
    coord.startScan("also-bad", "quick", onFile, onProg);
    coord.stopScan();
    SUCCEED();
}

TEST(ScanCoordinator, RequestStopReturnsWhileOnFinishedBlocked) {
    std::mutex gate;
    gate.lock();
    ScanCoordinator coord;
    std::atomic<bool> inFinish{false};
    auto onFinished = [&](int) {
        inFinish = true;
        std::lock_guard<std::mutex> hold(gate);
    };
    coord.startScan("not-a-number", "quick", [](const FileRecord&) {},
                    [](uint64_t, uint64_t) {}, nullptr, nullptr, onFinished);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!inFinish.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(inFinish.load());

    const auto t0 = std::chrono::steady_clock::now();
    coord.requestStop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));

    gate.unlock();
}

TEST(ScanCoordinator, StopScanFromFinishedCallbackDoesNotDeadlock) {
    ScanCoordinator coord;
    std::atomic<bool> finished{false};
    auto onFinished = [&](int) {
        coord.stopScan();
        finished = true;
    };
    coord.startScan("not-a-number", "quick", [](const FileRecord&) {},
                    [](uint64_t, uint64_t) {}, nullptr, nullptr, onFinished);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(finished.load());
}

TEST(ScanCoordinator, DeepScanCarvesPng) {
    auto img = byteback::testfix::buildPngCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, false);

    bool carved = false;
    for (const auto& s : sources) {
        if (s == "carver" || s == "carver_bgc") carved = true;
    }
    EXPECT_TRUE(carved);
}

TEST(ScanCoordinator, CarveScanResumesFromCheckpoint) {
    constexpr uint64_t kSectors = 200;
    std::vector<uint8_t> disk(kSectors * 512, 0);
    auto png = byteback::testfix::buildPngCarveDisk();
    const uint64_t pngSector = 120;
    std::memcpy(disk.data() + pngSector * 512, png.data() + 4096, 4096);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto countCarved = [&](uint64_t resumeAt) {
        size_t hits = 0;
        std::atomic<bool> running{true};
        runCarveScan(reader, [&](const FileRecord& fr) {
            if (fr.source == "carver" || fr.source == "carver_bgc") ++hits;
        }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, false, resumeAt);
        return hits;
    };

    EXPECT_GE(countCarved(110), 1u) << "PNG after resume checkpoint should be found";
    EXPECT_EQ(countCarved(kSectors - 1), 0u) << "resume past PNG should find nothing";
}

TEST(ScanProgress, MeterDoesNotRewind) {
    MonotonicMeter meter;
    EXPECT_EQ(meter.tick(10), 10u);
    EXPECT_EQ(meter.tick(3), 10u);
    EXPECT_EQ(meter.tick(12), 12u);
}

TEST(ScanProgress, MapWorkDoesNotOverflowOnMultiTbDisk) {
    const uint64_t sectors = 8ull << 30; // 4 TiB at 512-byte sectors
    const uint64_t half = mapWorkToBudget(sectors / 2, sectors, sectors);
    EXPECT_GT(half, sectors / 4);
    EXPECT_LT(half, sectors);
    const uint64_t metaBudget = sectors * 3 / 4;
    EXPECT_EQ(mulDivU64(sectors / 2, metaBudget, sectors), sectors * 3 / 8);
}

TEST(ScanCoordinator, QuickScanProgressNeverDecreasesOnJumpingRuns) {
    auto img = byteback::testfix::buildNtfsJumpingDataRunsVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    uint64_t last = 0;
    bool decreased = false;
    size_t ticks = 0;
    std::atomic<bool> running{true};
    runQuickScan(reader, [](const FileRecord&) {},
                 [&](uint64_t current, uint64_t) {
                     ++ticks;
                     if (current < last) decreased = true;
                     last = current;
                 },
                 &running, nullptr, false);

    EXPECT_GT(ticks, 0u);
    EXPECT_FALSE(decreased) << "progress used file startSector and rewound";
}

TEST(ScanCoordinator, QuickScanMetadataProgressHasMidTicks) {
    auto img = byteback::testfix::buildNtfsJumpingDataRunsVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    uint64_t last = 0;
    bool decreased = false;
    bool midPositive = false;
    size_t distinct = 0;
    uint64_t prev = 0;
    bool havePrev = false;
    std::atomic<bool> running{true};
    runQuickScan(reader, [](const FileRecord&) {},
                 [&](uint64_t current, uint64_t) {
                     if (current < last) decreased = true;
                     last = current;
                     if (current > 0) midPositive = true;
                     if (!havePrev || current != prev) {
                         ++distinct;
                         prev = current;
                         havePrev = true;
                     }
                 },
                 &running, nullptr, false);

    EXPECT_FALSE(decreased);
    EXPECT_TRUE(midPositive) << "metadata current stayed at 0 until complete";
    EXPECT_GE(distinct, 2u);
}
