#include "wolf_recovery.h"
#include "wolf_memory.h"
#include "crypto/wolf_md5.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace wolf {

RecoveryEngine::RecoveryEngine() {}
RecoveryEngine::~RecoveryEngine() {}

RecoveryResult RecoveryEngine::recoverFile(DiskReader& reader, const FileRecord& record,
                                            const std::string& destDir, ProgressCallback onProgress,
                                            std::atomic<bool>* isRunning) {
    RecoveryResult result;
    result.success = false;
    result.bytesRecovered = 0;

    if (!reader.isOpen()) {
        result.error = "Disk is not open";
        return result;
    }

    if (record.runs.empty()) {
        // No data runs — fall back to carved file recovery (contiguous sectors)
        return recoverCarvedFile(reader, record, destDir, onProgress, isRunning);
    }

    // Create destination directory if needed
    std::filesystem::create_directories(destDir);

    std::string destPath = destDir + "/" + record.name;
    result.destPath = destPath;

    std::ofstream outFile(destPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + destPath;
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
                // Bad sector — write zeros to maintain offset integrity
                uint32_t zeroSize = std::min(toRead, (uint32_t)(totalBytes - bytesWritten));
                std::vector<char> zeros(zeroSize, 0);
                outFile.write(zeros.data(), zeroSize);
                md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
                bytesWritten += zeroSize;
                runBytesRead += toRead;
                continue;
            }

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
    result.success = true;
    result.bytesRecovered = bytesWritten;
    result.md5Hash = md5ctx.finalHex();
    return result;
}

RecoveryResult RecoveryEngine::recoverCarvedFile(DiskReader& reader, const FileRecord& record,
                                                  const std::string& destDir, ProgressCallback onProgress,
                                                  std::atomic<bool>* isRunning) {
    RecoveryResult result;
    result.success = false;
    result.bytesRecovered = 0;

    if (!reader.isOpen()) {
        result.error = "Disk is not open";
        return result;
    }

    std::filesystem::create_directories(destDir);

    std::string destPath = destDir + "/" + record.name;
    result.destPath = destPath;

    std::ofstream outFile(destPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + destPath;
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
            uint32_t zeroSize = std::min(toRead, (uint32_t)(totalBytes - bytesWritten));
            std::vector<char> zeros(zeroSize, 0);
            outFile.write(zeros.data(), zeroSize);
            md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
            bytesWritten += zeroSize;
            continue;
        }

        uint32_t writeSize = (uint32_t)std::min((uint64_t)res.bytesRead, totalBytes - bytesWritten);
        outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
        md5ctx.update(poolBuf->data(), writeSize);

        bytesWritten += writeSize;
        if (onProgress) onProgress(bytesWritten, totalBytes);
    }

    outFile.close();
    result.success = true;
    result.bytesRecovered = bytesWritten;
    result.md5Hash = md5ctx.finalHex();
    return result;
}

} // namespace wolf
