#include "scan_coordinator.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace byteback;

TEST(BenchScan, Sparse64MiBDeepScanTiming) {
    if (!std::getenv("BYTEBACK_RUN_BENCH")) {
        GTEST_SKIP() << "Set BYTEBACK_RUN_BENCH=1 to run scan benchmarks";
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

// A2 measurement: parallel vs sequential carve over the SAME disjoint range
// list on a memory image (clonable reader -> real parallel FDs). Gated like
// the deep-scan bench. Record properties: seq_ms, par_ms, speedup, found.
TEST(BenchScan, ParallelCarveVsSequential) {
    if (!std::getenv("BYTEBACK_RUN_BENCH")) {
        GTEST_SKIP() << "Set BYTEBACK_RUN_BENCH=1 to run scan benchmarks";
    }

    constexpr size_t kDisk = 128u * 1024 * 1024;
    const auto png = testfix::buildMinimalValidPng();
    std::vector<uint8_t> img(kDisk, 0);
    // ~60 PNGs spread every 2 MiB so every range carries real work.
    for (size_t off = 1u << 20; off + 8192 < kDisk; off += 2u * (1u << 20)) {
        std::memcpy(img.data() + off, png.data(), png.size());
        std::memset(img.data() + off + png.size(), 0x5A, 4096);
    }

    std::vector<SectorRange> ranges;
    const uint64_t totalSectors = kDisk / 512;
    const int kParts = 16;
    const uint64_t per = totalSectors / kParts;
    for (int i = 0; i < kParts; ++i) ranges.push_back({i * per, per});

    auto run = [&](unsigned workers) {
        std::vector<uint8_t> copy = img;
        DiskReader reader;
        reader.attachMemoryVolume(std::move(copy));
        setParallelCarveWorkers(workers);
        std::atomic<bool> running{true};
        size_t found = 0;
        const auto t0 = std::chrono::steady_clock::now();
        runCarveScanRanges(reader, [&](const FileRecord& fr) {
            if (fr.id != -1 && !fr.name.empty()) ++found;
        }, [&](uint64_t, uint64_t) {}, &running, nullptr, ranges, 0, true);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        return std::pair<int64_t, size_t>{ms, found};
    };

    const auto seq = run(1);
    const auto par = run(4);
    EXPECT_EQ(seq.second, par.second);
    EXPECT_GT(seq.second, 0u);

    testing::Test::RecordProperty("seq_ms", static_cast<int>(seq.first));
    testing::Test::RecordProperty("par_ms", static_cast<int>(par.first));
    testing::Test::RecordProperty("found", static_cast<int>(seq.second));
    if (par.first > 0) {
        testing::Test::RecordProperty(
            "speedup_x100", static_cast<int>(seq.first * 100 / par.first));
    }
    // ponytail: relaxed — parallel must not be dramatically slower on any machine.
    EXPECT_LE(par.first, static_cast<int64_t>(seq.first * 2 + 100));
}
