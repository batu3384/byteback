#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include "wolf_fs.h"
#include "wolf_carver.h"

namespace wolf {

class ScanCoordinator {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;
    
    ScanCoordinator();
    ~ScanCoordinator();

    // badSectorOut (optional): receives the reader's current bad-sector
    // sample list on every progress tick so the UI map can render it.
    void startScan(const std::string& drivePath, const std::string& scanType,
                   FileSystemParser::FileRecordCallback onFileFound,
                   ProgressCallback onProgress,
                   std::vector<uint64_t>* badSectorOut = nullptr);
    
    void stopScan();

private:
    std::thread scanThread;
    std::atomic<bool> isRunning;

    void scanWorker(std::string drivePath, std::string scanType,
                    FileSystemParser::FileRecordCallback onFileFound,
                    ProgressCallback onProgress,
                    std::vector<uint64_t>* badSectorOut);
};

} // namespace wolf
