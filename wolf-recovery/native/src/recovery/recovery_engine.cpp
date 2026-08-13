#include "wolf_recovery.h"
#include "wolf_memory.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

// MD5 — minimal implementation for forensic hash verification
// ponytail: using a simple MD5 here; upgrade to OpenSSL for production
namespace {
    struct MD5Context {
        uint32_t state[4];
        uint64_t count;
        uint8_t buffer[64];
    };

    static const uint32_t MD5_S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    static const uint32_t MD5_K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };

    inline uint32_t leftRotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

    void md5Transform(uint32_t state[4], const uint8_t block[64]) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) | ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);

        for (uint32_t i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7*i) % 16; }
            uint32_t temp = d; d = c; c = b;
            b = b + leftRotate(a + f + MD5_K[i] + M[g], MD5_S[i]);
            a = temp;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    }

    void md5Init(MD5Context& ctx) {
        ctx.count = 0;
        ctx.state[0] = 0x67452301; ctx.state[1] = 0xefcdab89;
        ctx.state[2] = 0x98badcfe; ctx.state[3] = 0x10325476;
    }

    void md5Update(MD5Context& ctx, const uint8_t* data, size_t len) {
        size_t index = (size_t)(ctx.count % 64);
        ctx.count += len;
        size_t i = 0;
        if (index) {
            size_t part = 64 - index;
            if (len >= part) {
                memcpy(ctx.buffer + index, data, part);
                md5Transform(ctx.state, ctx.buffer);
                i = part;
            } else {
                memcpy(ctx.buffer + index, data, len);
                return;
            }
        }
        for (; i + 64 <= len; i += 64)
            md5Transform(ctx.state, data + i);
        if (i < len)
            memcpy(ctx.buffer, data + i, len - i);
    }

    std::string md5Final(MD5Context& ctx) {
        uint8_t padding[64] = {0x80};
        size_t index = (size_t)(ctx.count % 64);
        size_t padLen = (index < 56) ? (56 - index) : (120 - index);
        uint64_t bits = ctx.count * 8;
        md5Update(ctx, padding, padLen);
        uint8_t bitsLE[8];
        for (int i = 0; i < 8; i++) bitsLE[i] = (uint8_t)(bits >> (i * 8));
        md5Update(ctx, bitsLE, 8);

        char hex[33];
        for (int i = 0; i < 4; i++) {
            uint32_t s = ctx.state[i];
            snprintf(hex + i*8, 9, "%02x%02x%02x%02x", s&0xFF, (s>>8)&0xFF, (s>>16)&0xFF, (s>>24)&0xFF);
        }
        return std::string(hex, 32);
    }
}

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

    MD5Context md5ctx;
    md5Init(md5ctx);

    // Walk through each data run and read the clusters
    for (const auto& run : record.runs) {
        if (isRunning && !(*isRunning)) break;
        if (bytesWritten >= totalBytes) break;

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
                md5Update(md5ctx, reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
                bytesWritten += zeroSize;
                runBytesRead += toRead;
                continue;
            }

            // Don't write beyond the actual file size
            uint32_t writeSize = (uint32_t)std::min((uint64_t)res.bytesRead, totalBytes - bytesWritten);
            outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
            md5Update(md5ctx, poolBuf->data(), writeSize);
            
            bytesWritten += writeSize;
            runBytesRead += res.bytesRead;

            if (onProgress) onProgress(bytesWritten, totalBytes);
        }
    }

    outFile.close();
    result.success = true;
    result.bytesRecovered = bytesWritten;
    result.md5Hash = md5Final(md5ctx);
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

    MD5Context md5ctx;
    md5Init(md5ctx);

    while (bytesWritten < totalBytes) {
        if (isRunning && !(*isRunning)) break;

        uint32_t toRead = (uint32_t)std::min((uint64_t)readChunk, totalBytes - bytesWritten);
        toRead = ((toRead + sectorSize - 1) / sectorSize) * sectorSize;

        auto res = reader.readSectors(startOffset + bytesWritten, toRead, poolBuf->data());
        
        if (!res.success || res.bytesRead == 0) {
            uint32_t zeroSize = std::min(toRead, (uint32_t)(totalBytes - bytesWritten));
            std::vector<char> zeros(zeroSize, 0);
            outFile.write(zeros.data(), zeroSize);
            md5Update(md5ctx, reinterpret_cast<const uint8_t*>(zeros.data()), zeroSize);
            bytesWritten += zeroSize;
            continue;
        }

        uint32_t writeSize = (uint32_t)std::min((uint64_t)res.bytesRead, totalBytes - bytesWritten);
        outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
        md5Update(md5ctx, poolBuf->data(), writeSize);

        bytesWritten += writeSize;
        if (onProgress) onProgress(bytesWritten, totalBytes);
    }

    outFile.close();
    result.success = true;
    result.bytesRecovered = bytesWritten;
    result.md5Hash = md5Final(md5ctx);
    return result;
}

} // namespace wolf
