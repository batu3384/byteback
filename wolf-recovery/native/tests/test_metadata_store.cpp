#include "wolf_db.h"
#include "sqlite3.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstdio>

using namespace wolf;

class MetadataStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() / "wolf_meta_test.db").string();
        std::filesystem::remove(path_);
        ASSERT_TRUE(store_.open(path_));
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    MetadataStore store_;
    std::string path_;
};

TEST_F(MetadataStoreTest, FileRunsRoundTrip) {
    int64_t scanId = store_.createScan(0, "quick", 1000);
    ASSERT_GT(scanId, 0);

    FileRecord r;
    r.id = 0;
    r.parentId = -1;
    r.name = "photo.jpg";
    r.extension = "jpg";
    r.path = "/Users/x/photo.jpg";
    r.sizeBytes = 4096;
    r.startSector = 100;
    r.endSector = 108;
    r.status = 0;
    r.confidence = 90;
    r.category = "Image";
    r.source = "mft";
    r.runs = {{100, 4}, {200, 4}};

    int64_t fileId = store_.insertFile(scanId, r);
    ASSERT_GT(fileId, 0);

    auto page = store_.getFiles(scanId, 0, 10);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].name, "photo.jpg");
    ASSERT_EQ(page[0].runs.size(), 2u);
    EXPECT_EQ(page[0].runs[0].startSector, 100u);
    EXPECT_EQ(page[0].runs[1].sectorCount, 4u);
}

TEST_F(MetadataStoreTest, BatchInsertPreservesRuns) {
    int64_t scanId = store_.createScan(1, "deep", 5000);
    std::vector<FileRecord> batch(3);
    for (int i = 0; i < 3; ++i) {
        batch[i].name = "f" + std::to_string(i) + ".bin";
        batch[i].sizeBytes = 512;
        batch[i].runs = {{static_cast<uint64_t>(i * 10), 1}};
        batch[i].status = 0;
    }
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));
    EXPECT_EQ(store_.getFileCount(scanId), 3);

    auto files = store_.getFiles(scanId, 0, 10);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[1].runs.front().startSector, 10u);
}

TEST_F(MetadataStoreTest, ScanStateNamedColumns) {
    int64_t scanId = store_.createScan(2, "quick", 800);
    store_.updateScanProgress(scanId, 400);
    store_.incrementRecovered(scanId);

    ScanState st = store_.getScanState(scanId);
    EXPECT_EQ(st.id, scanId);
    EXPECT_EQ(st.driveIndex, 2);
    EXPECT_EQ(st.scanType, "quick");
    EXPECT_EQ(st.totalSectors, 800u);
    EXPECT_EQ(st.scannedSectors, 400u);
    EXPECT_EQ(st.recoveredFiles, 1);
    EXPECT_EQ(st.status, 0);
}

TEST_F(MetadataStoreTest, CorruptRunsJsonYieldsEmptyRuns) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "x.bin";
    r.sizeBytes = 1;
    r.status = 0;
    store_.insertFile(scanId, r);

    // Inject corrupt JSON directly.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path_.c_str(), &db), SQLITE_OK);
    std::string sql = "UPDATE files SET runs_json='[[oops]]' WHERE scan_id=" + std::to_string(scanId);
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);

    auto files = store_.getFiles(scanId, 0, 1);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(files[0].runs.empty());
}
