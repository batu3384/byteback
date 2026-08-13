#include "wolf_db.h"
#include "../../third_party/sqlite3.h"
#include <ctime>
#include <cstdio>

namespace wolf {

namespace {
std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}
} // namespace

MetadataStore::MetadataStore() : db_(nullptr) {}

MetadataStore::~MetadataStore() { close(); }

bool MetadataStore::open(const std::string& dbPath) {
    close();
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }
    // Enable WAL mode for concurrent reads
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    return createTables();
}

void MetadataStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MetadataStore::isOpen() const { return db_ != nullptr; }

bool MetadataStore::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS scans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drive_index INTEGER NOT NULL,
            scan_type TEXT NOT NULL,
            total_sectors INTEGER NOT NULL,
            scanned_sectors INTEGER DEFAULT 0,
            status INTEGER DEFAULT 0,
            started_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scan_id INTEGER NOT NULL,
            parent_id INTEGER DEFAULT -1,
            name TEXT NOT NULL,
            extension TEXT,
            path TEXT,
            size_bytes INTEGER,
            start_sector INTEGER,
            end_sector INTEGER,
            status INTEGER DEFAULT 3,
            confidence INTEGER DEFAULT 0,
            category TEXT,
            source TEXT,
            created_at INTEGER,
            modified_at INTEGER,
            FOREIGN KEY (scan_id) REFERENCES scans(id)
        );

        CREATE INDEX IF NOT EXISTS idx_files_scan_id ON files(scan_id);
        CREATE INDEX IF NOT EXISTS idx_files_extension ON files(extension);
        CREATE INDEX IF NOT EXISTS idx_files_category ON files(category);
        CREATE INDEX IF NOT EXISTS idx_files_confidence ON files(confidence);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}

int64_t MetadataStore::insertFile(int64_t scanId, const FileRecord& r) {
    const char* sql = R"(
        INSERT INTO files (scan_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, confidence, category, source,
            created_at, modified_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, scanId);
    sqlite3_bind_int64(stmt, 2, r.parentId);
    sqlite3_bind_text(stmt, 3, r.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, r.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, r.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(r.sizeBytes));
    sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(r.startSector));
    sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(r.endSector));
    sqlite3_bind_int(stmt, 9, r.status);
    sqlite3_bind_int(stmt, 10, r.confidence);
    sqlite3_bind_text(stmt, 11, r.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, r.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 13, r.createdAt);
    sqlite3_bind_int64(stmt, 14, r.modifiedAt);

    int rc = sqlite3_step(stmt);
    int64_t rowId = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(db_) : -1;
    sqlite3_finalize(stmt);
    return rowId;
}

bool MetadataStore::insertFilesBatch(int64_t scanId, const std::vector<FileRecord>& records) {
    if (records.empty()) return true;

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    const char* sql = R"(
        INSERT INTO files (scan_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, confidence, category, source,
            created_at, modified_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& r : records) {
        sqlite3_bind_int64(stmt, 1, scanId);
        sqlite3_bind_int64(stmt, 2, r.parentId);
        sqlite3_bind_text(stmt, 3, r.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, r.extension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, r.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(r.sizeBytes));
        sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(r.startSector));
        sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(r.endSector));
        sqlite3_bind_int(stmt, 9, r.status);
        sqlite3_bind_int(stmt, 10, r.confidence);
        sqlite3_bind_text(stmt, 11, r.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, r.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 13, r.createdAt);
        sqlite3_bind_int64(stmt, 14, r.modifiedAt);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

int64_t MetadataStore::createScan(int driveIndex, const std::string& scanType, uint64_t totalSectors) {
    const char* sql = R"(
        INSERT INTO scans (drive_index, scan_type, total_sectors, status, started_at, updated_at)
        VALUES (?, ?, ?, 0, ?, ?)
    )";

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, driveIndex);
    sqlite3_bind_text(stmt, 2, scanType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(totalSectors));
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, now);

    int rc = sqlite3_step(stmt);
    int64_t id = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(db_) : -1;
    sqlite3_finalize(stmt);
    return id;
}

bool MetadataStore::updateScanProgress(int64_t scanId, uint64_t scannedSectors) {
    const char* sql = "UPDATE scans SET scanned_sectors = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(scannedSectors));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MetadataStore::completeScan(int64_t scanId, int status) {
    const char* sql = "UPDATE scans SET status = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<FileRecord> MetadataStore::getFiles(int64_t scanId, int offset, int limit) {
    const char* sql = R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, confidence, category, source,
               created_at, modified_at
        FROM files WHERE scan_id = ? ORDER BY id LIMIT ? OFFSET ?
    )";

    std::vector<FileRecord> records;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return records;
    }
    sqlite3_bind_int64(stmt, 1, scanId);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.parentId = sqlite3_column_int64(stmt, 1);
        r.name = safe_column_text(stmt, 2);
        r.extension = safe_column_text(stmt, 3);
        r.path = safe_column_text(stmt, 4);
        r.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
        r.startSector = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        r.endSector = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        r.status = sqlite3_column_int(stmt, 8);
        r.confidence = sqlite3_column_int(stmt, 9);
        r.category = safe_column_text(stmt, 10);
        r.source = safe_column_text(stmt, 11);
        r.createdAt = sqlite3_column_int64(stmt, 12);
        r.modifiedAt = sqlite3_column_int64(stmt, 13);
        records.push_back(r);
    }

    sqlite3_finalize(stmt);
    return records;
}

int64_t MetadataStore::getFileCount(int64_t scanId) {
    const char* sql = "SELECT COUNT(*) FROM files WHERE scan_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, scanId);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

ScanState MetadataStore::getScanState(int64_t scanId) {
    const char* sql = "SELECT * FROM scans WHERE id = ?";
    ScanState state = {};
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return state;
    }
    sqlite3_bind_int64(stmt, 1, scanId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        state.id = sqlite3_column_int64(stmt, 0);
        state.driveIndex = sqlite3_column_int(stmt, 1);
        state.scanType = safe_column_text(stmt, 2);
        state.totalSectors = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        state.scannedSectors = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        state.status = sqlite3_column_int(stmt, 5);
        state.startedAt = sqlite3_column_int64(stmt, 6);
        state.updatedAt = sqlite3_column_int64(stmt, 7);
    }
    sqlite3_finalize(stmt);
    return state;
}

} // namespace wolf

