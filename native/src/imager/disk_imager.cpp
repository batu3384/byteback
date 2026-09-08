#include "byteback_imager.h"
#include "byteback_memory.h"
#include "crypto/byteback_md5.h"
#include <fstream>
#include <chrono>
#include <cstring>
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

void DiskImager::startImagingFromReader(DiskReader& reader, const std::string& destPath,
                                        ProgressCallback onProgress, ImageFormat format,
                                        const EwfOptions& ewfOpts) {
    stopImaging();
    isRunning_ = true;
    lastImageMd5_.clear();
    // Lifetime contract: `reader` is captured by reference and MUST outlive
    // the imaging thread (until stopImaging() joins it or the run completes).
    // The production path (bridge_imager.cpp) only uses startImaging(driveIndex),
    // which opens its own reader inside the worker — this overload is for
    // tests/pre-loaded images whose caller keeps the reader alive.
    imagingThread_ = std::thread([this, &reader, destPath, onProgress, format, ewfOpts]() {
        imagingRun(reader, destPath, onProgress, format, ewfOpts);
    });
}

void DiskImager::requestStop() {
    isRunning_ = false;
}

void DiskImager::stopImaging() {
    requestStop();
    if (!imagingThread_.joinable()) return;
    if (imagingThread_.get_id() == std::this_thread::get_id()) {
        // Self-stop from a progress callback: joining would deadlock, so the
        // thread detaches and finishes the current chunk on its own. Caveat:
        // the DiskImager and any captured reader must therefore outlive that
        // detached run — do not destroy the imager from inside its own callback.
        imagingThread_.detach();
        return;
    }
    imagingThread_.join();
}

void DiskImager::imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress,
                             ImageFormat format, EwfOptions ewfOpts) {
    DiskReader reader;
    if (!reader.openDrive(driveIndex)) {
        if (onProgress) onProgress(0, 0);
        isRunning_ = false;
        return;
    }
    imagingRun(reader, destPath, onProgress, format, ewfOpts);
}

void DiskImager::imagingRun(DiskReader& reader, const std::string& destPath, ProgressCallback onProgress,
                            ImageFormat format, EwfOptions ewfOpts) {
    auto fail = [&]() {
        if (onProgress) onProgress(0, 0);
        isRunning_ = false;
    };

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

        // CA-037: a failed 16MB chunk used to be zero-filled wholesale — one
        // bad sector silently punched a 16MB hole in the forensic image.
        // Retry halving down to single sectors (same policy as the carver's
        // CA-007 path), zero-fill only the sectors that truly failed, and
        // count each of them in badSectorReads_. Sub-reads are processed in
        // disk order, so the chunk buffer stays byte-exact.
        struct SubRead { uint64_t sector; uint32_t sectors; };
        std::vector<SubRead> subReads{{sector, sectorsToRead}};
        while (!subReads.empty()) {
            const auto sub = subReads.back();
            subReads.pop_back();
            const uint32_t want = sub.sectors * sectorSize;
            uint8_t* dst = poolBuf->data() + (sub.sector - sector) * sectorSize;
            auto res = reader.readSectors(sub.sector * sectorSize, want, dst);
            if ((!res.success || res.bytesRead < want) && sub.sectors > 1) {
                const uint32_t half = sub.sectors / 2;
                subReads.push_back({sub.sector + half, sub.sectors - half});
                subReads.push_back({sub.sector, half});
                continue;
            }
            if (!res.success || res.bytesRead == 0) {
                badSectorReads_.fetch_add(sub.sectors);
                std::memset(dst, 0, want);
                continue;
            }
            if (res.paddedZeros) badSectorReads_.fetch_add(1);
            if (res.bytesRead < want) {
                // Short-but-successful read (device EOD, flaky link): the
                // chunk advances a full chunk regardless, so the tail must be
                // zero-padded — writing only bytesRead would shift every
                // following byte and silently corrupt the rest of the image.
                const size_t missing = want - res.bytesRead;
                std::memset(dst + res.bytesRead, 0, missing);
                badSectorReads_.fetch_add(missing / sectorSize);
            }
        }

        const uint8_t* outPtr = poolBuf->data();
        size_t outLen = bytesToRead;

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
        if (onProgress) onProgress(sector, totalSectors);
    }

    if (!writeOk) {
        fail();
        return;
    }

    // requestStop mid-image: sector never reached totalSectors. Close the
    // output honestly — a partial EWF keeps its tables (still readable) but
    // gets NO digest/done section and NO MD5, so a truncated acquisition can
    // never present itself as a verified image.
    const bool stoppedEarly = sector < totalSectors;

    if (ewf) {
        if (stoppedEarly) {
            if (!ewf->abort()) {
                fail();
                return;
            }
        } else if (!ewf->finish()) {
            fail();
            return;
        }
        if (!stoppedEarly) lastImageMd5_ = ewf->md5Hex();
    } else {
        rawOut.close();
        if (!stoppedEarly) lastImageMd5_ = rawMd5.finalHex();
    }

    // Completion tick (current == total) is the caller's end-of-job signal
    // (the NAPI bridge releases its ThreadSafeFunction on it), so it is sent
    // even for a cancelled run — the empty lastImageMd5() marks it unverified.
    if (onProgress) onProgress(totalSectors, totalSectors);
    isRunning_ = false;
}

} // namespace byteback
