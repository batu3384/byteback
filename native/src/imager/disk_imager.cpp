#include "byteback_imager.h"
#include "byteback_memory.h"
#include "crypto/byteback_md5.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace byteback {

namespace {

// ---------------------------------------------------------------------------
// B2: RAW imaging resume. Data goes to `<dest>.part` with a sidecar
// `<dest>.part.json` recording the resume point. Cancel/abort KEEPS both (the
// sidecar is the resume point); success renames .part over the destination
// and removes the sidecar.
//
// EWF stays abort-only by design: appending mid-stream would invalidate the
// chunk tables/digest layout that follows the resume point (see
// EwfWriter::abort()). A cancelled EWF acquisition keeps its readable but
// unverified partial image; a restart starts from zero.
// ---------------------------------------------------------------------------

constexpr char kSidecarFormat[] = "raw";
constexpr uint32_t kMd5StateVersion = 1;

std::string b64Encode(const uint8_t* data, size_t len) {
    static const char* TBL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(TBL[(v >> 18) & 63]);
        out.push_back(TBL[(v >> 12) & 63]);
        out.push_back(TBL[(v >> 6) & 63]);
        out.push_back(TBL[v & 63]);
    }
    if (i + 1 == len) {
        const uint32_t v = data[i] << 16;
        out.push_back(TBL[(v >> 18) & 63]);
        out.push_back(TBL[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        const uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(TBL[(v >> 18) & 63]);
        out.push_back(TBL[(v >> 12) & 63]);
        out.push_back(TBL[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

bool b64Decode(const std::string& in, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (in.size() % 4 != 0) return false;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// Fieldwise Md5State serialization (92 bytes, little-endian): state[4], count,
// buffer[64], bufLen. The version lives in the sidecar JSON, not the blob.
void serializeMd5State(const crypto::Md5State& s, uint8_t out[16 + 8 + 64 + 4]) {
    size_t o = 0;
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 4; ++b) out[o++] = static_cast<uint8_t>(s.state[i] >> (8 * b));
    }
    for (int b = 0; b < 8; ++b) out[o++] = static_cast<uint8_t>(s.count >> (8 * b));
    std::memcpy(out + o, s.buffer, 64);
    o += 64;
    for (int b = 0; b < 4; ++b) out[o++] = static_cast<uint8_t>(s.bufLen >> (8 * b));
}

bool deserializeMd5State(const uint8_t* in, size_t len, crypto::Md5State& s) {
    if (len != 16 + 8 + 64 + 4) return false;
    size_t o = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b) v |= static_cast<uint32_t>(in[o++]) << (8 * b);
        s.state[i] = v;
    }
    uint64_t count = 0;
    for (int b = 0; b < 8; ++b) count |= static_cast<uint64_t>(in[o++]) << (8 * b);
    s.count = count;
    std::memcpy(s.buffer, in + o, 64);
    o += 64;
    uint32_t bufLen = 0;
    for (int b = 0; b < 4; ++b) bufLen |= static_cast<uint32_t>(in[o++]) << (8 * b);
    s.bufLen = bufLen;
    return true;
}

// Minimal flat-JSON field extraction for the sidecar we wrote ourselves.
bool jsonFindString(const std::string& json, const char* key, std::string& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find('"', p + needle.size());
    if (p == std::string::npos) return false;
    const size_t start = ++p;
    while (p < json.size() && json[p] != '"') ++p;
    if (p >= json.size()) return false;
    out = json.substr(start, p - start);
    return true;
}

bool jsonFindU64(const std::string& json, const char* key, uint64_t& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find_first_of("0123456789", p + needle.size());
    if (p == std::string::npos) return false;
    out = std::strtoull(json.c_str() + p, nullptr, 10);
    return true;
}

// D1: merge overlapping/adjacent failed-sector runs for the error section.
std::vector<std::pair<uint64_t, uint64_t>> mergeSectorRuns(std::vector<std::pair<uint64_t, uint64_t>> runs) {
    std::sort(runs.begin(), runs.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto& r : runs) {
        if (r.second == 0) continue;
        if (!merged.empty() && r.first <= merged.back().first + merged.back().second) {
            uint64_t end = merged.back().first + merged.back().second;
            end = std::max<uint64_t>(end, r.first + r.second);
            merged.back().second = end - merged.back().first;
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

} // namespace

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

namespace {
// B2: identity of the imaging source — a resume may only continue from a
// sidecar whose sourceKey and size match. Drives use index+size, memory
// volumes size, file backends path+size.
std::string imagingSourceKey(const DiskReader& reader, uint64_t sizeBytes) {
    const int idx = reader.getDriveIndex();
    if (idx >= 0) return "drive:" + std::to_string(idx) + ":" + std::to_string(sizeBytes);
    if (reader.hasMemoryVolume()) return "memory:" + std::to_string(sizeBytes);
    const std::string path = reader.imageSourcePath();
    if (!path.empty()) return "image:" + path + ":" + std::to_string(sizeBytes);
    return "src:" + std::to_string(sizeBytes);
}

bool writeResumeSidecar(const std::string& sidePath, const std::string& sourceKey,
                        uint64_t totalBytes, uint64_t doneBytes, uint32_t sectorSize,
                        const crypto::Md5& md5) {
    crypto::Md5State st;
    if (!md5.saveState(st)) return false;
    uint8_t blob[92];
    serializeMd5State(st, blob);
    std::ofstream f(sidePath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!f.is_open()) return false;
    f << "{\n"
      << "  \"format\": \"" << kSidecarFormat << "\",\n"
      << "  \"sourceKey\": \"" << sourceKey << "\",\n"
      << "  \"totalBytes\": " << totalBytes << ",\n"
      << "  \"doneBytes\": " << doneBytes << ",\n"
      << "  \"sectorSize\": " << sectorSize << ",\n"
      << "  \"md5StateVersion\": " << kMd5StateVersion << ",\n"
      << "  \"md5StateBase64\": \"" << b64Encode(blob, sizeof(blob)) << "\",\n"
      << "  \"appVersion\": \"byteback-native-1\"\n"
      << "}\n";
    return f.good();
}
} // namespace

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

    uint32_t chunkSectors = (16 * 1024 * 1024) / sectorSize;
    if (chunkSectorsOverride_ != 0) chunkSectors = chunkSectorsOverride_;
    if (chunkSectors == 0) chunkSectors = 1;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);

    const std::string partPath = destPath + ".part";
    const std::string sidePath = destPath + ".part.json";

    std::ofstream rawOut;
    std::unique_ptr<EwfWriter> ewf;
    crypto::Md5 rawMd5;
    std::vector<std::pair<uint64_t, uint64_t>> failedSectorRuns; // D1

    // B2: resume state (RAW only).
    uint64_t startSector = 0;
    bool resumed = false;

    if (format == ImageFormat::Ewf) {
        ewf = std::make_unique<EwfWriter>();
        if (ewfOpts.examiner.empty()) ewfOpts.examiner = "Byteback";
        if (!ewf->open(destPath, totalSectors, sectorSize, ewfOpts)) {
            fail();
            return;
        }
    } else {
        // Look for a resumable .part: the sidecar must match the source, the
        // geometry and the total size, and the .part must actually hold at
        // least doneBytes. Anything else means a FRESH restart that deletes
        // the stale artifacts first (logged; raw imaging has no result object
        // beyond lastImageMd5/progress, so the note goes to the log).
        bool sidecarPresent = false;
        {
            std::ifstream side(sidePath, std::ios::binary);
            sidecarPresent = side.is_open();
        }
        uint64_t resumeBytes = 0;
        std::string resumeMd5B64;
        if (sidecarPresent) {
            std::ifstream side(sidePath, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(side)),
                             std::istreambuf_iterator<char>());
            side.close();
            std::string fmt, srcKey, md5B64;
            uint64_t totalBytes = 0, doneBytes = 0, sectorSizeSide = 0, md5Version = 0;
            if (jsonFindString(json, "format", fmt) &&
                jsonFindString(json, "sourceKey", srcKey) &&
                jsonFindString(json, "md5StateBase64", md5B64) &&
                jsonFindU64(json, "totalBytes", totalBytes) &&
                jsonFindU64(json, "doneBytes", doneBytes) &&
                jsonFindU64(json, "sectorSize", sectorSizeSide) &&
                jsonFindU64(json, "md5StateVersion", md5Version) &&
                fmt == kSidecarFormat && md5Version == kMd5StateVersion &&
                totalBytes == totalSectors * sectorSize &&
                sectorSizeSide == sectorSize &&
                srcKey == imagingSourceKey(reader, totalSectors * sectorSize) &&
                doneBytes <= totalBytes &&
                doneBytes % sectorSize == 0) {
                resumeBytes = doneBytes;
                resumeMd5B64 = md5B64;
            }
        }

        if (resumeBytes > 0) {
            std::error_code ec;
            const bool partOk = std::filesystem::exists(partPath, ec) && !ec &&
                                std::filesystem::file_size(partPath, ec) >= resumeBytes && !ec;
            if (partOk) {
                ec.clear();
                std::filesystem::resize_file(partPath, resumeBytes, ec);
                if (!ec) {
                    crypto::Md5State st;
                    std::vector<uint8_t> blob;
                    if (b64Decode(resumeMd5B64, blob) &&
                        deserializeMd5State(blob.data(), blob.size(), st) &&
                        rawMd5.loadState(st)) {
                        rawOut.open(partPath, std::ios::binary | std::ios::out | std::ios::app);
                        if (rawOut.is_open()) {
                            startSector = resumeBytes / sectorSize;
                            resumed = true;
                        }
                    }
                }
            }
            if (!resumed) {
                // Sidecar said resume but the artifacts disagree — fresh.
                std::cerr << "[byteback] imaging resume unusable; restarting "
                          << partPath << " from zero" << std::endl;
            }
        }
        if (!resumed) {
            std::error_code ec;
            std::filesystem::remove(partPath, ec);
            std::filesystem::remove(sidePath, ec);
            rawOut.open(partPath, std::ios::binary | std::ios::out | std::ios::trunc);
        }
        if (!rawOut.is_open()) {
            fail();
            return;
        }
    }

    uint64_t sector = startSector;
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
                failedSectorRuns.push_back({sub.sector, sub.sectors}); // D1
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
                failedSectorRuns.push_back({sub.sector + res.bytesRead / sectorSize,
                                            missing / sectorSize}); // D1 (approx tail)
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
        if (!ewf && rawOut.is_open()) {
            rawOut.close();
            // Keep the resume point current even after a write error.
            writeResumeSidecar(sidePath, imagingSourceKey(reader, totalSectors * sectorSize),
                               totalSectors * sectorSize, sector * sectorSize, sectorSize, rawMd5);
        }
        fail();
        return;
    }

    // requestStop mid-image: sector never reached totalSectors. Close the
    // output honestly — a partial EWF keeps its tables (still readable) but
    // gets NO digest/done section and NO MD5, so a truncated acquisition can
    // never present itself as a verified image. RAW keeps .part + sidecar as
    // the exact resume point.
    const bool stoppedEarly = sector < totalSectors;

    if (ewf) {
        if (stoppedEarly) {
            if (!ewf->abort()) {
                fail();
                return;
            }
        } else {
            ewf->setAcquiryErrors(mergeSectorRuns(std::move(failedSectorRuns)));
            if (!ewf->finish()) {
                fail();
                return;
            }
        }
        if (!stoppedEarly) lastImageMd5_ = ewf->md5Hex();
    } else {
        rawOut.close();
        if (stoppedEarly) {
            writeResumeSidecar(sidePath, imagingSourceKey(reader, totalSectors * sectorSize),
                               totalSectors * sectorSize, sector * sectorSize, sectorSize, rawMd5);
        } else {
            // Success: promote .part over the destination (removing any old
            // destination first — same overwrite policy as before) and clear
            // the resume bookkeeping.
            std::error_code ec;
            std::filesystem::remove(destPath, ec);
            ec.clear();
            std::filesystem::rename(partPath, destPath, ec);
            if (ec) {
                fail();
                return;
            }
            ec.clear();
            std::filesystem::remove(sidePath, ec);
            lastImageMd5_ = rawMd5.finalHex();
        }
    }

    // Completion tick (current == total) is the caller's end-of-job signal
    // (the NAPI bridge releases its ThreadSafeFunction on it), so it is sent
    // even for a cancelled run — the empty lastImageMd5() marks it unverified.
    if (onProgress) onProgress(totalSectors, totalSectors);
    isRunning_ = false;
}

} // namespace byteback
