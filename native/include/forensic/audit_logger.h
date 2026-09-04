#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <fstream>
#include <cstdint>

namespace forensic {

class AuditLogger {
public:
    static AuditLogger& GetInstance() {
        static AuditLogger instance;
        return instance;
    }

    // Initialize the logger with an output file path
    void Initialize(const std::string& logFilePath);
    
    // Shutdown and wait for pending logs to be written
    void Shutdown();

    // Log a disk read operation
    void LogDiskRead(const std::string& devicePath, uint64_t offset, uint64_t size);

    // Log a general forensic operation event (scan started/completed,
    // imaging finished, wipe performed, ...). The message is folded into the
    // hash chain like every other entry.
    void LogEvent(const std::string& eventMessage);

    // Calculate SHA-256 and log file recovery
    void LogFileRecovered(const std::string& filePath, const uint8_t* data, size_t size);
    
    // Calculate SHA256 of data
    static std::string CalculateSHA256(const uint8_t* data, size_t size);

private:
    AuditLogger();
    ~AuditLogger();    
    // Disable copy/move
    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    void ProcessQueue();

    struct LogEntry {
        std::string message;
    };

    void EnqueueLog(const std::string& message);

    std::queue<LogEntry> logQueue_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    bool stopThread_ = false;
    std::thread workerThread_;

    std::ofstream logFile_;

    std::string previousHash_;
};

// Runtime chain verification — re-walks the log file recomputing
// SHA256(prevHash + message) per entry. Until now "tamper-evident" required
// manual verification; this makes the property machine-checkable.
struct AuditChainVerifyResult {
    bool ok = false;        // every entry verified (and at least one exists)
    int entries = 0;        // entries verified before the walk ended/failed
    int brokenAt = 0;       // 1-based line of first failure; 0 = none
    std::string detail;     // "ok" | "empty" | "open failed" | reason
};

AuditChainVerifyResult VerifyAuditChainFile(const std::string& path);

} // namespace forensic
