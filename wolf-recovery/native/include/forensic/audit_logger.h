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

    struct LogEvent {
        std::string message;
    };

    void EnqueueLog(const std::string& message);

    std::queue<LogEvent> logQueue_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    bool stopThread_ = false;
    std::thread workerThread_;

    std::ofstream logFile_;
    
    std::string previousHash_;
};

} // namespace forensic
