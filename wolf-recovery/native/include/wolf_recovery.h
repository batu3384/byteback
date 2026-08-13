#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <string>
#include <functional>
#include <atomic>

namespace wolf {

struct RecoveryResult {
    bool success;
    std::string destPath;
    uint64_t bytesRecovered;
    std::string error;
    std::string md5Hash;
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
};

} // namespace wolf
