#include "byteback_imager.h"
#include "byteback_memory.h"
#include "crypto/byteback_md5.h"
#include <fstream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <memory>

namespace byteback {

DiskImager::DiskImager() : isRunning_(false) {}

DiskImager::~DiskImager() {
    stopImaging();
}

void DiskImager::startImaging(int driveIndex, const std::string& destPath, ProgressCallback onProgress,
                              ImageFormat format, const EwfOptions& ewfOpts) {
    stopImaging();
    isRunning_ = true;
    lastImageMd5_.clear();
    imagingThread_ = std::thread(&DiskImager::imagingWorker, this, driveIndex, destPath, onProgress, format, ewfOpts);
}

void DiskImager::requestStop() {
    isRunning_ = false;
}

void DiskImager::stopImaging() {
    requestStop();
    if (!imagingThread_.joinable()) return;
    if (imagingThread_.get_id() == std::this_thread::get_id()) {
        imagingThread_.detach();
        return;
    }
    imagingThread_.join();
}

void DiskImager::imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress,
                             ImageFormat format, EwfOptions ewfOpts) {
    auto fail = [&]() {
        if (onProgress) onProgress(0, 0);
        isRunning_ = false;
    };

    DiskReader reader;
    if (!reader.openDrive(driveIndex)) {
        fail();
        return;
    }

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = diskSize / sectorSize;

    const uint32_t chunkSectors = (16 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);

    std::ofstream rawOut;
    std::unique_ptr<EwfWriter> ewf;
    crypto::Md5 rawMd5;

    if (format == ImageFormat::Ewf) {
        ewf = std::make_unique<EwfWriter>();
        if (ewfOpts.examiner.empty()) ewfOpts.examiner = "Byteback";
        if (!ewf->open(destPath, totalSectors, sectorSize, ewfOpts)) {
            fail();
            return;
        }
    } else {
        rawOut.open(destPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!rawOut.is_open()) {
            fail();
            return;
        }
    }

    uint64_t sector = 0;
    bool writeOk = true;

    while (sector < totalSectors) {
        if (!isRunning_) break;

        uint32_t sectorsToRead = std::min<uint64_t>(chunkSectors, totalSectors - sector);
        uint32_t bytesToRead = sectorsToRead * sectorSize;

        auto res = reader.readSectors(sector * sectorSize, bytesToRead, poolBuf->data());

        const uint8_t* outPtr = poolBuf->data();
        size_t outLen = bytesToRead;
        std::vector<uint8_t> zeros;
        if (!res.success || res.bytesRead == 0) {
            badSectorReads_.fetch_add(sectorsToRead);
            zeros.assign(bytesToRead, 0);
            outPtr = zeros.data();
            outLen = bytesToRead;
        } else {
            outLen = static_cast<size_t>(res.bytesRead);
            if (res.paddedZeros) badSectorReads_.fetch_add(1);
        }

        if (ewf) {
            if (!ewf->write(outPtr, outLen)) {
                writeOk = false;
                break;
            }
        } else {
            rawOut.write(reinterpret_cast<const char*>(outPtr), static_cast<std::streamsize>(outLen));
            rawMd5.update(outPtr, outLen);
            if (!rawOut.good()) {
                writeOk = false;
                break;
            }
        }

        sector += sectorsToRead;
        onProgress(sector, totalSectors);
    }

    if (!writeOk) {
        fail();
        return;
    }

    if (ewf) {
        if (!ewf->finish()) {
            fail();
            return;
        }
        lastImageMd5_ = ewf->md5Hex();
    } else {
        rawOut.close();
        lastImageMd5_ = rawMd5.finalHex();
    }

    onProgress(totalSectors, totalSectors);
    isRunning_ = false;
}

} // namespace byteback
