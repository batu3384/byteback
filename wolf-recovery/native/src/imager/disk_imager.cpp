#include "wolf_imager.h"
#include "wolf_memory.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>

namespace wolf {

DiskImager::DiskImager() : isRunning_(false) {}

DiskImager::~DiskImager() {
    stopImaging();
}

void DiskImager::startImaging(int driveIndex, const std::string& destPath, ProgressCallback onProgress) {
    if (isRunning_) return;
    isRunning_ = true;
    imagingThread_ = std::thread(&DiskImager::imagingWorker, this, driveIndex, destPath, onProgress);
}

void DiskImager::stopImaging() {
    if (isRunning_) {
        isRunning_ = false;
        if (imagingThread_.joinable()) {
            imagingThread_.join();
        }
    }
}

void DiskImager::imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress) {
    DiskReader reader;
    if (!reader.openDrive(driveIndex)) {
        isRunning_ = false;
        return;
    }

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = diskSize / sectorSize;

    // Use a large buffer for fast sequential reading (e.g. 16MB)
    const uint32_t chunkSectors = (16 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);

    // Open destination file for raw writing
    std::ofstream outFile(destPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        isRunning_ = false;
        return;
    }

    uint64_t sector = 0;
    int highLatencyCount = 0;
    
    while (sector < totalSectors) {
        if (!isRunning_) break;

        uint32_t sectorsToRead = std::min<uint64_t>(chunkSectors, totalSectors - sector);
        uint32_t bytesToRead = sectorsToRead * sectorSize;

        auto t1 = std::chrono::high_resolution_clock::now();
        auto res = reader.readSectors(sector * sectorSize, bytesToRead, poolBuf->data());
        auto t2 = std::chrono::high_resolution_clock::now();
        
        auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        
        if (res.success && res.bytesRead > 0) {
            outFile.write(reinterpret_cast<const char*>(poolBuf->data()), res.bytesRead);
        } else {
            // Write zeros for bad sectors to maintain image geometry
            std::vector<char> zeros(bytesToRead, 0);
            outFile.write(zeros.data(), zeros.size());
        }
        
        sector += sectorsToRead;



        // Report progress
        onProgress(sector, totalSectors);
    }

    outFile.close();
    isRunning_ = false;
}

} // namespace wolf
