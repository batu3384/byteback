#include "byteback_db.h"
#include "byteback_io.h"
#include "crypto/byteback_md5.h"
#include "../../third_party/sqlite3.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace byteback {

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
    const char* legacy = R"(CREATE VIRTUAL TABLE IF NOT EXISTS content_fts USING fts5(body);)";
    const char* chunk = R"(CREATE VIRTUAL TABLE IF NOT EXISTS content_chunk_fts USING fts5(body, file_id UNINDEXED);)";
    if (sqlite3_exec(db, legacy, nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    return sqlite3_exec(db, chunk, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void deleteChunks(sqlite3* db, int64_t fileId) {
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT rowid FROM content_chunk_fts WHERE file_id = ?", -1, &sel, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(sel, 1, fileId);
    std::vector<int64_t> ids;
    while (sqlite3_step(sel) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(sel, 0));
    sqlite3_finalize(sel);
    for (int64_t id : ids) {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM content_chunk_fts WHERE rowid = ?", -1, &del, nullptr) != SQLITE_OK) {
            continue;
        }
        sqlite3_bind_int64(del, 1, id);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
}

std::string hashContentPrefix(const uint8_t* data, size_t len, uint64_t actualSize) {
    if (!data || len == 0 || actualSize == 0) return {};
    const size_t n = std::min<size_t>({len, size_t{64 * 1024}, static_cast<size_t>(actualSize)});
    crypto::Md5 md;
    md.update(data, n);
    return md.finalHex();
}

bool hashFromRuns(DiskReader* reader, const FileRecord& fr, std::string& out) {
    if (!reader || fr.runs.empty() || fr.sizeBytes == 0) return false;
    uint32_t ss = reader->getSectorSize();
    if (ss == 0) ss = 512;
    const size_t want = static_cast<size_t>((std::min)(fr.sizeBytes, uint64_t{64 * 1024}));
    std::vector<uint8_t> buf(want);
    size_t got = 0;
    bool first = true;
    for (const auto& run : fr.runs) {
        if (got >= want) break;
        uint64_t off = run.startSector * static_cast<uint64_t>(ss);
        uint64_t runBytes = run.sectorCount * static_cast<uint64_t>(ss);
        if (first) {
            if (fr.startByteOffset >= runBytes) return false;
            off += fr.startByteOffset;
            runBytes -= fr.startByteOffset;
            first = false;
        }
        if (run.byteCount > 0 && run.byteCount < runBytes) runBytes = run.byteCount;
        const size_t take = static_cast<size_t>((std::min)(static_cast<uint64_t>(want - got), runBytes));
        if (take == 0) continue;
        auto res = reader->readBytes(off, static_cast<uint32_t>(take), buf.data() + got);
        if (!readComplete(res, take)) break;
        got += take;
    }
    if (got == 0) return false;
    out = hashContentPrefix(buf.data(), got, fr.sizeBytes);
    return !out.empty();
}
} // namespace

bool MetadataStore::replaceContentChunks(int64_t fileId, const std::vector<std::string>& chunks) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_ || fileId <= 0) return false;
    ensureContentFtsIndex(db_);
    deleteChunks(db_, fileId);
    if (chunks.empty()) return true;
    return appendContentChunks(fileId, chunks);
}

bool MetadataStore::appendContentChunks(int64_t fileId, const std::vector<std::string>& chunks) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_ || fileId <= 0) return false;
    ensureContentFtsIndex(db_);
    if (chunks.empty()) return true;
    const char* ins = "INSERT INTO content_chunk_fts(body, file_id) VALUES(?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, ins, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool ok = true;
    for (const auto& body : chunks) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, fileId);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataStore::upsertContentSample(int64_t scanId, int64_t fileId, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    (void)scanId;
    if (text.empty()) return replaceContentChunks(fileId, {});
    return replaceContentChunks(fileId, {text});
}

std::string MetadataStore::getContentSample(int64_t fileId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_ || fileId <= 0) return {};
    ensureContentFtsIndex(db_);
    const char* sql = "SELECT body FROM content_chunk_fts WHERE file_id = ? ORDER BY rowid LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(stmt, 1, fileId);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = safe_column_text(stmt, 0);
    sqlite3_finalize(stmt);
    if (!out.empty()) return out;

    const char* legacy = "SELECT body FROM content_fts WHERE rowid = ?";
    if (sqlite3_prepare_v2(db_, legacy, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(stmt, 1, fileId);
    if (sqlite3_step(stmt) == SQLITE_ROW) out = safe_column_text(stmt, 0);
    sqlite3_finalize(stmt);
    return out;
}

int64_t MetadataStore::getContentIndexCount(int64_t scanId) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) return 0;
    ensureContentFtsIndex(db_);
    const char* sql = R"(
        SELECT COUNT(*) FROM files f
        WHERE f.scan_id = ? AND (
          EXISTS (SELECT 1 FROM content_chunk_fts c WHERE c.file_id = f.id)
          OR EXISTS (SELECT 1 FROM content_fts c WHERE c.rowid = f.id)
        )
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
    std::lock_guard<std::recursive_mutex> lock(mu_);
    int64_t files = getFileCount(scanId);
    if (files <= 0) return false;
    return getContentIndexCount(scanId) >= files;
}

std::vector<int64_t> MetadataStore::searchContentFts(int64_t scanId, const std::string& query,
                                                    int offset, int limit) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<int64_t> ids;
    if (!db_ || query.empty() || limit <= 0) return ids;
    ensureContentFtsIndex(db_);
    std::string match = buildFtsMatch(query);
    const char* sql = R"(
        SELECT DISTINCT f.id FROM files f
        INNER JOIN content_chunk_fts c ON f.id = c.file_id
        WHERE f.scan_id = ? AND content_chunk_fts MATCH ?
        ORDER BY f.id LIMIT ? OFFSET ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, scanId);
        sqlite3_bind_text(stmt, 2, match.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
        sqlite3_bind_int(stmt, 4, offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ids.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    if (!ids.empty()) return ids;

    const char* legacy = R"(
        SELECT f.id FROM files f
        INNER JOIN content_fts ON f.id = content_fts.rowid
        WHERE f.scan_id = ? AND content_fts MATCH ?
        ORDER BY f.id LIMIT ? OFFSET ?
    )";
    if (sqlite3_prepare_v2(db_, legacy, -1, &stmt, nullptr) != SQLITE_OK) return ids;
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

bool MetadataStore::trySetContentHash(int64_t fileId, const std::string& hash) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_ || fileId <= 0 || hash.empty()) return false;
    const char* sql =
        "UPDATE files SET content_hash = ? WHERE id = ? AND (content_hash IS NULL OR content_hash = '')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileId);
    const int before = sqlite3_total_changes(db_);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_total_changes(db_) > before;
}

int MetadataStore::hashEmptyContent(int64_t scanId, DiskReader* reader) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_ || scanId <= 0) return 0;
    const char* sql =
        "SELECT id FROM files WHERE scan_id = ? AND (content_hash IS NULL OR content_hash = '')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, scanId);
    std::vector<int64_t> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);

    int hashed = 0;
    for (int64_t id : ids) {
        FileRecord fr = getFileById(id, scanId);
        if (fr.id <= 0) continue;
        std::string h;
        if (!fr.residentData.empty()) {
            h = hashContentPrefix(fr.residentData.data(), fr.residentData.size(), fr.sizeBytes);
        } else if (reader) {
            hashFromRuns(reader, fr, h);
        }
        if (!h.empty() && trySetContentHash(id, h)) ++hashed;
    }
    return hashed;
}

} // namespace byteback
