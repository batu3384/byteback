#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include "wolf_db.h"

struct sqlite3;

namespace wolf {

struct SessionData {
    ScanState state;
    std::vector<FileRecord> files;
};

class ScanSessionManager {
public:
    ScanSessionManager();
    ~ScanSessionManager();

    // Initialize the session manager with a database path
    bool initialize(const std::string& dbPath);
    void close();
    bool isOpen() const;

    // Create a new session, returns sessionId
    int64_t createSession(int driveIndex, const std::string& scanType, uint64_t totalSectors);

    // Save state periodically to allow crash recovery
    // This updates the scanned sectors and inserts any newly found files
    bool saveState(int64_t sessionId, uint64_t scannedSectors, const std::vector<FileRecord>& newFiles);

    // Load state to resume an interrupted scan
    bool loadState(int64_t sessionId, SessionData& outData);

    // Mark session as complete or failed
    bool updateSessionStatus(int64_t sessionId, int status);

private:
    bool createTables();
    int64_t insertFileInternal(int64_t sessionId, const FileRecord& record);

    sqlite3* db_;
    std::mutex dbMutex_;
};

} // namespace wolf
