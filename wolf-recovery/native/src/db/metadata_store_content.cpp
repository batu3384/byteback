#include "wolf_db.h"
#include "../../third_party/sqlite3.h"
#include <cctype>
#include <string>
#include <vector>

namespace wolf {

namespace {
std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}

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

bool ensureContentFtsIndex(sqlite3* db) {
    const char* ftsSql = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS content_fts USING fts5(body);
    )";
    return sqlite3_exec(db, ftsSql, nullptr, nullptr, nullptr) == SQLITE_OK;
}
} // namespace

bool MetadataStore::upsertContentSample(int64_t scanId, int64_t fileId, const std::string& text) {
    (void)scanId;
    if (!db_ || fileId <= 0) return false;
    ensureContentFtsIndex(db_);

    if (!getContentSample(fileId).empty()) {
        const char* del = "INSERT INTO content_fts(content_fts, rowid, body) VALUES('delete', ?, '')";
        sqlite3_stmt* delStmt = nullptr;
        if (sqlite3_prepare_v2(db_, del, -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(delStmt, 1, fileId);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }
    }

    const char* ins = "INSERT INTO content_fts(rowid, body) VALUES(?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, ins, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_text(stmt, 2, text.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string MetadataStore::getContentSample(int64_t fileId) {
    if (!db_ || fileId <= 0) return {};
    const char* sql = "SELECT body FROM content_fts WHERE rowid = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(stmt, 1, fileId);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = safe_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t MetadataStore::getContentIndexCount(int64_t scanId) {
    if (!db_) return 0;
    const char* sql = R"(
        SELECT COUNT(*) FROM files f
        INNER JOIN content_fts c ON f.id = c.rowid
        WHERE f.scan_id = ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, scanId);
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

bool MetadataStore::isContentIndexComplete(int64_t scanId) {
    int64_t files = getFileCount(scanId);
    if (files <= 0) return false;
    return getContentIndexCount(scanId) >= files;
}

std::vector<int64_t> MetadataStore::searchContentFts(int64_t scanId, const std::string& query,
                                                    int offset, int limit) {
    std::vector<int64_t> ids;
    if (!db_ || query.empty() || limit <= 0) return ids;
    std::string match = buildFtsMatch(query);
    const char* sql = R"(
        SELECT f.id FROM files f
        INNER JOIN content_fts ON f.id = content_fts.rowid
        WHERE f.scan_id = ? AND content_fts MATCH ?
        ORDER BY f.id LIMIT ? OFFSET ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return ids;
    sqlite3_bind_int64(stmt, 1, scanId);
    sqlite3_bind_text(stmt, 2, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    sqlite3_bind_int(stmt, 4, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

} // namespace wolf
