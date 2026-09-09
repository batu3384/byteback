#include "byteback_db.h"
#include "sqlite3.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <algorithm>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace byteback;

namespace {
// CA-054: per-process DB name — two concurrent byteback_tests.exe instances
// (multi-lane sweeps share the machine temp dir) used to fight over the fixed
// path: the second open hit a locked/corrupted SQLite file.
int testPid() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
} // namespace

class MetadataStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("byteback_meta_test_" + std::to_string(testPid()) + ".db"))
                    .string();
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

    FileListFilter deletedMetaOnly = deleted;
    deletedMetaOnly.sourceNotLike = "carver%";
    EXPECT_EQ(store_.getFileCount(scanId, deletedMetaOnly), 1);

    FileListFilter withDup = carvedOnly;
    withDup.includeDuplicates = true;
    EXPECT_EQ(store_.getFileCount(scanId, withDup), 2);

    auto summary = store_.getScanSummary(scanId);
    EXPECT_EQ(summary.deletedFiles, 1); // metadata deleted only — carve excluded
    EXPECT_EQ(summary.carvedFiles, 1);  // carver only; duplicate not counted as unique carve
    EXPECT_EQ(summary.totalFiles, 3);   // mft + carve + dup; discovery hidden
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

TEST_F(MetadataStoreTest, SizeAndDateFilters) {
    int64_t scanId = store_.createScan(0, "deep", 100);
    auto rec = [&](const char* name, uint64_t sz, int64_t mod) {
        FileRecord r; r.id = 0; r.parentId = -1; r.name = name;
        r.sizeBytes = sz; r.startSector = 10; r.endSector = 12;
        r.status = 0; r.confidence = 80; r.source = "mft";
        r.modifiedAt = mod; r.createdAt = mod;
        return r;
    };
    std::vector<FileRecord> batch = {
        rec("small_new.bin", 100, 1700000000),
        rec("big_new.bin", 9000000, 1700000100),
        rec("big_old.bin", 9000000, 1600000000),
    };
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    FileListFilter f;
    f.sizeMin = 1000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 2);

    f = FileListFilter{}; f.sizeMax = 1000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 1);

    f = FileListFilter{}; f.sizeMin = 1000; f.sizeMax = 10000000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 2);

    f = FileListFilter{}; f.dateFrom = 1650000000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 2);

    f = FileListFilter{}; f.dateTo = 1650000000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 1);

    // created_at fallback: zero modified_at must fall back to created_at.
    FileRecord fb = rec("fb.bin", 500, 0);
    fb.createdAt = 1700000500;
    ASSERT_TRUE(store_.insertFile(scanId, fb));
    f = FileListFilter{}; f.dateFrom = 1650000000;
    EXPECT_EQ(store_.getFileCount(scanId, f), 3);
}

// Negative LIMIT in SQLite means "no upper bound" — an IPC caller passing a
// wrapped negative (e.g. JS 3e9 -> Int32) would materialize the whole table.
TEST_F(MetadataStoreTest, GetFilesNegativeLimitReturnsNoRows) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    std::vector<FileRecord> batch(3);
    for (int i = 0; i < 3; ++i) {
        batch[i].name = "f" + std::to_string(i);
        batch[i].status = 0;
    }
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    EXPECT_TRUE(store_.getFiles(scanId, 0, -1).empty());
    EXPECT_EQ(store_.getFiles(scanId, -5, 10).size(), 3u); // negative offset clamps to 0
}

TEST_F(MetadataStoreTest, TimelineNegativeLimitReturnsNoRows) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    TimelineEvent ev;
    ev.eventType = "create";
    ev.fileName = "a.txt";
    ASSERT_GT(store_.insertTimelineEvent(scanId, ev), 0);
    ASSERT_GT(store_.insertTimelineEvent(scanId, ev), 0);

    EXPECT_TRUE(store_.getTimelineEvents(scanId, 0, -1).empty());
}

// Hunt: carve_only sets metadata_complete early (checkpoint fires as soon as
// carving starts). An orphaned carve_only scan mid-carve must stay paused, not
// be marked complete; one that actually reached total is complete.
TEST_F(MetadataStoreTest, CarveOnlyOrphanMidCarveStaysPaused) {
    int64_t mid = store_.createScan(0, "carve_only", 0);
    ASSERT_TRUE(store_.setScanTotalSectors(mid, 100));
    ASSERT_TRUE(store_.updateScanCheckpoint(mid, true, 40)); // carve started
    ASSERT_TRUE(store_.updateScanProgress(mid, 50));          // halfway

    int64_t done = store_.createScan(0, "carve_only", 0);
    ASSERT_TRUE(store_.setScanTotalSectors(done, 100));
    ASSERT_TRUE(store_.updateScanCheckpoint(done, true, 100));
    ASSERT_TRUE(store_.updateScanProgress(done, 100));

    EXPECT_EQ(store_.reclaimOrphanRunningScans(), 2);
    EXPECT_EQ(store_.getScanState(mid).status, 4);
    EXPECT_EQ(store_.getScanState(done).status, 1);
}

// Failed batch must not leave an open transaction that breaks the next batch.
TEST_F(MetadataStoreTest, BatchInsertStepFailureLeavesNoOpenTransaction) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "x.bin";
    r.status = 0;
    ASSERT_FALSE(store_.insertFilesBatch(scanId + 999, {r})); // FK violation

    FileRecord ok = r;
    ok.name = "ok.bin";
    ASSERT_TRUE(store_.insertFilesBatch(scanId, {ok}));
    EXPECT_EQ(store_.getFileCount(scanId), 1);
}

// Schema stamping: open() is idempotent and upgrades a v2 database in place.
TEST_F(MetadataStoreTest, OpenUpgradesV2AndIsIdempotent) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "keep.bin";
    r.status = 0;
    ASSERT_TRUE(store_.insertFile(scanId, r));
    store_.close();

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path_.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=2;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);

    for (int i = 0; i < 2; ++i) { // migrate once, reopen again: both must hold
        ASSERT_TRUE(store_.open(path_)) << "open pass " << i;
        EXPECT_EQ(store_.getFileCount(scanId), 1);
        EXPECT_EQ(store_.getScanState(scanId).status, 0);

        sqlite3* chk = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &chk), SQLITE_OK);
        sqlite3_stmt* ver = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(chk, "PRAGMA user_version;", -1, &ver, nullptr), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(ver), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(ver, 0), 3);
        sqlite3_finalize(ver);
        sqlite3_close(chk);
        store_.close();
    }
}

// FTS5 query building must tolerate embedded double quotes (graceful fallback,
// never a crash / never a full-table dump).
TEST_F(MetadataStoreTest, SearchFilesEmbeddedDoubleQuote) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord q;
    q.name = "say\"hi.txt";
    q.path = "/docs/say\"hi.txt";
    q.status = 0;
    ASSERT_TRUE(store_.insertFile(scanId, q));
    FileRecord plain;
    plain.name = "plain.txt";
    plain.path = "/docs/plain.txt";
    plain.status = 0;
    ASSERT_TRUE(store_.insertFile(scanId, plain));

    auto hits = store_.searchFiles(scanId, "say\"hi", 0, 10, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].name, "say\"hi.txt");

    // A lone quote is a legal substring: matches only the quoted name, no crash.
    auto lone = store_.searchFiles(scanId, "\"", 0, 10, false);
    ASSERT_EQ(lone.size(), 1u);
    EXPECT_EQ(lone[0].name, "say\"hi.txt");
    EXPECT_EQ(store_.searchFilesCount(scanId, "\"", false), 1);
}

// P0-6: content hash must survive a DB round trip.
TEST_F(MetadataStoreTest, ContentHashRoundTrip) {
    int64_t scanId = store_.createScan(0, "quick", 10);
    FileRecord r;
    r.name = "dup.bin";
    r.sizeBytes = 128;
    r.source = "carver";
    r.contentHash = "abc123";
    int64_t id = store_.insertFile(scanId, r);
    ASSERT_GT(id, 0);

    auto loaded = store_.getFileById(id, scanId);
    EXPECT_EQ(loaded.contentHash, "abc123");

    auto page = store_.getFiles(scanId, 0, 10);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].contentHash, "abc123");
}

// CA-036: the batched timeline insert must persist every event, in order,
// through one transaction — the per-record path paid a prepared statement
// per event and dominated scan finalize on journal-heavy volumes.
TEST_F(MetadataStoreTest, TimelineBatchInsertPreservesOrderAndContent) {
    int64_t scanId = store_.createScan(0, "deep", 1000);
    ASSERT_GT(scanId, 0);

    std::vector<TimelineEvent> events;
    events.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        TimelineEvent ev;
        ev.timestamp = 1600000000 + i; // increasing: ORDER BY == insertion order
        ev.eventType = (i % 2 == 0) ? "create" : "delete";
        ev.fileName = "file_" + std::to_string(i) + ".bin";
        ev.mftRef = static_cast<uint64_t>(i + 1);
        ev.source = "usn_journal";
        events.push_back(ev);
    }

    ASSERT_TRUE(store_.appendTimelineEventsBatch(scanId, events));
    EXPECT_EQ(store_.getTimelineEventCount(scanId), 5000);

    for (int off = 0; off < 5000; off += 1000) {
        auto page = store_.getTimelineEvents(scanId, off, 1000);
        ASSERT_EQ(page.size(), 1000u);
        for (int j = 0; j < 1000; ++j) {
            EXPECT_EQ(page[j].timestamp, 1600000000 + off + j);
            EXPECT_EQ(page[j].fileName, "file_" + std::to_string(off + j) + ".bin");
            EXPECT_EQ(page[j].eventType, ((off + j) % 2 == 0) ? "create" : "delete");
            EXPECT_EQ(page[j].mftRef, static_cast<uint64_t>(off + j + 1));
        }
    }

    // Empty batch is a successful no-op, matching insertFilesBatch.
    EXPECT_TRUE(store_.appendTimelineEventsBatch(scanId, {}));
}

// CA-030: path_asc / path_desc whitelist keys — case-insensitive full-path
// ordering; unknown keys keep falling back to the id default (injection-safe).
TEST_F(MetadataStoreTest, OrderByPathWhitelist) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);

    const char* paths[] = {"/docs/b.pdf", "/Alpha/a.txt", "/alpha/z.txt", "/docs/a.pdf"};
    for (int i = 0; i < 4; ++i) {
        FileRecord r;
        r.name = "f" + std::to_string(i);
        r.path = paths[i];
        r.sizeBytes = 10;
        r.status = 0;
        ASSERT_GT(store_.insertFile(scanId, r), 0);
    }

    FileListFilter f;
    f.orderBy = "path_asc";
    auto asc = store_.getFiles(scanId, 0, 10, f);
    ASSERT_EQ(asc.size(), 4u);
    // NOCASE: /Alpha/a.txt and /alpha/* interleave by folded bytes.
    EXPECT_EQ(asc[0].path, "/Alpha/a.txt");
    EXPECT_EQ(asc[1].path, "/alpha/z.txt");
    EXPECT_EQ(asc[2].path, "/docs/a.pdf");
    EXPECT_EQ(asc[3].path, "/docs/b.pdf");

    f.orderBy = "path_desc";
    auto desc = store_.getFiles(scanId, 0, 10, f);
    ASSERT_EQ(desc.size(), 4u);
    EXPECT_EQ(desc[0].path, "/docs/b.pdf");
    EXPECT_EQ(desc[1].path, "/docs/a.pdf");
    EXPECT_EQ(desc[2].path, "/alpha/z.txt");
    EXPECT_EQ(desc[3].path, "/Alpha/a.txt");

    // Non-whitelisted key: id fallback (stable), table untouched.
    f.orderBy = "path; DROP TABLE files";
    auto safe = store_.getFiles(scanId, 0, 10, f);
    ASSERT_EQ(safe.size(), 4u);
    for (size_t i = 1; i < safe.size(); ++i) {
        EXPECT_LE(safe[i - 1].id, safe[i].id);
    }
}

// CA-031: snippet fields are transient search output — insert paths must not
// persist them and read paths must not resurrect them.
TEST_F(MetadataStoreTest, SnippetIsTransientNeverPersisted) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);

    FileRecord r;
    r.name = "leak.txt";
    r.path = "/docs/leak.txt";
    r.sizeBytes = 10;
    r.status = 0;
    r.snippet = "should never be stored";
    r.snippetMatchStart = 3;
    r.snippetMatchEnd = 9;
    int64_t fileId = store_.insertFile(scanId, r);
    ASSERT_GT(fileId, 0);

    auto page = store_.getFiles(scanId, 0, 10);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_TRUE(page[0].snippet.empty());
    EXPECT_EQ(page[0].snippetMatchStart, -1);
    EXPECT_EQ(page[0].snippetMatchEnd, -1);

    auto byId = store_.getFileById(fileId);
    EXPECT_TRUE(byId.snippet.empty());
    EXPECT_EQ(byId.snippetMatchStart, -1);
    EXPECT_EQ(byId.snippetMatchEnd, -1);
}

// FAZ 1.2 keyset pagination: a cursor-driven page must be byte-identical to
// the same OFFSET page for every whitelist sort key in both directions —
// including 100-rows-per-value duplicate boundaries where the id tiebreaker
// decides the page edge (no skips, no duplicates).
TEST_F(MetadataStoreTest, KeysetCursorPagesMatchOffsetPages) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);

    constexpr int kRows = 10000;
    constexpr int kPage = 500;
    {
        std::vector<FileRecord> batch;
        batch.reserve(1000);
        for (int i = 0; i < kRows; ++i) {
            FileRecord r;
            // Case-flipped prefixes interleave under COLLATE NOCASE.
            r.name = (i % 2 ? "File_" : "file_") + std::to_string(100000 + i) + ".bin";
            r.path = (i % 2 ? "/Docs" : "/docs") + std::to_string(i % 50) + "/f" + std::to_string(i) + ".bin";
            r.sizeBytes = static_cast<uint64_t>((i % 37) * 1024);
            r.confidence = i % 100;                    // 100 rows share each value
            r.createdAt = 1600000000 + (i % 500);
            r.modifiedAt = r.createdAt + (i % 3) - 1;  // CASE flips both ways
            r.status = 0;
            batch.push_back(r);
            if (static_cast<int>(batch.size()) == 1000) {
                ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));
                batch.clear();
            }
        }
        ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));
    }
    ASSERT_EQ(store_.getFileCount(scanId), kRows);

    // Renderer-side cursorValueFor parity: the native date CASE.
    auto dateKey = [](const FileRecord& r) {
        return r.modifiedAt > r.createdAt ? r.modifiedAt : r.createdAt;
    };

    const char* keys[] = {"confidence_asc", "confidence_desc", "size_asc", "size_desc",
                          "name_asc", "name_desc", "path_asc", "path_desc",
                          "date_asc", "date_desc"};
    for (const char* key : keys) {
        const std::string k(key);
        const bool textKey = k.rfind("name", 0) == 0 || k.rfind("path", 0) == 0;
        FileListFilter f;
        f.orderBy = k;
        int64_t lastV = 0;
        std::string lastText;
        int64_t lastId = 0;
        for (int off = 0; off < kRows; off += kPage) {
            auto ref = store_.getFiles(scanId, off, kPage, f);
            ASSERT_EQ(ref.size(), static_cast<size_t>(kPage)) << k << " offset " << off;
            FileListFilter cf = f;
            if (off > 0) {
                cf.hasCursor = true;
                cf.cursorId = lastId;
                cf.cursorV = lastV;
                cf.cursorText = lastText;
            }
            // Offset still travels but must be IGNORED while the cursor applies.
            auto page = store_.getFiles(scanId, off, kPage, cf);
            ASSERT_EQ(page.size(), ref.size()) << k << " cursor page " << off / kPage;
            for (size_t j = 0; j < ref.size(); ++j) {
                ASSERT_EQ(page[j].id, ref[j].id)
                    << k << " page " << off / kPage << " row " << j;
            }
            const FileRecord& last = ref.back();
            lastId = last.id;
            if (textKey) {
                lastText = k.rfind("name", 0) == 0 ? last.name : last.path;
                lastV = 0;
            } else if (k.rfind("date", 0) == 0) {
                lastV = dateKey(last);
            } else if (k.rfind("confidence", 0) == 0) {
                lastV = last.confidence;
            } else {
                lastV = static_cast<int64_t>(last.sizeBytes);
            }
        }
    }
}

// Keyset boundary with a single shared sort value: every row has the same
// confidence, so every page edge is decided by the id tiebreaker alone.
TEST_F(MetadataStoreTest, KeysetCursorTiebreakerSameValue) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);

    std::vector<FileRecord> batch(10);
    for (int i = 0; i < 10; ++i) {
        batch[static_cast<size_t>(i)].name = "same_" + std::to_string(i) + ".bin";
        batch[static_cast<size_t>(i)].confidence = 50;
        batch[static_cast<size_t>(i)].status = 0;
    }
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    FileListFilter f;
    f.orderBy = "confidence_desc";
    std::vector<int64_t> seen;
    int64_t lastId = 0;
    bool haveCursor = false;
    for (int guard = 0; guard < 10; ++guard) {
        FileListFilter cf = f;
        cf.hasCursor = haveCursor;
        cf.cursorId = lastId;
        cf.cursorV = 50;
        auto page = store_.getFiles(scanId, 0, 3, cf);
        if (page.empty()) break;
        // Full pages except the last one (10 rows / 3 per page → 3,3,3,1).
        ASSERT_EQ(page.size(), std::min<size_t>(3, 10 - seen.size()));
        for (const auto& r : page) seen.push_back(r.id);
        lastId = page.back().id;
        haveCursor = true;
    }
    ASSERT_EQ(seen.size(), 10u);
    for (size_t i = 1; i < seen.size(); ++i) {
        EXPECT_LT(seen[i - 1], seen[i]) << "duplicate or skipped id at " << i;
    }
}

// Unknown / default sort keys are not keyset-capable: the cursor must be
// ignored and the OFFSET path used verbatim.
TEST_F(MetadataStoreTest, KeysetCursorUnknownKeyFallsBackToOffset) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    ASSERT_GT(scanId, 0);

    std::vector<FileRecord> batch(7);
    for (int i = 0; i < 7; ++i) {
        batch[static_cast<size_t>(i)].name = "f" + std::to_string(i) + ".bin";
        batch[static_cast<size_t>(i)].confidence = i;
        batch[static_cast<size_t>(i)].status = 0;
    }
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    const char* keys[] = {"", "id", "bogus_desc", "confidence; DROP TABLE files"};
    for (const char* key : keys) {
        FileListFilter cf;
        cf.orderBy = key;
        cf.hasCursor = true;
        cf.cursorId = 3;
        cf.cursorV = 999999;
        auto page = store_.getFiles(scanId, 2, 2, cf);
        FileListFilter ref;
        ref.orderBy = key;
        auto expected = store_.getFiles(scanId, 2, 2, ref);
        ASSERT_EQ(page.size(), expected.size()) << "key=" << key;
        for (size_t j = 0; j < expected.size(); ++j) {
            EXPECT_EQ(page[j].id, expected[j].id) << "key=" << key;
        }
    }
}

// ---- FAZ 1.3c: streaming CSV export ----

namespace {
std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Non-ASCII content MUST be written as hex-escape byte literals: MSVC without
// /utf-8 runs a codepage conversion on non-ASCII source literals (u8 included)
// and double-encodes them. Hex escapes are byte-exact on every toolchain, so
// the assertions below prove real UTF-8 passthrough (renderer parity) instead
// of compiler-codepage accidents. '—' = E2 80 94, 'şğüöçı' = the escapes below.
const std::string kEmDash = "\xE2\x80\x94";
const std::string kTurkish = "\xC5\x9F\xC4\x9F\xC3\xBC\xC3\xB6\xC3\xA7\xC4\xB1"; // şğüöçı

// Renderer-parity header labels (csv.* keys, tr locale).
std::vector<std::string> csvHeader() {
    return {"Ad", "Boyut (bayt)", "Kategori", "G\xC3\xBCven", "Durum", "Yol", "Kaynak",
            "Ba\xC5\x9Flang\xC4\xB1\xC3\xA7 Sekt\xC3\xB6r\xC3\xBC", "Olu\xC5\x9Fturma", "De\xC4\x9Fi\xC5\x9Ftirme"};
}
} // namespace

TEST_F(MetadataStoreTest, ExportCsvHeaderQuotingAndDates) {
    const int64_t scanId = store_.createScan(0, "deep", 100);
    ASSERT_GT(scanId, 0);

    auto mk = [&](const std::string& name, const std::string& source, int status,
                  int64_t createdAt, int64_t modifiedAt) {
        FileRecord r;
        r.name = name;
        r.path = "/docs/" + name;
        r.sizeBytes = 123;
        r.status = status;
        r.confidence = 90;
        r.category = "Document";
        r.source = source;
        r.startSector = 10;
        r.createdAt = createdAt;
        r.modifiedAt = modifiedAt;
        return r;
    };
    ASSERT_TRUE(store_.insertFilesBatch(scanId, {
        mk("duz.txt", "mft", 0, 1700000000, 1700000001),
        mk("virgul,icinde.txt", "mft", 0, 0, 0),
        mk("tirnak\"icinde.txt", "mft", 0, 0, 0),
        mk("satir\nsonu.txt", "mft", 0, 0, 0),
        mk(kTurkish + ".txt", "mft", 0, 0, 0),
        mk("-bayragu.txt", "mft", 0, 0, 0),
        mk("carve_no_date.jpg", "carver", 0, 0, 0),
        mk("ayrilmis.txt", "mft", 1, 0, 0),
    }));

    const std::string dest =
        (std::filesystem::temp_directory_path() /
         ("byteback_csv_test_" + std::to_string(testPid()) + ".csv")).string();
    FileListFilter filter; // defaults: all statuses, no discovery rows exist anyway
    int64_t rows = -1;
    std::string err = "unchanged";
    ASSERT_TRUE(store_.exportCsv(scanId, dest, filter, csvHeader(),
                                 "FS tarihi yok", kEmDash, &rows, &err));
    EXPECT_TRUE(err.empty());
    ASSERT_EQ(rows, 8);

    const std::string bom = "\xEF\xBB\xBF";
    const std::string header =
        "Ad;Boyut (bayt);Kategori;G\xC3\xBCven;Durum;Yol;Kaynak;Ba\xC5\x9Flang\xC4\xB1\xC3\xA7 Sekt\xC3\xB6r\xC3\xBC;Olu\xC5\x9Fturma;De\xC4\x9Fi\xC5\x9Ftirme";
    const std::string expected =
        bom + header + "\r\n"
        // 1700000000 -> 2023-11-14T22:13:20.000Z (toISOString parity).
        "duz.txt;123;Document;90;0;/docs/duz.txt;mft;10;2023-11-14T22:13:20.000Z;2023-11-14T22:13:21.000Z\r\n"
        // csvCell parity: comma/quote/newline trigger RFC4180 quoting ("" escape)
        // on EVERY cell — the path inherits the name's special characters.
        "\"virgul,icinde.txt\";123;Document;90;0;\"/docs/virgul,icinde.txt\";mft;10;" + kEmDash + ";" + kEmDash + "\r\n"
        "\"tirnak\"\"icinde.txt\";123;Document;90;0;\"/docs/tirnak\"\"icinde.txt\";mft;10;" + kEmDash + ";" + kEmDash + "\r\n"
        "\"satir\nsonu.txt\";123;Document;90;0;\"/docs/satir\nsonu.txt\";mft;10;" + kEmDash + ";" + kEmDash + "\r\n"
        // UTF-8 Turkish passes through unquoted (byte-identical with the input).
        + kTurkish + ".txt;123;Document;90;0;/docs/" + kTurkish + ".txt;mft;10;" + kEmDash + ";" + kEmDash + "\r\n"
        // Formula-injection guard: leading '-' gains a ' prefix on the cell it
        // starts with only — the path starts with '/', so it stays untouched.
        "'-bayragu.txt;123;Document;90;0;/docs/-bayragu.txt;mft;10;" + kEmDash + ";" + kEmDash + "\r\n"
        // Carve record with no FS date: localized no-FS-date label.
        "carve_no_date.jpg;123;Document;90;0;/docs/carve_no_date.jpg;carver;10;FS tarihi yok;FS tarihi yok\r\n"
        "ayrilmis.txt;123;Document;90;1;/docs/ayrilmis.txt;mft;10;" + kEmDash + ";" + kEmDash + "\r\n";
    EXPECT_EQ(readFileBytes(dest), expected);
    std::filesystem::remove(dest);
}

TEST_F(MetadataStoreTest, ExportCsvRespectsFilterAndOrderBy) {
    const int64_t scanId = store_.createScan(0, "deep", 100);
    ASSERT_GT(scanId, 0);

    auto mk = [&](const std::string& name, int status, uint64_t size) {
        FileRecord r;
        r.name = name;
        r.sizeBytes = size;
        r.status = status;
        r.source = "mft";
        return r;
    };
    ASSERT_TRUE(store_.insertFilesBatch(scanId, {
        mk("kucuk.txt", 0, 10),
        mk("buyuk.txt", 0, 900),
        mk("orta.txt", 1, 300),
    }));

    const std::string dest =
        (std::filesystem::temp_directory_path() /
         ("byteback_csv_filter_" + std::to_string(testPid()) + ".csv")).string();

    // Status filter: deleted only.
    FileListFilter del;
    del.status = 0;
    int64_t rows = -1;
    std::string err;
    ASSERT_TRUE(store_.exportCsv(scanId, dest, del, csvHeader(), "x", "—", &rows, &err));
    EXPECT_EQ(rows, 2);
    const std::string delFile = readFileBytes(dest);
    EXPECT_NE(delFile.find("kucuk.txt"), std::string::npos);
    EXPECT_NE(delFile.find("buyuk.txt"), std::string::npos);
    EXPECT_EQ(delFile.find("orta.txt"), std::string::npos);

    // Size bounds + orderBy parity with getFiles (size_desc): the single row
    // must be buyuk.txt AND it must be the first data row.
    FileListFilter sized;
    sized.status = 0;
    sized.sizeMin = 100;
    sized.orderBy = "size_desc";
    rows = -1;
    ASSERT_TRUE(store_.exportCsv(scanId, dest, sized, csvHeader(), "x", "—", &rows, &err));
    EXPECT_EQ(rows, 1);
    const std::string sizedFile = readFileBytes(dest);
    const size_t headerEnd = sizedFile.find("\r\n");
    ASSERT_NE(headerEnd, std::string::npos);
    EXPECT_EQ(sizedFile.find("buyuk.txt"), headerEnd + 2);
    EXPECT_EQ(sizedFile.find("kucuk.txt"), std::string::npos);

    // Name query rides the same FTS/LIKE dispatch as getFiles/searchFiles.
    FileListFilter q;
    q.query = "kucuk";
    rows = -1;
    err.clear();
    ASSERT_TRUE(store_.exportCsv(scanId, dest, q, csvHeader(), "x", "—", &rows, &err)) << err;
    EXPECT_EQ(rows, 1);
    EXPECT_NE(readFileBytes(dest).find("kucuk.txt"), std::string::npos);
    std::filesystem::remove(dest);
}

TEST_F(MetadataStoreTest, ExportCsvLargeRowSetFlushesEvery1000) {
    const int64_t scanId = store_.createScan(0, "deep", 100000);
    ASSERT_GT(scanId, 0);

    std::vector<FileRecord> batch(2500);
    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i].name = "r" + std::to_string(i) + ".bin";
        batch[i].sizeBytes = static_cast<uint64_t>(i);
        batch[i].status = 0;
    }
    ASSERT_TRUE(store_.insertFilesBatch(scanId, batch));

    const std::string dest =
        (std::filesystem::temp_directory_path() /
         ("byteback_csv_bulk_" + std::to_string(testPid()) + ".csv")).string();
    int64_t rows = -1;
    std::string err;
    ASSERT_TRUE(store_.exportCsv(scanId, dest, {}, csvHeader(), "x", "—", &rows, &err));
    EXPECT_EQ(rows, 2500);

    // Header line + 2500 CRLF-terminated rows.
    const std::string file = readFileBytes(dest);
    const int64_t newlines = static_cast<int64_t>(std::count(file.begin(), file.end(), '\n'));
    EXPECT_EQ(newlines, 2501);
    EXPECT_EQ(file.compare(0, 3, "\xEF\xBB\xBF"), 0);
    std::filesystem::remove(dest);
}

TEST_F(MetadataStoreTest, ExportCsvRejectsBadHeaderAndReportsError) {
    const int64_t scanId = store_.createScan(0, "quick", 10);
    ASSERT_GT(scanId, 0);
    const std::string dest =
        (std::filesystem::temp_directory_path() /
         ("byteback_csv_bad_" + std::to_string(testPid()) + ".csv")).string();

    int64_t rows = 7;
    std::string err;
    EXPECT_FALSE(store_.exportCsv(scanId, dest, {}, {"tek"}, "x", "—", &rows, &err));
    EXPECT_EQ(rows, 0);
    EXPECT_FALSE(err.empty());
    // No partial output may survive a rejected export.
    EXPECT_FALSE(std::filesystem::exists(dest));
    // Unwritable destination (a directory path) must fail with an error, not crash.
    const std::string dirDest = (std::filesystem::temp_directory_path() /
                                 ("byteback_csv_dir_" + std::to_string(testPid()))).string();
    std::filesystem::create_directories(dirDest);
    err.clear();
    EXPECT_FALSE(store_.exportCsv(scanId, dirDest, {}, csvHeader(), "x", "—", &rows, &err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove_all(dirDest);
}
