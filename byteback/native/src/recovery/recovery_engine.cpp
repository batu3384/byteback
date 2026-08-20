#include "byteback_recovery.h"
#include "byteback_memory.h"
#include "recovery/path_util.h"
#include "recovery/validation.h"
#include "crypto/byteback_md5.h"
#include "fs/ntfs_util.h"
#include "fs/vss_scanner.h"
#include "fs/virtual_raid.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace byteback {

namespace {

bool destReady(RecoveryResult& result, const std::string& destDir) {
    if (!destDirIsSafe(destDir)) {
        result.error = "unsafe destination directory";
        return false;
    }
    return true;
}

bool applyUniquePath(RecoveryResult& result, const std::string& destDir, const std::string& name) {
    result.destPath = uniqueDestPath(destDir, name);
    if (result.destPath.empty()) {
        result.error = "too many name collisions in destination";
        return false;
    }
    return true;
}

void finishRecoverWrite(RecoveryResult& result, uint64_t bytes, const std::string& md5,
                        const FileRecord& record) {
    result.bytesRecovered = bytes;
    result.md5Hash = md5;
    if (result.zeroFilled) {
        result.success = false;
        if (result.error.empty()) result.error = "short or padded read; output is incomplete";
        return;
    }
    result.success = true;
    applyPostRecoveryValidation(result, record);
}

} // namespace

bool isDiscoveryOnlySource(const std::string& source) {
    return source == "apfs_volume" || source == "apfs_container" ||
           source == "apfs_file" || source == "bitlocker_detect" ||
           source == "bitlocker_fve" || source == "vss_unbound" ||
           source == "vss_bind" || source == "vss_snapshot" ||
           source == "hfs_limit" || source == "usn_journal" ||
           source == "ntfs_logfile" || source == "ntfs_logfile_restart" ||
           source == "carver_duplicate";
}

bool bindReaderForRecord(DiskReader& reader, const FileRecord& rec, int driveIndex,
                         std::shared_ptr<VirtualRaid> raid, std::string& err) {
    err.clear();
    const std::string vss = vssDevicePathFromRecord(rec);
    if (!vss.empty()) {
        if (!reader.openVolumePath(vss)) {
            err = "VSS volume not available";
            return false;
        }
        return true;
    }
    if (raid) {
        reader.setRaidBackend(raid);
        return true;
    }
    if (driveIndex < 0) {
        err = "Disk is not open";
        return false;
    }
    if (!reader.openDrive(driveIndex)) {
        err = "could not open PhysicalDrive";
        return false;
    }
    return true;
}

void applyBoundFvek(DiskReader& dest, const DiskReader& src, const FileRecord& rec) {
    if (!vssDevicePathFromRecord(rec).empty()) return;
    dest.copyXtsFvekFrom(src);
}

RecoveryEngine::RecoveryEngine() {}
RecoveryEngine::~RecoveryEngine() {}

bool loadRecoverRecord(MetadataStore& store, int64_t scanId, int64_t fileId,
                       FileRecord& out, std::string& err) {
    if (scanId <= 0 || fileId <= 0) {
        err = "scan and file id required";
        return false;
    }
    out = store.getFileById(fileId, scanId);
    if (out.id <= 0) {
        err = "file id not in scan";
        return false;
    }
    return true;
}

RecoveryResult RecoveryEngine::recoverFile(DiskReader& reader, const FileRecord& record,
                                            const std::string& destDir, ProgressCallback onProgress,
                                            std::atomic<bool>* isRunning) {
    RecoveryResult result;
    result.success = false;
    result.bytesRecovered = 0;

    if (!destReady(result, destDir)) return result;

    if (!reader.isOpen() && !reader.hasRaidBackend()) {
        result.error = "Disk is not open";
        return result;
    }

    if (isDiscoveryOnlySource(record.source)) {
        result.error = "discovery-only record; no recoverable data";
        return result;
    }

    if (!record.residentData.empty()) {
        std::filesystem::create_directories(destDir);
        if (!applyUniquePath(result, destDir, record.name)) return result;
        std::ofstream outFile(result.destPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!outFile.is_open()) {
            result.error = "Could not open destination file: " + result.destPath;
            return result;
        }
        uint64_t n = record.residentData.size();
        if (record.sizeBytes > 0) n = std::min(n, record.sizeBytes);
        outFile.write(reinterpret_cast<const char*>(record.residentData.data()),
                      static_cast<std::streamsize>(n));
        crypto::Md5 md5ctx;
        md5ctx.update(record.residentData.data(), static_cast<size_t>(n));
        outFile.close();
        result.success = true;
        result.bytesRecovered = n;
        result.md5Hash = md5ctx.finalHex();
        if (onProgress) onProgress(n, n);
        applyPostRecoveryValidation(result, record);
        return result;
    }

    if (record.runs.empty()) {
        if (record.source == "carver" || record.source == "carver_bgc" || record.source.empty()) {
            return recoverCarvedFile(reader, record, destDir, onProgress, isRunning);
        }
        result.error = "no data runs; resident content not extracted";
        return result;
    }

    std::filesystem::create_directories(destDir);

    if (!applyUniquePath(result, destDir, record.name)) return result;

    std::ofstream outFile(result.destPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + result.destPath;
        return result;
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t totalBytes = record.sizeBytes;
    uint64_t bytesWritten = 0;

    // Read buffer — 1MB at a time
    const uint32_t readChunk = 1024 * 1024;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(readChunk);

    crypto::Md5 md5ctx;

    // CA-005 fix: LZNT1-compressed $DATA must be inflated before writing;
    // the raw on-disk bytes are not the file. Stream the compressed runs
    // into one buffer (capped), decompress, write up to realSize.
    if (record.compressed) {
        const uint64_t kMaxCompressedBuffer = 64ull * 1024 * 1024; // 64 MiB cap
        uint64_t compressedLen = 0;
        for (const auto& run : record.runs) {
            if (run.startSector == UINT64_MAX) continue; // sparse: contributes zeros
            compressedLen += (uint64_t)run.sectorCount * sectorSize;
        }
        if (compressedLen > 0 && compressedLen <= kMaxCompressedBuffer) {
            // CA-009: readSectors rounds the read length UP to sector
            // boundaries; today every `want` is already a sector multiple
            // (runs are sectorCount*sectorSize), but the invariant is implicit
            // — pay one sector of slack so a future byte-exact contributor
            // cannot overflow the tail.
            std::vector<uint8_t> compressed(static_cast<size_t>(compressedLen) + sectorSize);
            uint64_t filled = 0;
            for (const auto& run : record.runs) {
                if (isRunning && !(*isRunning)) break;
                if (run.startSector == UINT64_MAX) {
                    // Sparse gap inside a compressed stream reads as zeros.
                    uint64_t gap = (uint64_t)run.sectorCount * sectorSize;
                    uint64_t take = std::min(gap, compressedLen - filled);
                    std::memset(compressed.data() + filled, 0, static_cast<size_t>(take));
                    filled += take;
                    continue;
                }
                uint64_t want = (uint64_t)run.sectorCount * sectorSize;
                want = std::min(want, compressedLen - filled);
                if (want == 0) break;
                auto res = reader.readSectors(run.startSector * sectorSize,
                                              static_cast<uint32_t>(((want + sectorSize - 1) / sectorSize) * sectorSize),
                                              compressed.data() + filled);
                if (!res.success || res.bytesRead < want || res.paddedZeros) {
                    std::memset(compressed.data() + filled, 0, static_cast<size_t>(want));
                    result.zeroFilled = true;
                }
                filled += want;
            }

            std::vector<uint8_t> inflated;
            inflated.resize(static_cast<size_t>(std::max<uint64_t>(totalBytes, 4096)));
            int outLen = ntfs::lznt1Decompress(compressed.data(), filled,
                                               inflated.data(), inflated.size());
            if (outLen >= 0) {
                uint64_t writeLen = std::min<uint64_t>(totalBytes, static_cast<uint64_t>(outLen));
                outFile.write(reinterpret_cast<const char*>(inflated.data()),
                              static_cast<std::streamsize>(writeLen));
                md5ctx.update(inflated.data(), static_cast<size_t>(writeLen));
                outFile.close();
                finishRecoverWrite(result, writeLen, md5ctx.finalHex(), record);
                return result;
            }
            // Decompression failed: fall through to raw recovery so the user
            // still gets SOMETHING, flagged by a lower confidence upstream.
        }
        // Above the cap: fall through to raw write (documented limitation).
    }

    // Walk through each data run and read the clusters
    for (const auto& run : record.runs) {
        if (isRunning && !(*isRunning)) break;
        if (bytesWritten >= totalBytes) break;

        // Sparse run sentinel (set by the NTFS parser for runs with no
        // physical clusters). NTFS sparse files and compressed-unit gaps read
        // as zeros, so emit zeros for the whole run without touching the disk.
        if (run.startSector == UINT64_MAX) {
            uint64_t runSizeBytes = run.sectorCount * sectorSize;
            uint64_t toZero = std::min(runSizeBytes, totalBytes - bytesWritten);
            uint64_t zeroed = 0;
            const uint32_t zeroChunk = readChunk;
            std::vector<char> zeros(zeroChunk, 0);
            while (zeroed < toZero) {
                if (isRunning && !(*isRunning)) break;
                uint32_t n = (uint32_t)std::min((uint64_t)zeroChunk, toZero - zeroed);
                outFile.write(zeros.data(), n);
                md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), n);
                zeroed += n;
                bytesWritten += n;
                if (onProgress) onProgress(bytesWritten, totalBytes);
            }
            continue;
        }

        uint64_t runOffsetBytes = run.startSector * sectorSize;
        uint64_t runSizeBytes = run.sectorCount * sectorSize;
        uint64_t runBytesRead = 0;

        while (runBytesRead < runSizeBytes && bytesWritten < totalBytes) {
            if (isRunning && !(*isRunning)) break;

            uint32_t toRead = (uint32_t)std::min((uint64_t)readChunk, runSizeBytes - runBytesRead);
            // Align to sector
            toRead = ((toRead + sectorSize - 1) / sectorSize) * sectorSize;

            auto res = reader.readSectors(runOffsetBytes + runBytesRead, toRead, poolBuf->data());
            
            if (!res.success || res.bytesRead == 0) {
                result.zeroFilled = true;
                uint32_t zeroSize = std::min(toRead, (uint32_t)(totalBytes - bytesWritten));
                std::vector<char> zeros(zeroSize, 0);
                outFile.write(zeros.data(), zeroSize);
                md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
                bytesWritten += zeroSize;
                runBytesRead += toRead;
                continue;
            }
            if (res.paddedZeros) result.zeroFilled = true;

            // Don't write beyond the actual file size
            uint32_t writeSize = (uint32_t)std::min((uint64_t)res.bytesRead, totalBytes - bytesWritten);
            outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
            md5ctx.update(poolBuf->data(), writeSize);
            
            bytesWritten += writeSize;
            runBytesRead += res.bytesRead;

            if (onProgress) onProgress(bytesWritten, totalBytes);
        }
    }

    outFile.close();
    finishRecoverWrite(result, bytesWritten, md5ctx.finalHex(), record);
    return result;
}

RecoveryResult RecoveryEngine::recoverCarvedFile(DiskReader& reader, const FileRecord& record,
                                                  const std::string& destDir, ProgressCallback onProgress,
                                                  std::atomic<bool>* isRunning) {
    RecoveryResult result;
    result.success = false;
    result.bytesRecovered = 0;

    if (!destReady(result, destDir)) return result;

    if (!reader.isOpen() && !reader.hasRaidBackend()) {
        result.error = "Disk is not open";
        return result;
    }

    if (isDiscoveryOnlySource(record.source)) {
        result.error = "discovery-only record; no recoverable data";
        return result;
    }

    std::filesystem::create_directories(destDir);

    if (!applyUniquePath(result, destDir, record.name)) return result;

    std::ofstream outFile(result.destPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + result.destPath;
        return result;
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t startOffset = record.startSector * sectorSize;
    uint64_t totalBytes = record.sizeBytes;
    uint64_t bytesWritten = 0;

    const uint32_t readChunk = 1024 * 1024;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(readChunk);

    crypto::Md5 md5ctx;

    while (bytesWritten < totalBytes) {
        if (isRunning && !(*isRunning)) break;

        uint32_t toRead = (uint32_t)std::min((uint64_t)readChunk, totalBytes - bytesWritten);
        toRead = ((toRead + sectorSize - 1) / sectorSize) * sectorSize;

        auto res = reader.readSectors(startOffset + bytesWritten, toRead, poolBuf->data());
        
        if (!res.success || res.bytesRead == 0) {
            result.zeroFilled = true;
            uint32_t zeroSize = std::min(toRead, (uint32_t)(totalBytes - bytesWritten));
            std::vector<char> zeros(zeroSize, 0);
            outFile.write(zeros.data(), zeroSize);
            md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
            bytesWritten += zeroSize;
            continue;
        }
        if (res.paddedZeros) result.zeroFilled = true;

        uint32_t writeSize = (uint32_t)std::min((uint64_t)res.bytesRead, totalBytes - bytesWritten);
        outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
        md5ctx.update(poolBuf->data(), writeSize);

        bytesWritten += writeSize;
        if (onProgress) onProgress(bytesWritten, totalBytes);
    }

    outFile.close();
    finishRecoverWrite(result, bytesWritten, md5ctx.finalHex(), record);
    return result;
}

BatchRecoverySummary RecoveryEngine::recoverFilesBatch(DiskReader& reader,
                                                       const std::vector<FileRecord>& records,
                                                       const std::string& destDir,
                                                       ProgressCallback onProgress,
                                                       std::atomic<bool>* isRunning) {
    BatchRecoverySummary summary;
    summary.results.reserve(records.size());
    for (const auto& record : records) {
        if (isRunning && !(*isRunning)) break;
        RecoveryResult one = recoverFile(reader, record, destDir, onProgress, isRunning);
        summary.results.push_back(one);
        if (one.success) ++summary.succeeded;
        else ++summary.failed;
    }
    return summary;
}

} // namespace byteback
