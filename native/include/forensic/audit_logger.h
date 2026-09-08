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
    // Primary process-wide instance. CA-028 flush hardening also allows
    // standalone construction (tests, embedded loggers): a local instance
    // models a separate session without touching the singleton.
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

    // CA-028: JS-origin audit events (logAuditEvent bridge export). Validates
    // the event token (UPPERCASE/underscore first token, printable ASCII
    // payload, <=512 chars, non-empty) and writes the line with a "JS" origin
    // tag in the existing "[ts] EVENT | <payload>" convention:
    //   "[ts] EVENT | JS | <event>"
    // Returns false when the event was rejected (nothing written).
    bool LogEventFromBridge(const std::string& event);

    // Calculate SHA-256 and log file recovery
    void LogFileRecovered(const std::string& filePath, const uint8_t* data, size_t size);

    // Calculate SHA256 of data
    static std::string CalculateSHA256(const uint8_t* data, size_t size);

    // CA-028 flush hardening: synchronously drain the async queue into the
    // hash chain + file. Safe against the worker thread (same-mutex
    // serialization); a hard crash after this call loses nothing queued so far.
    void FlushPending();

    // Standalone construction (CA-028): a local instance models an independent
    // session — this is what lets tests simulate a crash-killed logger without
    // terminating the process-wide singleton.
    AuditLogger();
    ~AuditLogger();
    // Disable copy/move
    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

private:
    void ProcessQueue();

    struct LogEntry {
        std::string message;
    };

    // critical=true (CA-028 crash-evidence categories: RECOVER*/WIPE*/
    // BITLOCKER*/FILE_RECOVERED) bypasses the queue and writes through the
    // hash chain synchronously on the calling thread.
    void EnqueueLog(const std::string& message, bool critical = false);
    // Caller must hold queueMutex_: hash-chain fold + line write + flush.
    void WriteEntryLocked(const std::string& message);

    std::queue<LogEntry> logQueue_;    std::mutex queueMutex_;
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
