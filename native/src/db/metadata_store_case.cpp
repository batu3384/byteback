#include "byteback_db.h"
#include "../../third_party/sqlite3.h"
#include <ctime>
#include <mutex>
#include <string>

namespace byteback {

namespace {
std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return txt ? txt : "";
}
} // namespace

CaseInfo MetadataStore::getCaseInfo() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    CaseInfo c;
    if (!db_) return c;
    const char* sql = R"(
        SELECT case_number, investigator, agency, notes, created_at, updated_at
        FROM case_info WHERE id = 1
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return c;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        c.caseNumber = safe_column_text(stmt, 0);
        c.investigator = safe_column_text(stmt, 1);
        c.agency = safe_column_text(stmt, 2);
        c.notes = safe_column_text(stmt, 3);
        c.createdAt = sqlite3_column_int64(stmt, 4);
        c.updatedAt = sqlite3_column_int64(stmt, 5);
    }
    sqlite3_finalize(stmt);
    return c;
}

bool MetadataStore::setCaseInfo(const CaseInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) return false;
    CaseInfo existing = getCaseInfo();
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t created = existing.createdAt > 0 ? existing.createdAt : now;

    const char* sql = R"(
        INSERT INTO case_info (id, case_number, investigator, agency, notes, created_at, updated_at)
        VALUES (1, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            case_number = excluded.case_number,
            investigator = excluded.investigator,
            agency = excluded.agency,
            notes = excluded.notes,
            created_at = CASE
                WHEN case_info.created_at = 0 THEN excluded.created_at
                ELSE case_info.created_at
            END,
            updated_at = excluded.updated_at
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, info.caseNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, info.investigator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, info.agency.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, info.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, created);
    sqlite3_bind_int64(stmt, 6, now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

} // namespace byteback
