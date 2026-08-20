// Golden recovery regression — programmatic fixtures (volume_fixtures.h).
// ctest -R GoldenRecovery
#include "scan_coordinator.h"
#include "wolf_db.h"
#include "wolf_io.h"
#include "wolf_recovery.h"
#include "fixtures/volume_fixtures.h"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>

using namespace wolf;

namespace {

struct GoldenStats {
    size_t found = 0;
    size_t recovered = 0;
};

GoldenStats runGoldenPipeline(DiskReader& reader, MetadataStore& store, const std::string& dest,
                              const char* scanType, const std::function<bool(const FileRecord&)>& pick) {
    GoldenStats stats;
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t totalSectors = reader.getDiskSize() / ss;
    const int64_t scanId = store.createScan(0, scanType, totalSectors);
    if (scanId <= 0) return stats;

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    auto onFound = [&](const FileRecord& fr) {
        if (!pick(fr)) return;
        hits.push_back(fr);
        store.insertFile(scanId, fr);
    };
    auto onProg = [&](uint64_t, uint64_t) {};

    if (std::string(scanType) == "quick") {
        runQuickScan(reader, onFound, onProg, &running, nullptr);
    } else if (std::string(scanType) == "deep") {
        runDeepScan(reader, onFound, onProg, &running, nullptr);
    } else {
        runFullCarveScan(reader, onFound, onProg, &running, nullptr);
    }

    stats.found = hits.size();
    RecoveryEngine engine;
    for (const auto& rec : hits) {
        auto res = engine.recoverFile(reader, rec, dest);
        if (res.success) ++stats.recovered;
    }
    return stats;
}

} // namespace

class GoldenRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath_ = (std::filesystem::temp_directory_path() / "wolf_golden.db").string();
        dest_ = (std::filesystem::temp_directory_path() / "wolf_golden_out").string();
        std::filesystem::remove(dbPath_);
        std::filesystem::remove_all(dest_);
        std::filesystem::create_directories(dest_);
        ASSERT_TRUE(store_.open(dbPath_));
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(dbPath_);
        std::filesystem::remove_all(dest_);
    }

    MetadataStore store_;
    std::string dbPath_;
    std::string dest_;
};

TEST_F(GoldenRecoveryTest, FatQuickFindAndRecover) {
    auto fatVol = testfix::buildFat16Volume();
    auto disk = testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [](const FileRecord& fr) {
        return fr.name.find("TEST") != std::string::npos;
    });
    EXPECT_GE(stats.found, 1u);
    EXPECT_EQ(stats.found, stats.recovered);
}

TEST_F(GoldenRecoveryTest, PngDeepCarveAndRecover) {
    auto disk = testfix::buildPngCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto stats = runGoldenPipeline(reader, store_, dest_, "deep", [](const FileRecord& fr) {
        return fr.source == "carver" && fr.category == "Image";
    });
    EXPECT_GE(stats.found, 1u);
    EXPECT_EQ(stats.found, stats.recovered);
}

TEST_F(GoldenRecoveryTest, Fat32StyleSecondVolume) {
    auto fatVol = testfix::buildFat16Volume();
    auto disk = testfix::buildMbrDiskWithFatPartition(fatVol, 4096);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    ScanBounds bounds;
    bounds.startSector = 4096;
    bounds.sizeInSectors = static_cast<uint64_t>(fatVol.size() / 512);

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) hits.push_back(fr);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, false, bounds);

    EXPECT_GE(hits.size(), 1u);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hits[0], dest_);
    EXPECT_TRUE(res.success) << res.error;
}

TEST_F(GoldenRecoveryTest, ReportsFindRecoverRatio) {
    auto fatVol = testfix::buildFat16Volume();
    auto disk = testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [](const FileRecord& fr) {
        return !fr.name.empty();
    });
    EXPECT_GE(stats.found, 1u);
    EXPECT_EQ(stats.recovered, stats.found);
    testing::Test::RecordProperty("golden_found", static_cast<int>(stats.found));
    testing::Test::RecordProperty("golden_recovered", static_cast<int>(stats.recovered));
}

TEST_F(GoldenRecoveryTest, NtfsDeletedResidentFindAndRecover) {
    auto disk = testfix::buildNtfsDeletedResidentVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [](const FileRecord& fr) {
        return fr.name == "doc.txt" && fr.status == 0;
    });
    EXPECT_GE(stats.found, 1u);
    EXPECT_EQ(stats.found, stats.recovered);

    std::ifstream in(dest_ + "/doc.txt", std::ios::binary);
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "hello");
}

TEST_F(GoldenRecoveryTest, NtfsDeletedNonResidentFindAndRecover) {
    auto disk = testfix::buildNtfsDeletedNonResidentVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [](const FileRecord& fr) {
        return fr.name == "doc.txt" && fr.status == 0;
    });
    EXPECT_GE(stats.found, 1u);
    EXPECT_EQ(stats.found, stats.recovered);

    std::ifstream in(dest_ + "/doc.txt", std::ios::binary);
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "hello nonres");
}
