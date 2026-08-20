#pragma once

#include "byteback_fs.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace byteback {

struct NtfsLogHint {
    std::string name;
    uint64_t mftRef = UINT64_MAX;
};

// Collects filename (+ optional MFT ref) hints from $LogFile without emitting recoverable files.
class NtfsLogHintCollector {
public:
    void add(const std::string& name, uint64_t mftRef = UINT64_MAX);
    bool findByName(const std::string& name, uint64_t* mftRefOut = nullptr) const;
    bool findByMftRef(uint64_t mftRef, std::string* nameOut = nullptr) const;
    size_t size() const { return byLowerName_.size(); }

private:
    static std::string lowerKey(const std::string& name);
    std::unordered_map<std::string, NtfsLogHint> byLowerName_;
    std::unordered_map<uint64_t, std::string> byMftRef_;
};

// Scan $LogFile. When collector is set, hints go there; standalone ntfs_logfile file
// records are not emitted (restart page still uses callback).
void scanNtfsLogFileHints(DiskReader& reader, uint64_t partitionOffsetBytes,
                          FileSystemParser::FileRecordCallback callback,
                          std::atomic<bool>* isRunning,
                          NtfsLogHintCollector* collector = nullptr);

} // namespace byteback
