// Golden recovery regression — programmatic fixtures (volume_fixtures.h).
// ctest -R GoldenRecovery
#include "scan_coordinator.h"
#include "byteback_db.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include "crypto/byteback_md5.h"
#include "recovery/path_util.h"
#include "fixtures/volume_fixtures.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace byteback;

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
        if (res.success) {
            ++stats.recovered;
        } else {
            std::fprintf(stderr, "[golden] recover fail name=%s source=%s err=%s val=%d %s\n",
                         rec.name.c_str(), rec.source.c_str(), res.error.c_str(),
                         res.validationScore, res.validationError.c_str());
        }
    }
    return stats;
}

} // namespace

class GoldenRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath_ = bytebackTestTemp("byteback_golden", ".db").string();
        dest_ = bytebackTestTemp("byteback_golden_out").string();
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

std::string md5Bytes(const std::vector<uint8_t>& b) {
    crypto::Md5 m;
    m.update(b.data(), b.size());
    return m.finalHex();
}

std::string md5File(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
    return md5Bytes(buf);
}

TEST_F(GoldenRecoveryTest, Fat32FiveDeletedJpegMd5) {
    const uint8_t tags[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    const auto jpeg0 = testfix::minimalValidJpeg(tags[0]);
    auto vol = testfix::buildFat32DeletedJpegVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(vol));

    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [&](const FileRecord& fr) {
        return fr.source == "fat" && fr.status == 0 && fr.sizeBytes == jpeg0.size();
    });
    EXPECT_EQ(stats.found, 5u);
    EXPECT_EQ(stats.recovered, 5u);

    std::set<std::string> want;
    for (uint8_t t : tags) want.insert(md5Bytes(testfix::minimalValidJpeg(t)));
    std::set<std::string> got;
    for (const auto& ent : std::filesystem::directory_iterator(dest_)) {
        if (ent.is_regular_file()) got.insert(md5File(ent.path()));
    }
    EXPECT_EQ(got, want);
}

TEST_F(GoldenRecoveryTest, Fat16DeletedFragmentedUsesBackupFatMd5) {
    const auto jpegA = testfix::minimalValidJpeg(0x11);
    auto vol = testfix::buildFat16DeletedFragmentedJpegVolume(true);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(vol));

    FileRecord hit{};
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.status == 0 && fr.name.find("HOTO1") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    ASSERT_FALSE(hit.name.empty());
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dest_);
    EXPECT_TRUE(res.success) << res.error << " " << res.validationError;

    const auto outPath = std::filesystem::path(dest_) / hit.name;
    ASSERT_TRUE(std::filesystem::exists(outPath));
    EXPECT_EQ(md5File(outPath), md5Bytes(jpegA));
}

TEST_F(GoldenRecoveryTest, Fat16DeletedFragmentedDoesNotSucceedWithPoisonGlue) {
    const auto jpegA = testfix::minimalValidJpeg(0x11);
    auto vol = testfix::buildFat16DeletedFragmentedJpegVolume(false);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(vol));

    FileRecord hit{};
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.status == 0 && fr.name.find("HOTO1") != std::string::npos) hit = fr;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    ASSERT_FALSE(hit.name.empty());
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dest_);
    const auto outPath = std::filesystem::path(dest_) / hit.name;
    if (res.success) {
        ASSERT_TRUE(std::filesystem::exists(outPath));
        EXPECT_EQ(md5File(outPath), md5Bytes(jpegA))
            << "contiguous undelete must not report success for a glue of cluster 2+3";
    } else {
        EXPECT_FALSE(res.success);
    }
}

TEST_F(GoldenRecoveryTest, ExFatDeletedPreservePathsMd5) {
    auto vol = testfix::buildExFatDeletedPreservePathsVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(vol));

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name == "shot.jpg" && fr.status == 0) hits.push_back(fr);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(safeRelativeDir(hits[0].path), "pics");

    const std::string dest = joinDestDir(dest_, safeRelativeDir(hits[0].path));
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hits[0], dest);
    EXPECT_TRUE(res.success) << res.error;

    const auto wantJpeg = testfix::minimalValidJpeg(0xAB, 20);
    const auto outPath = std::filesystem::path(dest) / "shot.jpg";
    ASSERT_TRUE(std::filesystem::exists(outPath));
    EXPECT_EQ(md5File(outPath), md5Bytes(wantJpeg));
}
