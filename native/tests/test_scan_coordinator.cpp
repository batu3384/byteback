#include "scan_coordinator.h"
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
