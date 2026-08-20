#include "forensic/nsrl_lookup.h"
#include "sqlite3.h"

#include <cctype>
#include <fstream>

namespace forensic {

namespace {

std::string sqlitePathFor(const std::string& src) {
    return src + ".wolf-nsrl.sqlite";
}

} // namespace

NsrlLookup::~NsrlLookup() {
    closeDb();
}

void NsrlLookup::closeDb() {
    if (!db_) return;
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
}

bool NsrlLookup::ensureDb(const std::string& sqlitePath) {
    closeDb();
    sqlite3* db = nullptr;
    if (sqlite3_open(sqlitePath.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", nullptr, nullptr, nullptr);
    if (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS nsrl(md5 TEXT PRIMARY KEY);", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    db_ = db;
    return true;
}

bool NsrlLookup::normalizeMd5(std::string& hex) {
    if (hex.size() != 32) return false;
    for (char& c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

bool NsrlLookup::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    if (!ensureDb(sqlitePathFor(path))) return false;
    sqlite3* db = static_cast<sqlite3*>(db_);
    sqlite3_exec(db, "DELETE FROM nsrl;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO nsrl(md5) VALUES(?)", -1, &ins, nullptr) != SQLITE_OK) {
        return false;
    }

    auto strip = [](std::string token) {
        while (!token.empty() && (token.front() == '"' || token.front() == ' ' || token.front() == '\t')) {
            token.erase(token.begin());
        }
        while (!token.empty() && (token.back() == '"' || token.back() == ' ' ||
                                  token.back() == '\t' || token.back() == '\r')) {
            token.pop_back();
        }
        return token;
    };

    lastPath_ = path;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t start = 0;
        while (start <= line.size()) {
            const auto comma = line.find(',', start);
            const std::string token = strip(
                comma == std::string::npos ? line.substr(start) : line.substr(start, comma - start));
            std::string hex = token;
            if (normalizeMd5(hex)) {
                sqlite3_reset(ins);
                sqlite3_bind_text(ins, 1, hex.c_str(), 32, SQLITE_TRANSIENT);
                sqlite3_step(ins);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    sqlite3_finalize(ins);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

void NsrlLookup::clear() {
    closeDb();
    lastPath_.clear();
}

bool NsrlLookup::contains(const std::string& md5Hex) const {
    if (!db_) return false;
    std::string key = md5Hex;
    if (!normalizeMd5(key)) return false;
    sqlite3_stmt* st = nullptr;
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM nsrl WHERE md5=? LIMIT 1", -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(), 32, SQLITE_TRANSIENT);
    bool hit = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return hit;
}

size_t NsrlLookup::size() const {
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM nsrl", -1, &st, nullptr) != SQLITE_OK) {
        return 0;
    }
    size_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = static_cast<size_t>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return n;
}

} // namespace forensic
