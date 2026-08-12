#include "db/scan_session.h"
#include "../../third_party/sqlite3.h"
#include <ctime>
#include <cstdio>
#include <iostream>

namespace wolf {

namespace {
std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}
} // namespace

ScanSessionManager::ScanSessionManager() : db_(nullptr) {}

ScanSessionManager::~ScanSessionManager() {
    close();
}

bool ScanSessionManager::initialize(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(dbMutex_);
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

void ScanSessionManager::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ScanSessionManager::isOpen() const {
    return db_ != nullptr;
}

bool ScanSessionManager::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS scan_sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drive_index INTEGER NOT NULL,
            scan_type TEXT NOT NULL,
            total_sectors INTEGER NOT NULL,
            scanned_sectors INTEGER DEFAULT 0,
            status INTEGER DEFAULT 0,
            started_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS session_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER NOT NULL,
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
            FOREIGN KEY (session_id) REFERENCES scan_sessions(id)
        );

        CREATE INDEX IF NOT EXISTS idx_session_files_session_id ON session_files(session_id);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}

int64_t ScanSessionManager::createSession(int driveIndex, const std::string& scanType, uint64_t totalSectors) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO scan_sessions (drive_index, scan_type, total_sectors, status, started_at, updated_at)
        VALUES (?, ?, ?, 0, ?, ?)
    )";

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
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

int64_t ScanSessionManager::insertFileInternal(int64_t sessionId, const FileRecord& r) {
    const char* sql = R"(
        INSERT INTO session_files (session_id, parent_id, name, extension, path, size_bytes,
            start_sector, end_sector, status, confidence, category, source,
            created_at, modified_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, sessionId);
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

bool ScanSessionManager::saveState(int64_t sessionId, uint64_t scannedSectors, const std::vector<FileRecord>& newFiles) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;

    // Use transaction for bulk insert
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    const char* sql = "UPDATE scan_sessions SET scanned_sectors = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(scannedSectors));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, sessionId);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_finalize(stmt);

    for (const auto& file : newFiles) {
        if (insertFileInternal(sessionId, file) == -1) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

bool ScanSessionManager::loadState(int64_t sessionId, SessionData& outData) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;

    // Load session state
    const char* sqlState = "SELECT * FROM scan_sessions WHERE id = ?";
    sqlite3_stmt* stmtState = nullptr;
    if (sqlite3_prepare_v2(db_, sqlState, -1, &stmtState, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmtState, 1, sessionId);
    
    if (sqlite3_step(stmtState) == SQLITE_ROW) {
        outData.state.id = sqlite3_column_int64(stmtState, 0);
        outData.state.driveIndex = sqlite3_column_int(stmtState, 1);
        outData.state.scanType = safe_column_text(stmtState, 2);
        outData.state.totalSectors = static_cast<uint64_t>(sqlite3_column_int64(stmtState, 3));
        outData.state.scannedSectors = static_cast<uint64_t>(sqlite3_column_int64(stmtState, 4));
        outData.state.status = sqlite3_column_int(stmtState, 5);
        outData.state.startedAt = sqlite3_column_int64(stmtState, 6);
        outData.state.updatedAt = sqlite3_column_int64(stmtState, 7);
    } else {
        sqlite3_finalize(stmtState);
        return false; // Session not found
    }
    sqlite3_finalize(stmtState);

    // Load files
    const char* sqlFiles = R"(
        SELECT id, parent_id, name, extension, path, size_bytes,
               start_sector, end_sector, status, confidence, category, source,
               created_at, modified_at
        FROM session_files WHERE session_id = ? ORDER BY id
    )";
    
    sqlite3_stmt* stmtFiles = nullptr;
    if (sqlite3_prepare_v2(db_, sqlFiles, -1, &stmtFiles, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmtFiles, 1, sessionId);

    outData.files.clear();
    while (sqlite3_step(stmtFiles) == SQLITE_ROW) {
        FileRecord r;
        r.id = sqlite3_column_int64(stmtFiles, 0);
        r.parentId = sqlite3_column_int64(stmtFiles, 1);
        r.name = safe_column_text(stmtFiles, 2);
        r.extension = safe_column_text(stmtFiles, 3);
        r.path = safe_column_text(stmtFiles, 4);
        r.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmtFiles, 5));
        r.startSector = static_cast<uint64_t>(sqlite3_column_int64(stmtFiles, 6));
        r.endSector = static_cast<uint64_t>(sqlite3_column_int64(stmtFiles, 7));
        r.status = sqlite3_column_int(stmtFiles, 8);
        r.confidence = sqlite3_column_int(stmtFiles, 9);
        r.category = safe_column_text(stmtFiles, 10);
        r.source = safe_column_text(stmtFiles, 11);
        r.createdAt = sqlite3_column_int64(stmtFiles, 12);
        r.modifiedAt = sqlite3_column_int64(stmtFiles, 13);
        outData.files.push_back(r);
    }
    sqlite3_finalize(stmtFiles);

    return true;
}

bool ScanSessionManager::updateSessionStatus(int64_t sessionId, int status) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;

    const char* sql = "UPDATE scan_sessions SET status = ?, updated_at = ? WHERE id = ?";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, sessionId);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

} // namespace wolf
