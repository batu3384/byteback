#include "byteback_db.h"
#include "db/runs_codec.h"
#include "../../third_party/sqlite3.h"
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <regex>
#include <cctype>

namespace byteback {

namespace {
const char* kDiscoverySourcesSql =
    "'apfs_container','apfs_volume','apfs_file','bitlocker_detect','bitlocker_fve',"
    "'vss_unbound','vss_bind','vss_snapshot','hfs_limit','usn_journal',"
    "'ntfs_logfile','ntfs_logfile_restart','ntfs_recycle_meta'";

void appendListFilter(std::string& sql, const FileListFilter& f, const char* prefix) {
    if (f.status >= 0) {
        sql += " AND ";
        sql += prefix;
        sql += "status = ?";
    }
    if (!f.category.empty()) {
        sql += " AND ";
        sql += prefix;
        sql += "category = ?";
    }
    if (!f.sourceLike.empty()) {
        sql += " AND ";
        sql += prefix;
        sql += "source LIKE ?";
    }
    if (!f.includeDuplicates) {
        sql += " AND ";
        sql += prefix;
        sql += "source != 'carver_duplicate'";
    }
    if (!f.includeDiscovery) {
        sql += " AND ";
        sql += prefix;
        sql += "source NOT IN (";
        sql += kDiscoverySourcesSql;
        sql += ")";
    }
}

void bindListFilter(sqlite3_stmt* stmt, int& bind, const FileListFilter& f) {
    if (f.status >= 0) sqlite3_bind_int(stmt, bind++, f.status);
    if (!f.category.empty()) sqlite3_bind_text(stmt, bind++, f.category.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.sourceLike.empty()) sqlite3_bind_text(stmt, bind++, f.sourceLike.c_str(), -1, SQLITE_TRANSIENT);
}

std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}

void bindFileRecord(sqlite3_stmt* stmt, int64_t scanId, const FileRecord& r) {
    sqlite3_bind_int64(stmt, 1, scanId);
    sqlite3_bind_int64(stmt, 2, r.parentId);
    sqlite3_bind_text(stmt, 3, r.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, r.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, r.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(r.sizeBytes));
    sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(r.startSector));
    sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(r.endSector));
    sqlite3_bind_int(stmt, 9, r.status);
    sqlite3_bind_int(stmt, 10, r.compressed ? 1 : 0);
    sqlite3_bind_int(stmt, 11, r.confidence);
    sqlite3_bind_text(stmt, 12, r.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, r.source.c_str(), -1, SQLITE_TRANSIENT);
    std::string runsJson = serializeRuns(r.runs);
    sqlite3_bind_int64(stmt, 14, r.createdAt);
    sqlite3_bind_int64(stmt, 15, r.modifiedAt);
    sqlite3_bind_text(stmt, 16, runsJson.c_str(), -1, SQLITE_TRANSIENT);
    if (r.residentData.empty()) {
        sqlite3_bind_null(stmt, 17);
    } else {
        sqlite3_bind_blob(stmt, 17, r.residentData.data(),
                          static_cast<int>(r.residentData.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 18, static_cast<sqlite3_int64>(r.integrityChecksum));
}

constexpr const char* kFileSelect =
    "id, parent_id, name, extension, path, size_bytes, "
    "start_sector, end_sector, status, compressed, confidence, category, source, "
    "created_at, modified_at, runs_json, resident_blob, integrity_checksum";

std::string buildFtsMatch(const std::string& query) {
    std::string out;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        if (!out.empty()) out += " AND ";
        std::string esc;
        for (char c : token) {
            if (c == '"') esc += "\"\"";
            else esc += c;
        }
        out += "\"" + esc + "\"*";
        token.clear();
    };
    for (char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) flush();
        else token += c;
    }
    flush();
    return out.empty() ? "\"\"" : out;
}

bool ensureFtsIndex(sqlite3* db) {
    const char* ftsSql = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5(
            scan_id UNINDEXED,
            name,
            path,
            extension
        );
    )";
    if (sqlite3_exec(db, ftsSql, nullptr, nullptr, nullptr) != SQLITE_OK) return false;

    const char* triggers = R"(
        CREATE TRIGGER IF NOT EXISTS files_fts_ai AFTER INSERT ON files BEGIN
          INSERT INTO files_fts(rowid, scan_id, name, path, extension)
          VALUES (new.id, new.scan_id, new.name, COALESCE(new.path,''), COALESCE(new.extension,''));
        END;
        CREATE TRIGGER IF NOT EXISTS files_fts_ad AFTER DELETE ON files BEGIN
          INSERT INTO files_fts(files_fts, rowid, scan_id, name, path, extension)
          VALUES('delete', old.id, old.scan_id, old.name, old.path, old.extension);
        END;
        CREATE TRIGGER IF NOT EXISTS files_fts_au AFTER UPDATE ON files BEGIN
          INSERT INTO files_fts(files_fts, rowid, scan_id, name, path, extension)
          VALUES('delete', old.id, old.scan_id, old.name, old.path, old.extension);
          INSERT INTO files_fts(rowid, scan_id, name, path, extension)
          VALUES (new.id, new.scan_id, new.name, COALESCE(new.path,''), COALESCE(new.extension,''));
        END;
    )";
    sqlite3_exec(db, triggers, nullptr, nullptr, nullptr);

    const char* backfill = R"(
        INSERT INTO files_fts(rowid, scan_id, name, path, extension)
        SELECT f.id, f.scan_id, f.name, COALESCE(f.path,''), COALESCE(f.extension,'')
        FROM files f
        WHERE f.id NOT IN (SELECT rowid FROM files_fts);
    )";
    sqlite3_exec(db, backfill, nullptr, nullptr, nullptr);
    return true;
}

bool ensureContentFtsIndex(sqlite3* db) {
    const char* ftsSql = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS content_fts USING fts5(body);
    )";
    return sqlite3_exec(db, ftsSql, nullptr, nullptr, nullptr) == SQLITE_OK;
}
} // namespace

MetadataStore::MetadataStore() : db_(nullptr) {}

MetadataStore::~MetadataStore() { close(); }

bool MetadataStore::open(const std::string& dbPath) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    close();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(dbPath.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }
    // Enable WAL mode for concurrent reads
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    bool ok = createTables();
    // CA-005 migration: existing databases predate the compressed column.
    if (ok) {
        char* err = nullptr;
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN compressed INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err); // column already exists -> ignore
        sqlite3_exec(db_, "ALTER TABLE scans ADD COLUMN recovered_files INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN runs_json TEXT DEFAULT '';", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN resident_blob BLOB;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN integrity_checksum INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE scans ADD COLUMN partition_start_sector INTEGER DEFAULT -1;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE scans ADD COLUMN partition_size_sectors INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE scans ADD COLUMN metadata_complete INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE scans ADD COLUMN carve_resume_sector INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        ensureFtsIndex(db_);
        ensureContentFtsIndex(db_);
    }
    return ok;
}

void MetadataStore::close() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MetadataStore::isOpen() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return db_ != nullptr;
}

bool MetadataStore::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS scans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drive_index INTEGER NOT NULL,
            scan_type TEXT NOT NULL,
            total_sectors INTEGER NOT NULL,
            scanned_sectors INTEGER DEFAULT 0,
            status INTEGER DEFAULT 0,
            recovered_files INTEGER DEFAULT 0,
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
            compressed INTEGER DEFAULT 0,
            confidence INTEGER DEFAULT 0,
            category TEXT,
            source TEXT,
            created_at INTEGER,
            modified_at INTEGER,
            runs_json TEXT DEFAULT '',
            resident_blob BLOB,
            FOREIGN KEY (scan_id) REFERENCES scans(id)
        );

        CREATE TABLE IF NOT EXISTS timeline_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scan_id INTEGER NOT NULL,
            timestamp INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            file_name TEXT,
            mft_ref INTEGER,
            source TEXT,
            FOREIGN KEY (scan_id) REFERENCES scans(id)
        );
        CREATE INDEX IF NOT EXISTS idx_timeline_scan_ts ON timeline_events(scan_id, timestamp);
        CREATE INDEX IF NOT EXISTS idx_timeline_type ON timeline_events(event_type);

        CREATE INDEX IF NOT EXISTS idx_files_scan_id ON files(scan_id);
        CREATE INDEX IF NOT EXISTS idx_files_scan_status ON files(scan_id, status);
        CREATE INDEX IF NOT EXISTS idx_files_extension ON files(extension);
        CREATE INDEX IF NOT EXISTS idx_files_category ON files(category);
        CREATE INDEX IF NOT EXISTS idx_files_confidence ON files(confidence);

        CREATE TABLE IF NOT EXISTS case_info (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            case_number TEXT NOT NULL DEFAULT '',
            investigator TEXT NOT NULL DEFAULT '',
            agency TEXT NOT NULL DEFAULT '',
            notes TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL DEFAULT 0,
            updated_at INTEGER NOT NULL DEFAULT 0
        );
        INSERT OR IGNORE INTO case_info (id) VALUES (1);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}

int64_t MetadataStore::insertFile(int64_t scanId, const FileRecord& r) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = R"(
        INSERT INTO files (scan_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, compressed, confidence, category, source,
            created_at, modified_at, runs_json, resident_blob, integrity_checksum)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return -1;
    }

    bindFileRecord(stmt, scanId, r);

    int rc = sqlite3_step(stmt);
    int64_t rowId = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(db_) : -1;
    sqlite3_finalize(stmt);
    return rowId;
}

bool MetadataStore::insertFilesBatch(int64_t scanId, const std::vector<FileRecord>& records) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (records.empty()) return true;
    if (!db_) return false;

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    const char* sql = R"(
        INSERT INTO files (scan_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, compressed, confidence, category, source,
            created_at, modified_at, runs_json, resident_blob, integrity_checksum)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& r : records) {
        bindFileRecord(stmt, scanId, r);

        const int rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

int64_t MetadataStore::createScan(int driveIndex, const std::string& scanType, uint64_t totalSectors) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
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

bool MetadataStore::setScanTotalSectors(int64_t scanId, uint64_t totalSectors) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = "UPDATE scans SET total_sectors = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(totalSectors));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MetadataStore::updateScanProgress(int64_t scanId, uint64_t scannedSectors) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
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

bool MetadataStore::setScanPartition(int64_t scanId, int64_t partitionStartSector,
                                     uint64_t partitionSizeSectors) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql =
        "UPDATE scans SET partition_start_sector = ?, partition_size_sectors = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, partitionStartSector);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(partitionSizeSectors));
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MetadataStore::updateScanCheckpoint(int64_t scanId, bool metadataComplete,
                                         uint64_t carveResumeSector) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql =
        "UPDATE scans SET metadata_complete = ?, carve_resume_sector = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, metadataComplete ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(carveResumeSector));
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MetadataStore::setScanRunning(int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = "UPDATE scans SET status = 0, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, scanId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MetadataStore::completeScan(int64_t scanId, int status) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
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

std::vector<FileRecord> MetadataStore::getFiles(int64_t scanId, int offset, int limit, const FileListFilter& filter) {
    if (!filter.query.empty()) {
        return searchFiles(scanId, filter.query, offset, limit, false, filter);
    }
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::string sql = R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum
        FROM files WHERE scan_id = ?
    )";
    appendListFilter(sql, filter, "");
    sql += " ORDER BY id LIMIT ? OFFSET ?";

    std::vector<FileRecord> records;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return records;
    }
    int bind = 1;
    sqlite3_bind_int64(stmt, bind++, scanId);
    bindListFilter(stmt, bind, filter);
    sqlite3_bind_int(stmt, bind++, limit);
    sqlite3_bind_int(stmt, bind++, offset);

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
        r.compressed = sqlite3_column_int(stmt, 9) != 0;
        r.confidence = sqlite3_column_int(stmt, 10);
        r.category = safe_column_text(stmt, 11);
        r.source = safe_column_text(stmt, 12);
        r.createdAt = sqlite3_column_int64(stmt, 13);
        r.modifiedAt = sqlite3_column_int64(stmt, 14);
        r.runs = deserializeRuns(safe_column_text(stmt, 15));
        records.push_back(r);
    }

    sqlite3_finalize(stmt);
    return records;
}

int64_t MetadataStore::getFileCount(int64_t scanId, const FileListFilter& filter) {
    if (!filter.query.empty()) {
        return searchFilesCount(scanId, filter.query, false, filter);
    }
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::string sql = "SELECT COUNT(*) FROM files WHERE scan_id = ?";
    appendListFilter(sql, filter, "");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return -1;
    }
    int bind = 1;
    sqlite3_bind_int64(stmt, bind++, scanId);
    bindListFilter(stmt, bind, filter);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

ScanState MetadataStore::getScanState(int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = R"(
        SELECT id, drive_index, scan_type, total_sectors, scanned_sectors, status,
               recovered_files, started_at, updated_at,
               partition_start_sector, partition_size_sectors, metadata_complete, carve_resume_sector
        FROM scans WHERE id = ?
    )";
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
        state.recoveredFiles = sqlite3_column_int64(stmt, 6);
        state.startedAt = sqlite3_column_int64(stmt, 7);
        state.updatedAt = sqlite3_column_int64(stmt, 8);
        if (sqlite3_column_count(stmt) > 9) {
            state.partitionStartSector = sqlite3_column_int64(stmt, 9);
            state.partitionSizeSectors = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
            state.metadataComplete = sqlite3_column_int(stmt, 11) != 0;
            state.carveResumeSector = static_cast<uint64_t>(sqlite3_column_int64(stmt, 12));
        }
    }
    sqlite3_finalize(stmt);
    return state;
}

// ---- Session + recovery bookkeeping (CA-008) ----

int64_t MetadataStore::getLatestScanId() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = "SELECT id FROM scans ORDER BY id DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

bool MetadataStore::incrementRecovered(int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = "UPDATE scans SET recovered_files = recovered_files + 1, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(stmt, 2, scanId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// ---- Unified timeline ----

// Decode the USN reason bitmap into the dominant event type. Priority: the
// most forensically significant reason wins (delete > rename > truncate >
// extend > overwrite > create > touch).
static std::string usnReasonToEventType(uint32_t reason) {
    if (reason & 0x00000002) return "delete";
    if (reason & 0x00010000) return "rename_old";
    if (reason & 0x00020000) return "rename_new";
    if (reason & 0x00000010) return "truncate";
    if (reason & 0x00000008) return "extend";
    if (reason & 0x00000004) return "overwrite";
    if (reason & 0x00000001) return "create";
    return "touch";
}

int64_t MetadataStore::insertTimelineEvent(int64_t scanId, const TimelineEvent& ev) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = R"(
        INSERT INTO timeline_events (scan_id, timestamp, event_type, file_name, mft_ref, source)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, scanId);
    sqlite3_bind_int64(stmt, 2, ev.timestamp);
    sqlite3_bind_text(stmt, 3, ev.eventType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, ev.fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(ev.mftRef));
    sqlite3_bind_text(stmt, 6, ev.source.c_str(), -1, SQLITE_TRANSIENT);

    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return id;
}

std::vector<TimelineEvent> MetadataStore::getTimelineEvents(int64_t scanId, int offset, int limit,
                                                            const std::string& eventTypeFilter) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<TimelineEvent> out;
    std::string sql =
        "SELECT id, scan_id, timestamp, event_type, file_name, mft_ref, source "
        "FROM timeline_events WHERE scan_id = ?";
    if (!eventTypeFilter.empty()) sql += " AND event_type = ?";
    sql += " ORDER BY timestamp ASC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

    int bind = 1;
    sqlite3_bind_int64(stmt, bind++, scanId);
    if (!eventTypeFilter.empty()) sqlite3_bind_text(stmt, bind++, eventTypeFilter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, bind++, limit);
    sqlite3_bind_int(stmt, bind++, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TimelineEvent ev;
        ev.id = sqlite3_column_int64(stmt, 0);
        ev.scanId = sqlite3_column_int64(stmt, 1);
        ev.timestamp = sqlite3_column_int64(stmt, 2);
        ev.eventType = safe_column_text(stmt, 3);
        ev.fileName = safe_column_text(stmt, 4);
        ev.mftRef = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
        ev.source = safe_column_text(stmt, 6);
        out.push_back(std::move(ev));
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t MetadataStore::getTimelineEventCount(int64_t scanId, const std::string& eventTypeFilter) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::string sql = "SELECT COUNT(*) FROM timeline_events WHERE scan_id = ?";
    if (!eventTypeFilter.empty()) sql += " AND event_type = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, scanId);
    if (!eventTypeFilter.empty()) sqlite3_bind_text(stmt, 2, eventTypeFilter.c_str(), -1, SQLITE_TRANSIENT);
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

MetadataStore::ScanSummary MetadataStore::getScanSummary(int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql = R"(
        SELECT COUNT(*),
               SUM(CASE WHEN status = 0 THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Image' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Document' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Video' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Audio' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Archive' THEN 1 ELSE 0 END),
               SUM(CASE WHEN source LIKE 'carver%' THEN 1 ELSE 0 END)
        FROM files WHERE scan_id = ?
    )";
    ScanSummary summary;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return summary;
    sqlite3_bind_int64(stmt, 1, scanId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        summary.totalFiles = sqlite3_column_int64(stmt, 0);
        summary.deletedFiles = sqlite3_column_int64(stmt, 1);
        summary.imageFiles = sqlite3_column_int64(stmt, 2);
        summary.documentFiles = sqlite3_column_int64(stmt, 3);
        summary.videoFiles = sqlite3_column_int64(stmt, 4);
        summary.audioFiles = sqlite3_column_int64(stmt, 5);
        summary.archiveFiles = sqlite3_column_int64(stmt, 6);
        summary.carvedFiles = sqlite3_column_int64(stmt, 7);
    }
    sqlite3_finalize(stmt);

    const char* tlSql = R"(
        SELECT COUNT(*),
               SUM(CASE WHEN event_type = 'create' THEN 1 ELSE 0 END),
               SUM(CASE WHEN event_type = 'delete' THEN 1 ELSE 0 END),
               SUM(CASE WHEN event_type IN ('rename_old','rename_new') THEN 1 ELSE 0 END)
        FROM timeline_events WHERE scan_id = ?
    )";
    if (sqlite3_prepare_v2(db_, tlSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, scanId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            summary.timelineEvents = sqlite3_column_int64(stmt, 0);
            summary.usnCreates = sqlite3_column_int64(stmt, 1);
            summary.usnDeletes = sqlite3_column_int64(stmt, 2);
            summary.usnRenames = sqlite3_column_int64(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }
    return summary;
}

namespace {
FileRecord rowToFileRecord(sqlite3_stmt* stmt) {
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
    r.compressed = sqlite3_column_int(stmt, 9) != 0;
    r.confidence = sqlite3_column_int(stmt, 10);
    r.category = safe_column_text(stmt, 11);
    r.source = safe_column_text(stmt, 12);
    r.createdAt = sqlite3_column_int64(stmt, 13);
    r.modifiedAt = sqlite3_column_int64(stmt, 14);
        r.runs = deserializeRuns(safe_column_text(stmt, 15));
        const void* blob = sqlite3_column_blob(stmt, 16);
        const int blobLen = sqlite3_column_bytes(stmt, 16);
        if (blob && blobLen > 0) {
            const auto* p = static_cast<const uint8_t*>(blob);
            r.residentData.assign(p, p + blobLen);
        }
        if (sqlite3_column_count(stmt) > 17) {
            r.integrityChecksum = static_cast<uint64_t>(sqlite3_column_int64(stmt, 17));
        }
        return r;
}
} // namespace

std::vector<FileRecord> MetadataStore::searchFiles(int64_t scanId, const std::string& query,
                                                   int offset, int limit, bool useRegex,
                                                   const std::string& categoryFilter,
                                                   int statusFilter) {
    FileListFilter filter;
    filter.category = categoryFilter;
    filter.status = statusFilter;
    return searchFiles(scanId, query, offset, limit, useRegex, filter);
}

std::vector<FileRecord> MetadataStore::searchFiles(int64_t scanId, const std::string& query,
                                                   int offset, int limit, bool useRegex,
                                                   const FileListFilter& filter) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<FileRecord> records;
    if (query.empty() || limit <= 0) return records;
    if (useRegex && query.size() > 128) return records;

    const char* baseSql = R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum
        FROM files WHERE scan_id = ?
    )";

    if (!useRegex) {
        std::string match = buildFtsMatch(query);
        std::string sql = R"(
            SELECT f.id, f.parent_id, f.name, f.extension, f.path, f.size_bytes,
                   f.start_sector, f.end_sector, f.status, f.compressed, f.confidence, f.category, f.source,
                   f.created_at, f.modified_at, f.runs_json, f.resident_blob, f.integrity_checksum
            FROM files f
            INNER JOIN files_fts fts ON f.id = fts.rowid
            WHERE f.scan_id = ? AND fts.scan_id = ? AND fts MATCH ?
        )";
        appendListFilter(sql, filter, "f.");
        sql += " ORDER BY f.id LIMIT ? OFFSET ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            int bind = 1;
            sqlite3_bind_int64(stmt, bind++, scanId);
            sqlite3_bind_int64(stmt, bind++, scanId);
            sqlite3_bind_text(stmt, bind++, match.c_str(), -1, SQLITE_TRANSIENT);
            bindListFilter(stmt, bind, filter);
            sqlite3_bind_int(stmt, bind++, limit);
            sqlite3_bind_int(stmt, bind++, offset);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                records.push_back(rowToFileRecord(stmt));
            }
            sqlite3_finalize(stmt);
            if (!records.empty()) return records;
        }

        std::string likeSql = std::string(baseSql);
        appendListFilter(likeSql, filter, "");
        likeSql += " AND (LOWER(name) LIKE LOWER(?) ESCAPE '\\' OR LOWER(path) LIKE LOWER(?) ESCAPE '\\') "
                   "ORDER BY id LIMIT ? OFFSET ?";
        std::string pattern = "%";
        for (char c : query) {
            if (c == '%' || c == '_' || c == '\\') pattern += '\\';
            pattern += c;
        }
        pattern += '%';
        if (sqlite3_prepare_v2(db_, likeSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return records;
        int bind = 1;
        sqlite3_bind_int64(stmt, bind++, scanId);
        bindListFilter(stmt, bind, filter);
        sqlite3_bind_text(stmt, bind++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind++, limit);
        sqlite3_bind_int(stmt, bind++, offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            records.push_back(rowToFileRecord(stmt));
        }
        sqlite3_finalize(stmt);
        return records;
    }

    std::regex re;
    try {
        re = std::regex(query, std::regex::icase | std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        return records;
    }

    std::string sql = std::string(baseSql);
    appendListFilter(sql, filter, "");
    sql += " ORDER BY id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return records;
    int bind = 1;
    sqlite3_bind_int64(stmt, bind++, scanId);
    bindListFilter(stmt, bind, filter);

    int skipped = 0;
    int scanned = 0;
    constexpr int kMaxRegexRows = 20000;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (++scanned > kMaxRegexRows) break;
        FileRecord r = rowToFileRecord(stmt);
        const std::string hay = r.name + " " + r.path;
        if (!std::regex_search(hay, re)) continue;
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        records.push_back(std::move(r));
        if (static_cast<int>(records.size()) >= limit) break;
    }
    sqlite3_finalize(stmt);
    return records;
}

int64_t MetadataStore::searchFilesCount(int64_t scanId, const std::string& query, bool useRegex,
                                        const std::string& categoryFilter,
                                        int statusFilter) {
    FileListFilter filter;
    filter.category = categoryFilter;
    filter.status = statusFilter;
    return searchFilesCount(scanId, query, useRegex, filter);
}

int64_t MetadataStore::searchFilesCount(int64_t scanId, const std::string& query, bool useRegex,
                                        const FileListFilter& filter) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (query.empty()) return 0;

    if (!useRegex) {
        std::string match = buildFtsMatch(query);
        std::string sql = R"(
            SELECT COUNT(*) FROM files f
            INNER JOIN files_fts fts ON f.id = fts.rowid
            WHERE f.scan_id = ? AND fts.scan_id = ? AND fts MATCH ?
        )";
        appendListFilter(sql, filter, "f.");
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
        int bind = 1;
        sqlite3_bind_int64(stmt, bind++, scanId);
        sqlite3_bind_int64(stmt, bind++, scanId);
        sqlite3_bind_text(stmt, bind++, match.c_str(), -1, SQLITE_TRANSIENT);
        bindListFilter(stmt, bind, filter);
        int64_t n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        if (n > 0) return n;
    }

    auto page = searchFiles(scanId, query, 0, 10000, useRegex, filter);
    return static_cast<int64_t>(page.size());
}

FileRecord MetadataStore::getFileById(int64_t fileId, int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    FileRecord r;
    r.id = -1;
    if (!db_ || fileId <= 0) return r;
    const char* sql = scanId > 0 ? R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum
        FROM files WHERE id = ? AND scan_id = ?
    )" : R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum
        FROM files WHERE id = ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return r;
    sqlite3_bind_int64(stmt, 1, fileId);
    if (scanId > 0) sqlite3_bind_int64(stmt, 2, scanId);
    if (sqlite3_step(stmt) == SQLITE_ROW) r = rowToFileRecord(stmt);
    sqlite3_finalize(stmt);
    return r;
}

} // namespace byteback

