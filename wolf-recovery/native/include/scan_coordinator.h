#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include "wolf_fs.h"
#include "wolf_carver.h"

namespace wolf {

class ScanCoordinator {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;
    
    ScanCoordinator();
    ~ScanCoordinator();

    void startScan(const std::string& drivePath, const std::string& scanType, 
                   FileSystemParser::FileRecordCallback onFileFound,
                   ProgressCallback onProgress);
    
    void stopScan();

private:
    std::thread scanThread;
    std::atomic<bool> isRunning;

    void scanWorker(std::string drivePath, std::string scanType, 
                    FileSystemParser::FileRecordCallback onFileFound,
                    ProgressCallback onProgress);
};

} // namespace wolf
