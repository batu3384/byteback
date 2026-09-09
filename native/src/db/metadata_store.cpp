#include "byteback_db.h"
#include "db/runs_codec.h"
#include "scan/discovery_sources.h"
#include "../../third_party/sqlite3.h"
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <regex>
#include <cctype>

namespace byteback {

namespace {

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
    if (!f.sourceNotLike.empty()) {
        sql += " AND ";
        sql += prefix;
        sql += "source NOT LIKE ?";
    }
    if (f.sizeMin > 0) {
        sql += " AND ";
        sql += prefix;
        sql += "size_bytes >= ?";
    }
    if (f.sizeMax > 0) {
        sql += " AND ";
        sql += prefix;
        sql += "size_bytes <= ?";
    }
    // P0-4: date bounds against modified_at with created_at fallback —
    // each bound is its own CASE so binding order stays linear.
    if (f.dateFrom > 0) {
        sql += " AND CASE WHEN ";
        sql += prefix;
        sql += "modified_at > 0 THEN ";
        sql += prefix;
        sql += "modified_at ELSE ";
        sql += prefix;
        sql += "created_at END >= ?";
    }
    if (f.dateTo > 0) {
        sql += " AND CASE WHEN ";
        sql += prefix;
        sql += "modified_at > 0 THEN ";
        sql += prefix;
        sql += "modified_at ELSE ";
        sql += prefix;
        sql += "created_at END <= ?";
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
        sql += discoverySourcesSqlInList();
        sql += ")";
    }
}

void bindListFilter(sqlite3_stmt* stmt, int& bind, const FileListFilter& f) {
    if (f.status >= 0) sqlite3_bind_int(stmt, bind++, f.status);
    if (!f.category.empty()) sqlite3_bind_text(stmt, bind++, f.category.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.sourceLike.empty()) sqlite3_bind_text(stmt, bind++, f.sourceLike.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.sourceNotLike.empty()) sqlite3_bind_text(stmt, bind++, f.sourceNotLike.c_str(), -1, SQLITE_TRANSIENT);
    if (f.sizeMin > 0) sqlite3_bind_int64(stmt, bind++, static_cast<int64_t>(f.sizeMin));
    if (f.sizeMax > 0) sqlite3_bind_int64(stmt, bind++, static_cast<int64_t>(f.sizeMax));
    if (f.dateFrom > 0) sqlite3_bind_int64(stmt, bind++, f.dateFrom);
    if (f.dateTo > 0) sqlite3_bind_int64(stmt, bind++, f.dateTo);
}

// CA-030: whitelisted sort keys — raw input never reaches the SQL string.
// FAZ 1.2: sortKeySql is the SINGLE source for the key expression; ORDER BY
// (orderByToSql) and the keyset cursor predicate in getFiles both consume it,
// so the two can never drift apart. Empty return = unknown key / id default:
// not keyset-capable.
std::string sortKeySql(const std::string& key) {
    if (key == "confidence_desc" || key == "confidence_asc") return "confidence";
    if (key == "size_desc" || key == "size_asc") return "size_bytes";
    // CA-030: full path / name ordering (NOCASE so case differences do not
    // split directory siblings).
    if (key == "name_asc" || key == "name_desc") return "name COLLATE NOCASE";
    if (key == "path_asc" || key == "path_desc") return "path COLLATE NOCASE";
    if (key == "date_desc" || key == "date_asc")
        return "CASE WHEN modified_at > created_at THEN modified_at ELSE created_at END";
    return "";
}

// Direction suffix of a whitelisted key ("*_desc"); sortKeySql already
// rejected everything else, so the suffix check cannot see raw input.
bool sortKeyDesc(const std::string& key) {
    return key.size() >= 5 && key.compare(key.size() - 5, 5, "_desc") == 0;
}

std::string orderByToSql(const std::string& key) {
    std::string expr = sortKeySql(key);
    if (expr.empty()) return "id";
    return expr + (sortKeyDesc(key) ? " DESC, id" : " ASC, id");
}

std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}

// ---- FAZ 1.3c CSV formatting (parity with the renderer's csvCell) ----

// Mirrors shared/html-escape.ts csvCell: formula-injection guard first, then
// RFC4180 quoting when the cell contains a quote, comma, newline or the ';'
// delimiter. '\r' alone does NOT trigger quoting (same as the JS regex).
std::string csvCellNative(const std::string& s) {
    std::string out = s;
    if (!out.empty() && (out[0] == '=' || out[0] == '+' || out[0] == '-' || out[0] == '@')) {
        out.insert(out.begin(), '\'');
    }
    if (out.find_first_of("\",\n;") != std::string::npos) {
        std::string quoted;
        quoted += '"';
        for (char c : out) {
            if (c == '"') quoted += "\"\"";
            else quoted += c;
        }
        quoted += '"';
        return quoted;
    }
    return out;
}

// Mirrors new Date(unixSec * 1000).toISOString(): UTC, millisecond precision,
// trailing 'Z'. Records store whole seconds, so the ms field is always .000.
std::string iso8601Utc(int64_t unixSec) {
    std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tmv {};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// Mirrors the renderer row rule: `ts > 0 ? toISOString() : formatFsTimestamp(0, source)`
// where the placeholder is the localized no-FS-date label for carve sources
// and '—' otherwise. Labels are handed in by the caller (renderer i18n).
std::string csvDateCell(int64_t ts, const std::string& source,
                        const std::string& noFsDateLabel, const std::string& noDateLabel) {
    if (ts > 0) return iso8601Utc(ts);
    return source.rfind("carver", 0) == 0 ? noFsDateLabel : noDateLabel;
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
    sqlite3_bind_int64(stmt, 19, static_cast<sqlite3_int64>(r.startByteOffset));
    sqlite3_bind_text(stmt, 20, r.contentHash.c_str(), -1, SQLITE_TRANSIENT);
}

constexpr const char* kFileSelect =
    "id, parent_id, name, extension, path, size_bytes, "
    "start_sector, end_sector, status, compressed, confidence, category, source, "
    "created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash";

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
    // CA-040: surface structural corruption at open (quick_check: fast, skips
    // index cross-checks — full integrity_check would stall big scan DBs).
    // W7: quick_check reports corruption as RESULT ROWS with rc=SQLITE_OK —
    // capture the first row; a null callback discarded it and corrupt DBs
    // opened "successfully".
    {
        struct QuickCheckCtx { std::string first; bool sawRow = false; };
        QuickCheckCtx qc;
        auto cb = [](void* ud, int cols, char** vals, char** /*names*/) -> int {
            auto* ctx = static_cast<QuickCheckCtx*>(ud);
            if (!ctx->sawRow && cols > 0 && vals[0]) { ctx->first = vals[0]; ctx->sawRow = true; }
            return 1; // first row is enough
        };
        char* err = nullptr;
        const int rc = sqlite3_exec(db_, "PRAGMA quick_check;", cb, &qc, &err);
        if (err) sqlite3_free(err);
        // rc=SQLITE_ABORT is expected: the callback returns 1 after the first
        // row. Corruption is flagged by the row TEXT, not the rc.
        const bool abortedEarly = (rc == SQLITE_ABORT);
        if ((!abortedEarly && rc != SQLITE_OK) || (qc.sawRow && qc.first != "ok")) {
            std::fprintf(stderr, "[byteback] DB quick_check failed: %s (rc=%d)\n",
                         qc.sawRow ? qc.first.c_str() : "no result", rc);
        }
    }
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
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN start_byte_offset INTEGER DEFAULT 0;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN content_hash TEXT DEFAULT '';", nullptr, nullptr, &err);
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
        // CA-040/W7: stamp the schema version only AFTER migrations ran, and
        // refuse to open a database from a NEWER schema than this build knows.
        constexpr int kSchemaVersion = 3;
        sqlite3_stmt* ver = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &ver, nullptr) == SQLITE_OK) {
            if (sqlite3_step(ver) == SQLITE_ROW) {
                const int existing = sqlite3_column_int(ver, 0);
                if (existing > kSchemaVersion) {
                    std::fprintf(stderr, "[byteback] DB schema v%d newer than build (v%d)\n", existing, kSchemaVersion);
                    sqlite3_finalize(ver);
                    sqlite3_close(db_);
                    db_ = nullptr;
                    return false;
                }
            }
            sqlite3_finalize(ver);
        }
        char* verr = nullptr;
        sqlite3_exec(db_, "PRAGMA user_version = 3;", nullptr, nullptr, &verr);
        if (verr) sqlite3_free(verr);
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
            start_byte_offset INTEGER DEFAULT 0,
            content_hash TEXT DEFAULT '',
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
            created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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

    if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* sql = R"(
        INSERT INTO files (scan_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, compressed, confidence, category, source,
            created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
    // A failed COMMIT (disk full / IO error) leaves the transaction open and
    // the batch un-persisted — reporting success here would silently clear the
    // caller's buffer of records that never reached the DB.
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
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

int64_t MetadataStore::reclaimOrphanRunningScans() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) return 0;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t changed = 0;

    const char* completeSql =
        "UPDATE scans SET status = 1, updated_at = ? "
        "WHERE status = 0 AND total_sectors > 0 AND scanned_sectors >= total_sectors "
        "AND (scan_type = 'quick' OR metadata_complete = 1)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, completeSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
        sqlite3_finalize(stmt);
    }

    const char* pauseSql = "UPDATE scans SET status = 4, updated_at = ? WHERE status = 0";
    if (sqlite3_prepare_v2(db_, pauseSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
        sqlite3_finalize(stmt);
    }
    return changed;
}

std::vector<FileRecord> MetadataStore::getFiles(int64_t scanId, int offset, int limit, const FileListFilter& filter) {
    if (!filter.query.empty()) {
        return searchFiles(scanId, filter.query, offset, limit, false, filter);
    }
    std::lock_guard<std::recursive_mutex> lock(mu_);
    // Negative LIMIT in SQLite means "no upper bound": a wrapped-negative IPC
    // page size would materialize the entire table. Clamp at the trust boundary.
    if (limit < 0) limit = 0;
    if (offset < 0) offset = 0;
    std::string sql = R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash
        FROM files WHERE scan_id = ?
    )";
    appendListFilter(sql, filter, "");
    // FAZ 1.2 keyset pagination: a usable cursor on a keyset-capable key
    // replaces OFFSET with a seek predicate. The id tiebreaker sorts ASC in
    // every direction, so the row-value comparison must expand per direction:
    //   asc:  key > v OR (key = v AND id > cursorId)
    //   desc: key < v OR (key = v AND id > cursorId)
    // (a single "(key, id) > (v, ?)" row-value term cannot express the DESC
    // next-page because the two columns order oppositely).
    std::string keyExpr = sortKeySql(filter.orderBy);
    const bool keyset = filter.hasCursor && filter.cursorId > 0 && !keyExpr.empty();
    if (keyset) {
        sql += " AND (";
        sql += keyExpr;
        sql += sortKeyDesc(filter.orderBy) ? " < ? OR (" : " > ? OR (";
        sql += keyExpr;
        sql += " = ? AND id > ?))";
    }
    sql += " ORDER BY ";
    sql += orderByToSql(filter.orderBy);
    sql += keyset ? " LIMIT ?" : " LIMIT ? OFFSET ?";

    std::vector<FileRecord> records;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(db_));
        return records;
    }
    int bind = 1;
    sqlite3_bind_int64(stmt, bind++, scanId);
    bindListFilter(stmt, bind, filter);
    if (keyset) {
        // Bind order matches the predicate: (v, v, id). Text keys compare
        // under the same COLLATE NOCASE as the ORDER BY expression.
        if (filter.orderBy.rfind("name", 0) == 0 || filter.orderBy.rfind("path", 0) == 0) {
            sqlite3_bind_text(stmt, bind++, filter.cursorText.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind++, filter.cursorText.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_int64(stmt, bind++, filter.cursorV);
            sqlite3_bind_int64(stmt, bind++, filter.cursorV);
        }
        sqlite3_bind_int64(stmt, bind++, filter.cursorId);
    }
    sqlite3_bind_int(stmt, bind++, limit);
    if (!keyset) sqlite3_bind_int(stmt, bind++, offset);

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
        if (sqlite3_column_count(stmt) > 18) {
            r.startByteOffset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 18));
            r.contentHash = safe_column_text(stmt, 19);
        }
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

// FAZ 1.3c: single streaming pass — one prepared SELECT per pass, no
// LIMIT/OFFSET (a 100-round offset walk used to re-sort the whole table per
// batch in the renderer). Column semantics mirror the renderer's old export:
// name;sizeBytes;category;confidence;status;path;source;startSector;
// createdAt;modifiedAt with csvCell quoting, ';' delimiter, CRLF rows and a
// UTF-8 BOM. Query filters ride the same FTS→LIKE dispatch as searchFiles
// (ORDER BY id there, no keyset); plain filters reuse the getFiles ORDER BY.
bool MetadataStore::exportCsv(int64_t scanId, const std::string& destPath, const FileListFilter& filter,
                              const std::vector<std::string>& header,
                              const std::string& noFsDateLabel, const std::string& noDateLabel,
                              int64_t* rowsOut, std::string* errOut) {
    if (rowsOut) *rowsOut = 0;
    if (errOut) errOut->clear();
    if (header.size() != 10) {
        if (errOut) *errOut = "csv header must carry exactly 10 column labels";
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (errOut) *errOut = "database is not open";
        return false;
    }

    std::FILE* out = std::fopen(destPath.c_str(), "wb");
    if (!out) {
        if (errOut) *errOut = "cannot open destination file for writing";
        return false;
    }
    auto fail = [&](const std::string& msg) {
        std::fclose(out);
        std::remove(destPath.c_str());
        if (errOut) *errOut = msg;
        return false;
    };

    const std::string bomAndHeader = "\xEF\xBB\xBF" + header[0] + ";" + header[1] + ";" + header[2] + ";" +
        header[3] + ";" + header[4] + ";" + header[5] + ";" + header[6] + ";" + header[7] + ";" +
        header[8] + ";" + header[9] + "\r\n";
    if (std::fwrite(bomAndHeader.data(), 1, bomAndHeader.size(), out) != bomAndHeader.size()) {
        return fail("failed to write CSV header");
    }

    constexpr const char* kCsvCols =
        "name, size_bytes, category, confidence, status, path, source, start_sector, created_at, modified_at";
    // FTS pass joins files with files_fts: columns must carry the f. prefix or
    // SQLite rejects them as ambiguous (same reason searchFiles prefixes).
    constexpr const char* kCsvColsFts =
        "f.name, f.size_bytes, f.category, f.confidence, f.status, f.path, f.source, f.start_sector, f.created_at, f.modified_at";
    const bool hasQuery = !filter.query.empty();
    std::vector<std::string> passes;
    if (hasQuery) {
        // Same FTS branch as searchFiles (ORDER BY id, whitelisted filter terms).
        // MATCH rides the FTS5 hidden column (fts.files_fts) — an alias on the
        // bare MATCH form (`fts MATCH ?`) prepares as "no such column: fts".
        std::string fts = std::string("SELECT ") + kCsvColsFts +
            " FROM files f INNER JOIN files_fts fts ON f.id = fts.rowid "
            "WHERE f.scan_id = ? AND fts.scan_id = ? AND fts.files_fts MATCH ?";
        appendListFilter(fts, filter, "f.");
        fts += " ORDER BY f.id";
        passes.push_back(std::move(fts));
        // Fallback branch — only executed when the FTS pass yields zero rows
        // (searchFiles parity: empty FTS result falls through to LIKE).
        std::string like = std::string("SELECT ") + kCsvCols + " FROM files WHERE scan_id = ?";
        appendListFilter(like, filter, "");
        like += " AND (LOWER(name) LIKE LOWER(?) ESCAPE '\\' OR LOWER(path) LIKE LOWER(?) ESCAPE '\\') ORDER BY id";
        passes.push_back(std::move(like));
    } else {
        std::string plain = std::string("SELECT ") + kCsvCols + " FROM files WHERE scan_id = ?";
        appendListFilter(plain, filter, "");
        plain += " ORDER BY " + orderByToSql(filter.orderBy);
        passes.push_back(std::move(plain));
    }

    const std::string match = hasQuery ? buildFtsMatch(filter.query) : "";
    std::string pattern;
    if (hasQuery) {
        pattern = "%";
        for (char c : filter.query) {
            if (c == '%' || c == '_' || c == '\\') pattern += '\\';
            pattern += c;
        }
        pattern += '%';
    }

    int64_t totalRows = 0;
    // Chunked locking: the DB lock is dropped around the 1000-row fwrite/fflush
    // so a live scan can keep committing between chunks.
    std::unique_lock<std::recursive_mutex> stepLock(mu_, std::defer_lock);
    for (size_t p = 0; p < passes.size(); ++p) {
        // FTS pass runs first; the LIKE fallback runs only after zero FTS rows.
        if (p == 1 && totalRows > 0) break;
        stepLock.lock();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, passes[p].c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            const std::string msg = sqlite3_errmsg(db_);
            stepLock.unlock();
            std::fclose(out);
            std::remove(destPath.c_str());
            if (errOut) *errOut = "SQLite error: " + msg;
            return false;
        }
        int bind = 1;
        sqlite3_bind_int64(stmt, bind++, scanId);
        if (hasQuery && p == 0) {
            sqlite3_bind_int64(stmt, bind++, scanId);
            sqlite3_bind_text(stmt, bind++, match.c_str(), -1, SQLITE_TRANSIENT);
        }
        bindListFilter(stmt, bind, filter);
        if (hasQuery && p == 1) {
            sqlite3_bind_text(stmt, bind++, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        }

        std::string chunk;
        int64_t rowsInPass = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string source = safe_column_text(stmt, 6);
            const std::string row =
                csvCellNative(safe_column_text(stmt, 0)) + ";" +
                csvCellNative(std::to_string(sqlite3_column_int64(stmt, 1))) + ";" +
                csvCellNative(safe_column_text(stmt, 2)) + ";" +
                csvCellNative(std::to_string(sqlite3_column_int(stmt, 3))) + ";" +
                csvCellNative(std::to_string(sqlite3_column_int(stmt, 4))) + ";" +
                csvCellNative(safe_column_text(stmt, 5)) + ";" +
                csvCellNative(source) + ";" +
                csvCellNative(std::to_string(sqlite3_column_int64(stmt, 7))) + ";" +
                csvCellNative(csvDateCell(sqlite3_column_int64(stmt, 8), source, noFsDateLabel, noDateLabel)) + ";" +
                csvCellNative(csvDateCell(sqlite3_column_int64(stmt, 9), source, noFsDateLabel, noDateLabel)) +
                "\r\n";
            chunk += row;
            ++rowsInPass;
            if (rowsInPass % 1000 == 0) {
                // Flush outside the DB lock — a live scan keeps committing.
                stepLock.unlock();
                const bool wrote = std::fwrite(chunk.data(), 1, chunk.size(), out) == chunk.size() &&
                                   std::fflush(out) == 0;
                chunk.clear();
                stepLock.lock();
                if (!wrote) {
                    sqlite3_finalize(stmt);
                    stepLock.unlock();
                    return fail("failed to write CSV chunk");
                }
            }
        }
        sqlite3_finalize(stmt);
        stepLock.unlock();
        if (!chunk.empty()) {
            if (std::fwrite(chunk.data(), 1, chunk.size(), out) != chunk.size() || std::fflush(out) != 0) {
                return fail("failed to write CSV chunk");
            }
        }
        totalRows += rowsInPass;
    }

    if (std::fflush(out) != 0) {
        return fail("failed to flush CSV output");
    }
    std::fclose(out);
    if (rowsOut) *rowsOut = totalRows;
    return true;
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

int64_t MetadataStore::getLatestUsableScanId() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const char* sql =
        "SELECT id FROM scans WHERE status IN (1, 4) ORDER BY id DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

bool MetadataStore::clearAllScanData() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) return false;

    auto execIgnore = [this](const char* sql) {
        char* err = nullptr;
        sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    };

    execIgnore("DROP TRIGGER IF EXISTS files_fts_ai");
    execIgnore("DROP TRIGGER IF EXISTS files_fts_ad");
    execIgnore("DROP TRIGGER IF EXISTS files_fts_au");
    execIgnore("DROP TABLE IF EXISTS files_fts");
    execIgnore("DROP TABLE IF EXISTS content_fts");
    execIgnore("DROP TABLE IF EXISTS content_chunk_fts");

    const char* core[] = {
        "DELETE FROM files",
        "DELETE FROM timeline_events",
        "DELETE FROM scans",
        nullptr,
    };
    for (const char** p = core; *p; ++p) {
        char* err = nullptr;
        if (sqlite3_exec(db_, *p, nullptr, nullptr, &err) != SQLITE_OK) {
            if (err) {
                fprintf(stderr, "clearAllScanData: %s\n", err);
                sqlite3_free(err);
            }
            return false;
        }
    }

    if (!ensureFtsIndex(db_) || !ensureContentFtsIndex(db_)) return false;
    sqlite3_exec(db_, "VACUUM", nullptr, nullptr, nullptr);
    return true;
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

// CA-036: batched timeline insert mirroring insertFilesBatch — a scan with a
// large USN journal used to pay one prepare/bind/step/finalize per record.
bool MetadataStore::appendTimelineEventsBatch(int64_t scanId, const std::vector<TimelineEvent>& events) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (events.empty()) return true;
    if (!db_) return false;

    if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* sql = R"(
        INSERT INTO timeline_events (scan_id, timestamp, event_type, file_name, mft_ref, source)
        VALUES (?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& ev : events) {
        sqlite3_bind_int64(stmt, 1, scanId);
        sqlite3_bind_int64(stmt, 2, ev.timestamp);
        sqlite3_bind_text(stmt, 3, ev.eventType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, ev.fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(ev.mftRef));
        sqlite3_bind_text(stmt, 6, ev.source.c_str(), -1, SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    // Same contract as insertFilesBatch: a failed COMMIT leaves the batch
    // un-persisted and must not report success.
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::vector<TimelineEvent> MetadataStore::getTimelineEvents(int64_t scanId, int offset, int limit,
                                                            const std::string& eventTypeFilter) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (limit < 0) limit = 0;  // negative LIMIT = unbounded in SQLite
    if (offset < 0) offset = 0;
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
    std::string sql = R"(
        SELECT COUNT(*),
               SUM(CASE WHEN status = 0 AND source NOT LIKE 'carver%' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Image' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Document' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Video' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Audio' THEN 1 ELSE 0 END),
               SUM(CASE WHEN category = 'Archive' THEN 1 ELSE 0 END),
               SUM(CASE WHEN source IN ('carver','carver_bgc') THEN 1 ELSE 0 END)
        FROM files WHERE scan_id = ? AND source NOT IN ()";
    sql += discoverySourcesSqlInList();
    sql += ")";
    ScanSummary summary;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return summary;
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
        if (sqlite3_column_count(stmt) > 18) {
            r.startByteOffset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 18));
        }
        if (sqlite3_column_count(stmt) > 19) {
            r.contentHash = safe_column_text(stmt, 19);
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
               created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash
        FROM files WHERE scan_id = ?
    )";

    if (!useRegex) {
        std::string match = buildFtsMatch(query);
        std::string sql = R"(
            SELECT f.id, f.parent_id, f.name, f.extension, f.path, f.size_bytes,
                   f.start_sector, f.end_sector, f.status, f.compressed, f.confidence, f.category, f.source,
                   f.created_at, f.modified_at, f.runs_json, f.resident_blob, f.integrity_checksum, f.start_byte_offset, f.content_hash
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
        // An unparseable MATCH (e.g. a lone quote tokenizes to nothing) fails
        // at PREPARE here but at STEP inside searchFiles — returning 0 would
        // disagree with the LIKE fallback results. Skip to the page-count path.
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
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
    }

    auto page = searchFiles(scanId, query, 0, 10000, useRegex, filter);
    if (static_cast<int64_t>(page.size()) < 10000) {
        return static_cast<int64_t>(page.size());
    }
    // ponytail: regex / non-FTS path — exact count via paginated scan (cap 500k rows).
    int64_t total = static_cast<int64_t>(page.size());
    for (int offset = 10000; offset < 500000; offset += 10000) {
        auto more = searchFiles(scanId, query, offset, 10000, useRegex, filter);
        total += static_cast<int64_t>(more.size());
        if (more.size() < 10000) break;
    }
    return total;
}

FileRecord MetadataStore::getFileById(int64_t fileId, int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    FileRecord r;
    r.id = -1;
    if (!db_ || fileId <= 0) return r;
    const char* sql = scanId > 0 ? R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash
        FROM files WHERE id = ? AND scan_id = ?
    )" : R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, compressed, confidence, category, source,
               created_at, modified_at, runs_json, resident_blob, integrity_checksum, start_byte_offset, content_hash
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

