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

std::string serializeRuns(const std::vector<FileRecord::DataRun>& runs) {
    if (runs.empty()) return "";
    std::string out = "[";
    for (size_t i = 0; i < runs.size(); ++i) {
        if (i) out += ',';
        out += "[" + std::to_string(runs[i].startSector) + "," +
               std::to_string(runs[i].sectorCount) + "]";
    }
    return out + "]";
}

std::vector<FileRecord::DataRun> deserializeRuns(const std::string& json) {
    std::vector<FileRecord::DataRun> runs;
    if (json.empty() || json.front() != '[') return runs;
    size_t i = 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] != '[') break;
        ++i;
        size_t c1 = json.find(',', i);
        size_t c2 = json.find(']', i);
        if (c1 == std::string::npos || c2 == std::string::npos || c1 > c2) break;
        FileRecord::DataRun run;
        run.startSector = std::stoull(json.substr(i, c1 - i));
        run.sectorCount = std::stoull(json.substr(c1 + 1, c2 - c1 - 1));
        runs.push_back(run);
        i = c2 + 1;
    }
    return runs;
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
    }
    return ok;
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
            start_sector, end_sector, status, compressed, confidence, category, source,
            created_at, modified_at, runs_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
    sqlite3_bind_int(stmt, 10, r.compressed ? 1 : 0);
    sqlite3_bind_int(stmt, 11, r.confidence);
    sqlite3_bind_text(stmt, 12, r.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, r.source.c_str(), -1, SQLITE_TRANSIENT);
    std::string runsJson = serializeRuns(r.runs);
    sqlite3_bind_int64(stmt, 14, r.createdAt);
    sqlite3_bind_int64(stmt, 15, r.modifiedAt);
    sqlite3_bind_text(stmt, 16, runsJson.c_str(), -1, SQLITE_TRANSIENT);

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
            start_sector, end_sector, status, compressed, confidence, category, source,
            created_at, modified_at, runs_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
        sqlite3_bind_int(stmt, 10, r.compressed ? 1 : 0);
        sqlite3_bind_int(stmt, 11, r.confidence);
        sqlite3_bind_text(stmt, 12, r.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, r.source.c_str(), -1, SQLITE_TRANSIENT);
        std::string runsJson = serializeRuns(r.runs);
        sqlite3_bind_int64(stmt, 14, r.createdAt);
        sqlite3_bind_int64(stmt, 15, r.modifiedAt);
        sqlite3_bind_text(stmt, 16, runsJson.c_str(), -1, SQLITE_TRANSIENT);

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
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json
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
    const char* sql = R"(
        SELECT id, drive_index, scan_type, total_sectors, scanned_sectors, status,
               recovered_files, started_at, updated_at
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
        state.startedAt = sqlite3_column_int64(stmt, 6);
        state.updatedAt = sqlite3_column_int64(stmt, 7);
        state.recoveredFiles = sqlite3_column_int64(stmt, 8);
    }
    sqlite3_finalize(stmt);
    return state;
}

// ---- Session + recovery bookkeeping (CA-008) ----

int64_t MetadataStore::getLatestScanId() {
    const char* sql = "SELECT id FROM scans ORDER BY id DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

bool MetadataStore::incrementRecovered(int64_t scanId) {
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

} // namespace wolf

