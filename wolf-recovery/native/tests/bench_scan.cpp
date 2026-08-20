#include "scan_coordinator.h"
#include "wolf_io.h"
#include "fixtures/volume_fixtures.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace wolf;

TEST(BenchScan, Sparse64MiBDeepScanTiming) {
    if (!std::getenv("WOLF_RUN_BENCH")) {
        GTEST_SKIP() << "Set WOLF_RUN_BENCH=1 to run scan benchmarks";
    }

    // ponytail: ~64 MiB memory volume, not sparse file on disk.
    constexpr uint64_t sectors = (64u * 1024u * 1024u) / 512u;
    std::vector<uint8_t> disk(sectors * 512, 0);
    auto png = testfix::buildPngCarveDisk();
    if (png.size() >= 8192) {
        std::memcpy(disk.data() + sectors * 512 - 8192, png.data() + 4096, 4096);
    }

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::atomic<bool> running{true};
    size_t found = 0;
    const auto t0 = std::chrono::steady_clock::now();
    runDeepScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver") ++found;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    testing::Test::RecordProperty("bench_ms", static_cast<int>(ms));
    testing::Test::RecordProperty("bench_found", static_cast<int>(found));
    EXPECT_GE(ms, 0);
}
