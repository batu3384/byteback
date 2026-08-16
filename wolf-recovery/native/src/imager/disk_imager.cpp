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

void DiskImager::startImaging(int driveIndex, const std::string& destPath, ProgressCallback onProgress,
                              ImageFormat format) {
    if (isRunning_) return;
    isRunning_ = true;
    lastImageMd5_.clear();
    imagingThread_ = std::thread(&DiskImager::imagingWorker, this, driveIndex, destPath, onProgress, format);
}

void DiskImager::stopImaging() {
    if (isRunning_) {
        isRunning_ = false;
        if (imagingThread_.joinable()) {
            imagingThread_.join();
        }
    }
}

void DiskImager::imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress, ImageFormat format) {
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

    std::ofstream rawOut;          // Raw format
    std::unique_ptr<EwfWriter> ewf; // EWF format

    if (format == ImageFormat::Ewf) {
        ewf = std::make_unique<EwfWriter>();
        EwfOptions opts;
        opts.examiner = "Wolf Recovery";
        if (!ewf->open(destPath, totalSectors, sectorSize, opts)) {
            isRunning_ = false;
            return;
        }
    } else {
        rawOut.open(destPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!rawOut.is_open()) {
            isRunning_ = false;
            return;
        }
    }

    uint64_t sector = 0;

    while (sector < totalSectors) {
        if (!isRunning_) break;

        uint32_t sectorsToRead = std::min<uint64_t>(chunkSectors, totalSectors - sector);
        uint32_t bytesToRead = sectorsToRead * sectorSize;

        auto t1 = std::chrono::high_resolution_clock::now();
        auto res = reader.readSectors(sector * sectorSize, bytesToRead, poolBuf->data());
        auto t2 = std::chrono::high_resolution_clock::now();

        auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

        if (!res.success || res.bytesRead == 0) {
            badSectorReads_.fetch_add(sectorsToRead);
        }

        if (res.success && res.bytesRead > 0) {
            if (ewf) ewf->write(poolBuf->data(), res.bytesRead);
            else rawOut.write(reinterpret_cast<const char*>(poolBuf->data()), res.bytesRead);
        } else {
            // Write zeros for bad sectors to maintain image geometry
            std::vector<char> zeros(bytesToRead, 0);
            if (ewf) ewf->write(reinterpret_cast<const uint8_t*>(zeros.data()), zeros.size());
            else rawOut.write(zeros.data(), zeros.size());
        }

        sector += sectorsToRead;

        // Report progress
        onProgress(sector, totalSectors);
    }

    if (ewf) {
        if (ewf->finish()) {
            lastImageMd5_ = ewf->md5Hex();
        }
    } else {
        rawOut.close();
    }

    // CA-003 fix: the loop's final progress event fires BEFORE finish()
    // computes the digest, so the MD5 field always arrived empty. Emit one
    // more completion event after the digest exists — the UI keys off this
    // one to show the chain-of-custody panel.
    onProgress(totalSectors, totalSectors);

    isRunning_ = false;
}

} // namespace wolf
