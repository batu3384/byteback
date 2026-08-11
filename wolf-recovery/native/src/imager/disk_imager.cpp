#include "wolf_imager.h"
#include "wolf_memory.h"
#include <fstream>
#include <iostream>

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

    for (uint64_t sector = 0; sector < totalSectors; sector += chunkSectors) {
        if (!isRunning_) break;

        uint32_t sectorsToRead = std::min<uint64_t>(chunkSectors, totalSectors - sector);
        uint32_t bytesToRead = sectorsToRead * sectorSize;

        auto res = reader.readSectors(sector * sectorSize, bytesToRead, poolBuf->data());
        
        if (res.success && res.bytesRead > 0) {
            outFile.write(reinterpret_cast<const char*>(poolBuf->data()), res.bytesRead);
        } else {
            // Write zeros for bad sectors to maintain image geometry
            // In a real scenario we'd do this sector by sector, but for simplicity:
            std::vector<char> zeros(bytesToRead, 0);
            outFile.write(zeros.data(), zeros.size());
        }

        // Report progress
        onProgress(sector + sectorsToRead, totalSectors);
    }

    outFile.close();
    isRunning_ = false;
}

} // namespace wolf
