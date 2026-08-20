#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <string>
#include <functional>
#include <atomic>
#include <vector>

namespace wolf {

struct RecoveryResult {
    bool success = false;
    std::string destPath;
    uint64_t bytesRecovered = 0;
    std::string error;
    std::string md5Hash;
    bool zeroFilled = false; // at least one read was padded/failed
};

inline bool countsAsRecovered(const RecoveryResult& r) {
    return r.success && !r.zeroFilled;
}

bool loadRecoverRecord(MetadataStore& store, int64_t scanId, int64_t fileId,
                       FileRecord& out, std::string& err);

struct BatchRecoverySummary {
    int succeeded = 0;
    int failed = 0;
    std::vector<RecoveryResult> results;
};

class RecoveryEngine {
public:
    RecoveryEngine();
    ~RecoveryEngine();

    using ProgressCallback = std::function<void(uint64_t bytesWritten, uint64_t totalBytes)>;

    // Recover a single file from disk using its data runs (cluster chain)
    RecoveryResult recoverFile(DiskReader& reader, const FileRecord& record, 
                               const std::string& destDir, ProgressCallback onProgress = nullptr,
                               std::atomic<bool>* isRunning = nullptr);

    // Recover a carved file (contiguous sectors, no cluster chain)
    RecoveryResult recoverCarvedFile(DiskReader& reader, const FileRecord& record,
                                     const std::string& destDir, ProgressCallback onProgress = nullptr,
                                     std::atomic<bool>* isRunning = nullptr);

    BatchRecoverySummary recoverFilesBatch(DiskReader& reader,
                                           const std::vector<FileRecord>& records,
                                           const std::string& destDir,
                                           ProgressCallback onProgress = nullptr,
                                           std::atomic<bool>* isRunning = nullptr);
};

} // namespace wolf
