#pragma once

#include "wolf_io.h"
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace wolf {

class DiskImager {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

    DiskImager();
    ~DiskImager();

    void startImaging(int driveIndex, const std::string& destPath, ProgressCallback onProgress);
    void stopImaging();

private:
    void imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress);

    std::atomic<bool> isRunning_;
    std::thread imagingThread_;
};

} // namespace wolf
