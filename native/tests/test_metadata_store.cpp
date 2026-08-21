#include "byteback_db.h"
#include "sqlite3.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstdio>
#include <thread>

using namespace byteback;

class MetadataStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() / "byteback_meta_test.db").string();
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

TEST_F(MetadataStoreTest, SearchFilesFindsByName) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord a;
    a.name = "fatura_ocak.pdf";
    a.path = "/docs/fatura_ocak.pdf";
    a.extension = "pdf";
    a.sizeBytes = 100;
    a.status = 0;
    store_.insertFile(scanId, a);

    FileRecord b;
    b.name = "rapor.docx";
    b.path = "/docs/rapor.docx";
    b.sizeBytes = 200;
    b.status = 0;
    store_.insertFile(scanId, b);

    auto hits = store_.searchFiles(scanId, "fatura", 0, 10, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].name, "fatura_ocak.pdf");
}

TEST_F(MetadataStoreTest, SearchFilesRegexMode) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "IMG_2024_001.JPG";
    r.path = "/photos";
    r.sizeBytes = 1;
    r.status = 0;
    store_.insertFile(scanId, r);

    auto hits = store_.searchFiles(scanId, R"(IMG_\d+)", 0, 10, true);
    ASSERT_EQ(hits.size(), 1u);
}

TEST_F(MetadataStoreTest, CorruptRunsJsonYieldsEmptyRuns) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "x.bin";
    r.sizeBytes = 1;
    r.status = 0;
    int64_t fileId = store_.insertFile(scanId, r);
    ASSERT_GT(fileId, 0);

    store_.close();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path_.c_str(), &db), SQLITE_OK);
    sqlite3_exec(db, "DROP TRIGGER IF EXISTS files_fts_au;", nullptr, nullptr, nullptr);
    std::string sql = "UPDATE files SET runs_json='[[oops]]' WHERE scan_id=" + std::to_string(scanId);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    ASSERT_EQ(rc, SQLITE_OK) << (errMsg ? errMsg : "unknown");
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_close(db);
    std::filesystem::remove(path_ + "-wal");
    std::filesystem::remove(path_ + "-shm");

    ASSERT_TRUE(store_.open(path_));
    auto files = store_.getFiles(scanId, 0, 1);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(files[0].runs.empty());
}

TEST_F(MetadataStoreTest, InsertFilesBatchFailsWhenClosed) {
    store_.close();
    FileRecord r;
    r.name = "x.bin";
    EXPECT_FALSE(store_.insertFilesBatch(1, {r}));
}

TEST_F(MetadataStoreTest, ResidentBlobRoundTrip) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "tiny.txt";
    r.sizeBytes = 4;
    r.source = "ntfs_mft";
    r.residentData = {'a', 'b', 'c', 'd'};
    int64_t id = store_.insertFile(scanId, r);
    ASSERT_GT(id, 0);
    auto loaded = store_.getFileById(id, scanId);
    ASSERT_EQ(loaded.residentData.size(), 4u);
    EXPECT_EQ(loaded.residentData[0], 'a');
    EXPECT_EQ(loaded.residentData[3], 'd');
}

TEST_F(MetadataStoreTest, GetFileByIdHonorsScanId) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "a.bin";
    r.sizeBytes = 1;
    int64_t id = store_.insertFile(scanId, r);
    ASSERT_GT(id, 0);
    EXPECT_EQ(store_.getFileById(id, scanId).name, "a.bin");
    EXPECT_EQ(store_.getFileById(id, scanId + 99).id, -1);
}

TEST_F(MetadataStoreTest, ScanCheckpointAndPartitionPersist) {
    int64_t scanId = store_.createScan(1, "deep", 1000);
    ASSERT_TRUE(store_.setScanPartition(scanId, 2048, 4096));
    ASSERT_TRUE(store_.updateScanCheckpoint(scanId, true, 512));

    ScanState st = store_.getScanState(scanId);
    EXPECT_EQ(st.partitionStartSector, 2048);
    EXPECT_EQ(st.partitionSizeSectors, 4096u);
    EXPECT_TRUE(st.metadataComplete);
    EXPECT_EQ(st.carveResumeSector, 512u);
}

TEST_F(MetadataStoreTest, IntegrityChecksumRoundTrip) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "refs.bin";
    r.sizeBytes = 64;
    r.source = "refs";
    r.integrityChecksum = 0xDEADBEEFCAFEBABEULL;
    int64_t id = store_.insertFile(scanId, r);
    ASSERT_GT(id, 0);
    auto loaded = store_.getFileById(id, scanId);
    EXPECT_EQ(loaded.integrityChecksum, r.integrityChecksum);
}

TEST_F(MetadataStoreTest, ConcurrentInsertAndReadDoesNotCrash) {
    int64_t scanId = store_.createScan(0, "deep", 100);
    ASSERT_GT(scanId, 0);
    std::thread writer([&] {
        for (int i = 0; i < 80; ++i) {
            FileRecord r;
            r.name = "n" + std::to_string(i);
            r.status = 1;
            r.sizeBytes = 1;
            store_.insertFile(scanId, r);
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 80; ++i) {
            (void)store_.getFileCount(scanId);
            (void)store_.getFiles(scanId, 0, 10);
        }
    });
    writer.join();
    reader.join();
    EXPECT_EQ(store_.getFileCount(scanId), 80);
}

TEST_F(MetadataStoreTest, DeletedFilterIsNotPageLocal) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);
    std::vector<FileRecord> batch;
    for (int i = 0; i < 600; ++i) {
        FileRecord r;
        r.name = "sys" + std::to_string(i) + ".dll";
        r.status = 1;
        r.category = "Executable";
        r.sizeBytes = 10;
        batch.push_back(r);
    }
    FileRecord gone;
    gone.name = "photo.jpg";
    gone.status = 0;
    gone.category = "Image";
    gone.sizeBytes = 99;
    batch.push_back(gone);
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    FileListFilter deleted;
    deleted.status = 0;
    EXPECT_EQ(store_.getFileCount(scanId, deleted), 1);
    auto page = store_.getFiles(scanId, 0, 10, deleted);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].name, "photo.jpg");

    FileListFilter images;
    images.category = "Image";
    EXPECT_EQ(store_.getFileCount(scanId, images), 1);

    FileListFilter byName;
    byName.query = "photo";
    byName.status = 0;
    auto hits = store_.getFiles(scanId, 0, 10, byName);
    ASSERT_EQ(hits.size(), 1u);
}

TEST_F(MetadataStoreTest, CarvedFilterAndDuplicateToggle) {
    int64_t scanId = store_.createScan(0, "deep", 100);
    ASSERT_GT(scanId, 0);
    FileRecord mft;
    mft.name = "a.jpg";
    mft.status = 0;
    mft.source = "ntfs_mft";
    FileRecord carved;
    carved.name = "carve.bin";
    carved.status = 0;
    carved.source = "carver";
    FileRecord dup;
    dup.name = "carve.bin";
    dup.status = 0;
    dup.source = "carver_duplicate";
    FileRecord disc;
    disc.name = "hint";
    disc.status = 0;
    disc.source = "ntfs_recycle_meta";
    ASSERT_TRUE(store_.insertFilesBatch(scanId, {mft, carved, dup, disc}));

    FileListFilter deleted;
    deleted.status = 0;
    deleted.includeDuplicates = false;
    deleted.includeDiscovery = false;
    EXPECT_EQ(store_.getFileCount(scanId, deleted), 2);

    FileListFilter carvedOnly;
    carvedOnly.sourceLike = "carver%";
    carvedOnly.includeDuplicates = false;
    carvedOnly.includeDiscovery = false;
    EXPECT_EQ(store_.getFileCount(scanId, carvedOnly), 1);
    auto page = store_.getFiles(scanId, 0, 10, carvedOnly);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].source, "carver");

    FileListFilter withDup = carvedOnly;
    withDup.includeDuplicates = true;
    EXPECT_EQ(store_.getFileCount(scanId, withDup), 2);
}

TEST_F(MetadataStoreTest, ReclaimOrphanRunningMarksPaused) {
    int64_t running = store_.createScan(0, "deep", 1000);
    int64_t done = store_.createScan(1, "quick", 100);
    ASSERT_TRUE(store_.completeScan(done, 1));
    EXPECT_EQ(store_.getScanState(running).status, 0);
    EXPECT_EQ(store_.reclaimOrphanRunningScans(), 1);
    EXPECT_EQ(store_.getScanState(running).status, 4);
    EXPECT_EQ(store_.getScanState(done).status, 1);
    EXPECT_EQ(store_.reclaimOrphanRunningScans(), 0);
}

TEST_F(MetadataStoreTest, MetadataCheckpointDoesNotPolluteCarveResume) {
    int64_t scanId = store_.createScan(0, "deep", 10'000);
    ASSERT_TRUE(store_.updateScanProgress(scanId, 5000));
    ASSERT_TRUE(store_.updateScanCheckpoint(scanId, false, 0));
    auto st = store_.getScanState(scanId);
    EXPECT_FALSE(st.metadataComplete);
    EXPECT_EQ(st.carveResumeSector, 0u);
    EXPECT_EQ(st.scannedSectors, 5000u);
}

TEST_F(MetadataStoreTest, ReclaimOrphanDeepWithoutMetadataStaysPaused) {
    int64_t deep = store_.createScan(0, "deep", 100);
    ASSERT_TRUE(store_.updateScanProgress(deep, 100));
    EXPECT_EQ(store_.reclaimOrphanRunningScans(), 1);
    EXPECT_EQ(store_.getScanState(deep).status, 4);
}

TEST_F(MetadataStoreTest, ReclaimOrphanRunningMarksCompleteWhenFull) {
    int64_t full = store_.createScan(0, "deep", 100);
    ASSERT_TRUE(store_.updateScanProgress(full, 100));
    ASSERT_TRUE(store_.updateScanCheckpoint(full, true, 0));
    EXPECT_EQ(store_.reclaimOrphanRunningScans(), 1);
    EXPECT_EQ(store_.getScanState(full).status, 1);
}

TEST_F(MetadataStoreTest, ClearAllScanDataRemovesScans) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord f;
    f.name = "a.txt";
    f.path = "/a.txt";
    ASSERT_TRUE(store_.insertFile(scanId, f));
    ASSERT_TRUE(store_.clearAllScanData());
    EXPECT_EQ(store_.getLatestScanId(), -1);
    EXPECT_EQ(store_.getLatestUsableScanId(), -1);
    EXPECT_EQ(store_.getFileCount(scanId), 0);
}

TEST_F(MetadataStoreTest, GetLatestUsableScanIdPrefersPausedOrComplete) {
    int64_t failed = store_.createScan(0, "quick", 10);
    ASSERT_TRUE(store_.completeScan(failed, 3));
    int64_t paused = store_.createScan(1, "deep", 100);
    ASSERT_TRUE(store_.completeScan(paused, 4));
    EXPECT_EQ(store_.getLatestUsableScanId(), paused);
}
