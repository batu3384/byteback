#include "byteback_recovery.h"
#include "byteback_memory.h"
#include "recovery/path_util.h"
#include "recovery/validation.h"
#include "crypto/byteback_md5.h"
#include "fs/ntfs_util.h"
#include "fs/vss_scanner.h"
#include "fs/virtual_raid.h"
#include "scan/discovery_sources.h"
#include "io/volume_mapper_win.h"
#include "io/hex_bind.h"
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

// Bisect a failing read down to single sectors so one bad sector loses a
// sector, not the whole chunk (same policy as imager/carver). Failed leaves
// are zero-filled in place; zeroFilled marks that it happened.
void resilientRead(DiskReader& reader, uint64_t off, uint32_t want, uint8_t* dst,
                   uint32_t sectorSize, bool& zeroFilled) {
    struct Sub { uint64_t off; uint32_t len; };
    std::vector<Sub> stack{{off, want}};
    while (!stack.empty()) {
        const Sub s = stack.back();
        stack.pop_back();
        auto res = reader.readBytes(s.off, s.len, dst + (s.off - off));
        const bool ok = res.success && res.bytesRead >= s.len;
        if (!ok && s.len > sectorSize) {
            uint32_t half = (s.len / 2) / sectorSize * sectorSize;
            if (half == 0) half = sectorSize;
            stack.push_back({s.off + half, s.len - half});
            stack.push_back({s.off, half});
            continue;
        }
        if (res.paddedZeros) zeroFilled = true;
        if (!ok) {
            std::memset(dst + (s.off - off), 0, s.len);
            zeroFilled = true;
        }
    }
}

void finishRecoverWrite(RecoveryResult& result, uint64_t bytes, const std::string& md5,
                        const FileRecord& record, bool cancelled) {
    result.bytesRecovered = bytes;
    result.md5Hash = md5;
    // A cancel (or a run table that ends short) must not masquerade as a
    // successful recovery: the partial file on disk is not the payload the
    // record describes.
    if (cancelled && bytes < record.sizeBytes) {
        result.success = false;
        if (result.error.empty()) result.error = "recovery cancelled; output is incomplete";
        return;
    }
    if (result.zeroFilled) {
        result.success = false;
        if (result.error.empty()) result.error = "short or padded read; output is incomplete";
        return;
    }
    if (bytes > 0 && result.allZeros) {
        // Every byte read back as zero: on TRIMmed SSDs this is the mark of
        // destroyed data, not a successful recovery.
        result.success = false;
        if (result.error.empty())
            result.error = "recovered content is entirely zeros (TRIMmed or overwritten)";
        return;
    }
    result.success = true;
    applyPostRecoveryValidation(result, record);
}

} // namespace

namespace {

// CA-031: record.sizeBytes crosses a trust boundary (hostile/corrupt MFT or
// carve metadata). Clamp the claimed size to what the evidence actually
// supports: the total span of the record's data runs; when no runs exist, a
// hard sanity ceiling. A claimed 2^40 used to reach inflated.resize() and die
// on std::bad_alloc.
// The runs are the evidence: trust their full span (large VHD/PST/video
// files legitimately exceed any fixed ceiling). The ceiling only guards a
// record with NO runs backing its size claim.
constexpr uint64_t kMaxRecoveryClaimBytes = 4ull << 40; // 4 TiB absolute ceiling

uint64_t evidenceClampedSize(const FileRecord& record, uint32_t sectorSize) {
    uint64_t cap = record.runs.empty() ? (16ull << 30) : kMaxRecoveryClaimBytes;
    if (!record.runs.empty()) {
        uint64_t span = 0;
        bool first = true;
        for (const auto& run : record.runs) {
            const uint64_t skip = first ? record.startByteOffset : 0;
            first = false;
            span += dataRunPayloadBytes(run, sectorSize, skip);
        }
        cap = std::min(cap, span);
    }
    return std::min<uint64_t>(record.sizeBytes, cap);
}

} // namespace

// Exported API (byteback_recovery.h) — body shares discovery_sources.h list.
bool isDiscoveryOnlySource(const std::string& source) {
    return isDiscoverySourceName(source);
}

bool bindReaderForRecord(DiskReader& reader, const FileRecord& rec, int driveIndex,
                         std::shared_ptr<VirtualRaid> raid, std::string& err,
                         const std::string& volumePath) {
    err.clear();
    const std::string vss = vssDevicePathFromRecord(rec);
    if (!vss.empty()) {
        if (!reader.openVolumePath(vss)) {
            err = "VSS volume not available";
            return false;
        }
        return true;
    }
    if (driveIndex == -1) {
        if (!raid) {
            err = "RAID array not assembled";
            return false;
        }
        reader.setRaidBackend(raid);
        return true;
    }
    if (!volumePath.empty()) {
        if (isWin32VolumeDevicePath(volumePath)) {
            if (!reader.openVolumePath(volumePath)) {
                err = "volume device not available";
                return false;
            }
            return true;
        }
        if (volumePath.size() >= 4 && volumePath.compare(0, 4, "\\\\.\\") == 0) {
            err = "volume device not available";
            return false;
        }
        if (!reader.attachEvidenceImage(volumePath, &err)) {
            if (err.empty()) err = "image not available";
            return false;
        }
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

    if (isDiscoveryOnlySource(record.source)) {
        result.error = "discovery-only record; no recoverable data";
        return result;
    }

    if (!record.residentData.empty()) {
        std::filesystem::create_directories(utf8Path(destDir));
        if (!applyUniquePath(result, destDir, record.name)) return result;
        std::ofstream outFile(utf8Path(result.destPath), std::ios::binary | std::ios::out | std::ios::trunc);
        if (!outFile.is_open()) {
            result.error = "Could not open destination file: " + result.destPath;
            return result;
        }
        uint64_t n = record.residentData.size();
        if (record.sizeBytes > 0) n = std::min(n, record.sizeBytes);
        if (std::any_of(record.residentData.begin(), record.residentData.end(),
                        [](uint8_t b) { return b != 0; }))
            result.allZeros = false;
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

    if (!reader.isOpen() && !reader.hasRaidBackend()) {
        result.error = "Disk is not open";
        return result;
    }

    if (record.runs.empty()) {
        if (record.source == "carver" || record.source == "carver_bgc" || record.source.empty()) {
            return recoverCarvedFile(reader, record, destDir, onProgress, isRunning);
        }
        result.error = "no data runs; resident content not extracted";
        return result;
    }

    std::filesystem::create_directories(utf8Path(destDir));

    if (!applyUniquePath(result, destDir, record.name)) return result;

    std::ofstream outFile(utf8Path(result.destPath), std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + result.destPath;
        return result;
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t totalBytes = evidenceClampedSize(record, sectorSize);
    uint64_t bytesWritten = 0;

    // Read buffer — 1MB at a time
    const uint32_t readChunk = 1024 * 1024;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(readChunk);

    crypto::Md5 md5ctx;

    // CA-005 fix: LZNT1-compressed $DATA must be inflated before writing.
    // NTFS compresses each compression unit (16 clusters = 64 KiB at 4 KiB
    // clusters) INDEPENDENTLY: a unit is stored as one LZNT1 stream in fewer
    // clusters, written raw when compression does not pay, or left sparse for
    // all-zero data. Concatenating runs and inflating once truncates at the
    // first unit's zero padding — each run is decoded separately here.
    if (record.compressed) {
        const uint64_t cuBytes = 65536; // standard NTFS CU: log 4 × 4 KiB clusters
        uint64_t bytesWritten = 0;
        std::vector<char> zeros(static_cast<size_t>(std::min<uint64_t>(cuBytes, 1u << 20)), 0);
        for (const auto& run : record.runs) {
            if (isRunning && !(*isRunning)) break;
            if (bytesWritten >= totalBytes) break;
            if (run.startSector == UINT64_MAX) {
                uint64_t toZero = std::min<uint64_t>(
                    dataRunPayloadBytes(run, sectorSize, 0), totalBytes - bytesWritten);
                while (toZero > 0) {
                    uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(toZero, zeros.size()));
                    outFile.write(zeros.data(), n);
                    md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()), n);
                    bytesWritten += n;
                    toZero -= n;
                    if (onProgress) onProgress(bytesWritten, totalBytes);
                }
                continue;
            }
            const uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
            if (runBytes < cuBytes) {
                // Allocated part of one compressed unit.
                std::vector<uint8_t> compressed(static_cast<size_t>(runBytes) + sectorSize);
                auto res = reader.readSectors(run.startSector * sectorSize,
                                              static_cast<uint32_t>(runBytes),
                                              compressed.data());
                if (!res.success || res.bytesRead < runBytes || res.paddedZeros) {
                    result.zeroFilled = true;
                    continue;
                }
                const uint64_t wantOut = std::min<uint64_t>(cuBytes, totalBytes - bytesWritten);
                std::vector<uint8_t> inflated(static_cast<size_t>(std::max<uint64_t>(wantOut, 4096)));
                int outLen = ntfs::lznt1Decompress(compressed.data(), res.bytesRead,
                                                   inflated.data(), inflated.size());
                if (outLen < 0) {
                    outFile.close();
                    result.error = "NTFS LZNT1 decompression failed";
                    result.success = false;
                    return result;
                }
                const uint64_t writeLen = std::min<uint64_t>(wantOut, static_cast<uint64_t>(outLen));
                if (writeLen == 0) break;
                if (std::any_of(inflated.begin(), inflated.begin() + writeLen,
                                [](uint8_t b) { return b != 0; }))
                    result.allZeros = false;
                outFile.write(reinterpret_cast<const char*>(inflated.data()),
                              static_cast<std::streamsize>(writeLen));
                md5ctx.update(inflated.data(), static_cast<size_t>(writeLen));
                bytesWritten += writeLen;
                if (onProgress) onProgress(bytesWritten, totalBytes);
            } else {
                // One or more raw (uncompressed) units: copy clusters.
                uint64_t got = 0;
                while (got < runBytes && bytesWritten < totalBytes) {
                    if (isRunning && !(*isRunning)) break;
                    const uint32_t want = static_cast<uint32_t>(
                        std::min<uint64_t>(runBytes - got, readChunk));
                    auto res = reader.readSectors(run.startSector * sectorSize + got, want,
                                                  poolBuf->data());
                    if (!res.success || res.bytesRead == 0) {
                        result.zeroFilled = true;
                        const uint32_t zeroSize = want;
                        outFile.write(zeros.data(), std::min<uint64_t>(zeroSize, zeros.size()));
                        md5ctx.update(reinterpret_cast<const uint8_t*>(zeros.data()),
                                      std::min<uint64_t>(zeroSize, zeros.size()));
                        bytesWritten += zeroSize;
                        got += zeroSize;
                        continue;
                    }
                    if (res.paddedZeros) result.zeroFilled = true;
                    const uint32_t writeSize = static_cast<uint32_t>(
                        std::min<uint64_t>(res.bytesRead, totalBytes - bytesWritten));
                    if (result.allZeros &&
                        std::any_of(poolBuf->data(), poolBuf->data() + writeSize,
                                    [](uint8_t b) { return b != 0; }))
                        result.allZeros = false;
                    outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
                    md5ctx.update(poolBuf->data(), writeSize);
                    bytesWritten += writeSize;
                    got += res.bytesRead;
                    if (onProgress) onProgress(bytesWritten, totalBytes);
                }
            }
        }
        outFile.close();
        finishRecoverWrite(result, bytesWritten, md5ctx.finalHex(), record,
                           isRunning && !(*isRunning));
        return result;
    }

    // Walk through each data run and read the clusters
    bool firstRun = true;
    for (const auto& run : record.runs) {
        if (isRunning && !(*isRunning)) break;
        if (bytesWritten >= totalBytes) break;

        // BGC/carve records: the file truly starts startByteOffset bytes into
        // the first run — skip them or the stitched file that validated at
        // scan time recovers with sector-floor garbage in front.
        uint64_t runOffsetBytes = run.startSector * sectorSize;
        uint64_t skip = 0;
        if (firstRun && record.startByteOffset > 0 && run.startSector != UINT64_MAX) {
            const uint64_t phys = run.sectorCount * static_cast<uint64_t>(sectorSize);
            skip = std::min(record.startByteOffset, phys);
        }
        uint64_t runSizeBytes = dataRunPayloadBytes(run, sectorSize, skip);
        runOffsetBytes += skip;
        firstRun = false;
        uint64_t runBytesRead = 0;

        // Sparse run sentinel (set by the NTFS parser for runs with no
        // physical clusters). NTFS sparse files and compressed-unit gaps read
        // as zeros, so emit zeros for the whole run without touching the disk.
        if (run.startSector == UINT64_MAX) {
            uint64_t runSizeBytes = dataRunPayloadBytes(run, sectorSize, 0);
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

        while (runBytesRead < runSizeBytes && bytesWritten < totalBytes) {
            if (isRunning && !(*isRunning)) break;

            const uint64_t remainRun = runSizeBytes - runBytesRead;
            const uint64_t remainFile = totalBytes - bytesWritten;
            uint32_t toRead = (uint32_t)std::min((uint64_t)readChunk, remainRun);
            // Align to sector
            toRead = ((toRead + sectorSize - 1) / sectorSize) * sectorSize;

            resilientRead(reader, runOffsetBytes + runBytesRead, toRead,
                          poolBuf->data(), sectorSize, result.zeroFilled);

            uint32_t writeSize = (uint32_t)std::min({(uint64_t)toRead, remainFile, remainRun});
            if (result.allZeros &&
                std::any_of(poolBuf->data(), poolBuf->data() + writeSize,
                            [](uint8_t b) { return b != 0; }))
                result.allZeros = false;
            outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
            md5ctx.update(poolBuf->data(), writeSize);
            
            bytesWritten += writeSize;
            runBytesRead += writeSize;

            if (onProgress) onProgress(bytesWritten, totalBytes);
        }
    }

    outFile.close();
    finishRecoverWrite(result, bytesWritten, md5ctx.finalHex(), record,
                       isRunning && !(*isRunning));
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

    std::filesystem::create_directories(utf8Path(destDir));

    if (!applyUniquePath(result, destDir, record.name)) return result;

    std::ofstream outFile(utf8Path(result.destPath), std::ios::binary | std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        result.error = "Could not open destination file: " + result.destPath;
        return result;
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    // CA-005: the record start is byte-exact (startByteOffset inside the
    // start sector). Reading from the sector floor shifted every unaligned
    // carve by up to 511 bytes and truncated its tail.
    uint64_t startOffset = record.startSector * sectorSize + record.startByteOffset;
    // CA-031: carved records carry attacker-controlled sizeBytes too — clamp
    // to what the medium can back (bytes from startOffset to disk end) under
    // the hard sanity ceiling instead of reading a claimed 2^40.
    const uint64_t diskSize = reader.getDiskSize();
    const uint64_t mediumCap =
        diskSize > startOffset ? diskSize - startOffset : 0;
    uint64_t totalBytes = std::min<uint64_t>(record.sizeBytes,
                                             std::min<uint64_t>(kMaxRecoveryClaimBytes, mediumCap));
    uint64_t bytesWritten = 0;

    const uint32_t readChunk = 1024 * 1024;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(readChunk);

    crypto::Md5 md5ctx;

    while (bytesWritten < totalBytes) {
        if (isRunning && !(*isRunning)) break;

        const uint32_t toRead = (uint32_t)std::min((uint64_t)readChunk, totalBytes - bytesWritten);

        // readBytes handles mid-sector alignment internally; bisect failures
        // down to single sectors so one bad sector loses a sector only.
        resilientRead(reader, startOffset + bytesWritten, toRead, poolBuf->data(),
                      sectorSize, result.zeroFilled);

        uint32_t writeSize = (uint32_t)std::min((uint64_t)toRead, totalBytes - bytesWritten);
        if (result.allZeros &&
            std::any_of(poolBuf->data(), poolBuf->data() + writeSize,
                        [](uint8_t b) { return b != 0; }))
            result.allZeros = false;
        outFile.write(reinterpret_cast<const char*>(poolBuf->data()), writeSize);
        md5ctx.update(poolBuf->data(), writeSize);

        bytesWritten += writeSize;
        if (onProgress) onProgress(bytesWritten, totalBytes);
    }

    outFile.close();
    finishRecoverWrite(result, bytesWritten, md5ctx.finalHex(), record,
                       isRunning && !(*isRunning));
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
