#include "byteback_imager.h"
#include "byteback_memory.h"
#include "crypto/byteback_md5.h"
#include "fs/virtual_raid.h"
#include "io/volume_mapper_win.h"
#include "recovery/path_util.h"
#include <algorithm>
#include <cerrno>
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
    // A sign between the ':' and the digits means this is not the plain
    // unsigned count we write ("doneBytes": -512 must not parse as 512).
    size_t s = p;
    while (s > 0 && (json[s - 1] == ' ' || json[s - 1] == '\t')) --s;
    if (s > 0 && (json[s - 1] == '-' || json[s - 1] == '+')) return false;
    errno = 0;
    out = std::strtoull(json.c_str() + p, nullptr, 10);
    return errno != ERANGE; // overflowed garbage is not a resume point
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
                              ImageFormat format, const EwfOptions& ewfOpts, const std::string& volumePath) {
    stopImaging();
    isRunning_ = true;
    lastImageMd5_.clear();
    imagingThread_ = std::thread(&DiskImager::imagingWorker, this, driveIndex, destPath, onProgress, format, ewfOpts,
                                 volumePath);
}

void DiskImager::startImagingFromReader(DiskReader& reader, const std::string& destPath,
                                        ProgressCallback onProgress, ImageFormat format,
                                        const EwfOptions& ewfOpts) {
    stopImaging();
    isRunning_ = true;
    lastImageMd5_.clear();
    // Lifetime contract: `reader` is captured by reference and MUST outlive
    // the imaging thread (until stopImaging() joins it or the run completes).
    // Production uses this for assembled RAID (ImagerContext::raidReader);
    // PhysicalDrive/volume still go through imagingWorker.
    imagingThread_ = std::thread([this, &reader, destPath, onProgress, format, ewfOpts]() {
        imagingRun(reader, destPath, onProgress, format, ewfOpts);
    });
}

void DiskImager::requestStop() {
    isRunning_ = false;
}

void DiskImager::stopImaging() {
    requestStop();
    if (imagingThread_.get_id() == std::this_thread::get_id()) {
        // Self-stop from a progress callback: joining here would deadlock, so
        // the thread is parked joinable on the member and finishes the current
        // chunk on its own. A later foreign stopImaging()/startImaging*/
        // destructor joins it first, so a restarted acquisition can never
        // overlap the cancelled one on the same destination. Caveat: the
        // DiskImager and any captured reader must still outlive that run — do
        // not destroy the imager from inside its own callback.
        if (!imagingThread_.joinable()) return;
        if (parkedThread_.joinable()) parkedThread_.detach(); // unreachable in practice; never abandon a thread object
        parkedThread_ = std::move(imagingThread_);
        return;
    }
    if (imagingThread_.joinable()) imagingThread_.join();
    if (parkedThread_.joinable()) parkedThread_.join();
}

void DiskImager::imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress,
                             ImageFormat format, EwfOptions ewfOpts, std::string volumePath) {
    DiskReader reader;
    bool opened = false;
    if (isWin32VolumeDevicePath(volumePath)) {
        opened = reader.openVolumePath(volumePath);
    } else if (!volumePath.empty()) {
        opened = reader.attachEvidenceImage(volumePath);
    } else if (driveIndex >= 0) {
        opened = reader.openDrive(driveIndex);
    }
    if (!opened) {
        if (onProgress) onProgress(0, 0);
        isRunning_ = false;
        return;
    }
    imagingRun(reader, destPath, onProgress, format, ewfOpts);
}

namespace {

void appendHex(std::string& out, const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out += kHex[p[i] >> 4];
        out += kHex[p[i] & 0xf];
    }
}

// Three 16-byte windows (head / mid / tail). Prefix-verify only sees the first
// 1 MiB of a .part; two same-size volumes that share that window used to resume
// into a hybrid image. ponytail: windows that match all three still collide.
std::string sourceContentFingerprint(DiskReader& reader, uint64_t sizeBytes) {
    auto hexAt = [&](uint64_t off) {
        uint8_t buf[16] = {};
        if (sizeBytes >= 16 && off + 16 <= sizeBytes) {
            (void)reader.readBytes(off, 16, buf);
        }
        std::string h;
        appendHex(h, buf, 16);
        return h;
    };
    std::string k = ":fp=";
    k += hexAt(0);
    if (sizeBytes >= 48) {
        k += '.';
        k += hexAt(sizeBytes / 2);
        k += '.';
        k += hexAt(sizeBytes - 16);
    }
    return k;
}

// B2: identity of the imaging source — a resume may only continue from a
// sidecar whose sourceKey matches. RAID uses VirtualRaid::resumeKey();
// drive/memory add content fingerprints so same-size sources do not collide.
std::string imagingSourceKey(DiskReader& reader, uint64_t sizeBytes) {
    if (auto raid = reader.raidBackend()) return raid->resumeKey();
    const int idx = reader.getDriveIndex();
    if (idx >= 0) {
        return "drive:" + std::to_string(idx) + ":" + std::to_string(sizeBytes)
            + sourceContentFingerprint(reader, sizeBytes);
    }
    if (reader.hasMemoryVolume()) {
        return "memory:" + std::to_string(sizeBytes)
            + sourceContentFingerprint(reader, sizeBytes);
    }
    const std::string path = reader.imageSourcePath();
    if (!path.empty()) {
        const char* kind = isWin32VolumeDevicePath(path) ? "volume:" : "image:";
        return std::string(kind) + path + ":" + std::to_string(sizeBytes)
            + sourceContentFingerprint(reader, sizeBytes);
    }
    return "src:" + std::to_string(sizeBytes)
        + sourceContentFingerprint(reader, sizeBytes);
}

bool writeResumeSidecar(const std::string& sidePath, const std::string& sourceKey,
                        uint64_t totalBytes, uint64_t doneBytes, uint32_t sectorSize,
                        const crypto::Md5& md5) {
    crypto::Md5State st;
    if (!md5.saveState(st)) return false;
    uint8_t blob[92];
    serializeMd5State(st, blob);
    std::ofstream f(utf8Path(sidePath), std::ios::binary | std::ios::out | std::ios::trunc);
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

// B2 forensic rule: a resumed image is byte-identical to a one-shot run, or
// the resume must clearly fail. Two gates decide that, and BOTH must pass
// before the .part is trusted:
//
//   1) Prefix integrity — stream the .part's [0, resumeBytes) through a fresh
//      MD5 and require the resulting midstream state to equal the sidecar's
//      serialized state. A corrupt, truncated, tampered or fabricated pair
//      (sidecar/.part disagree in any byte) fails here instead of producing a
//      silently wrong digest over wrong bytes. The reseeded context is also
//      the one the run continues with, so `loadState` never has to trust the
//      sidecar blob on its own.
//
//   2) Source identity — the sidecar's sourceKey includes geometry plus a
//      three-window content fingerprint (head/mid/tail). Prefix-verify still
//      samples the first 1 MiB of the .part so a swapped source that collides
//      on the fingerprint still fails here when the window differs.
//
// Ceiling: sources matching all three 16-byte fingerprint windows AND the
// first kResumeSampleWindow of a .part stay confusable — full certainty would
// require re-reading the entire prefix, which is the work resume exists to avoid.
constexpr uint64_t kResumeSampleWindow = 1024ull * 1024ull;

bool verifyResumePrefix(const std::string& partPath, uint64_t resumeBytes,
                        DiskReader& reader, crypto::Md5& reseeded, std::string& why) {
    reseeded = crypto::Md5();
    std::ifstream f(utf8Path(partPath), std::ios::binary);
    if (!f.is_open()) {
        why = "part reopen failed";
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(resumeBytes, 1024ull * 1024ull)));
    std::vector<uint8_t> head; // sample window as it exists in the .part
    uint64_t off = 0;
    while (off < resumeBytes) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), resumeBytes - off));
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(f.gcount()) != want) {
            why = "part shortened under resume";
            return false;
        }
        reseeded.update(buf.data(), want);
        if (head.empty()) {
            const size_t win = static_cast<size_t>(std::min<uint64_t>(resumeBytes, kResumeSampleWindow));
            head.assign(buf.data(), buf.data() + win);
        }
        off += want;
    }

    const uint64_t win = std::min<uint64_t>(resumeBytes, kResumeSampleWindow);
    std::vector<uint8_t> src(static_cast<size_t>(win), 0);
    const auto res = reader.readSectors(0, static_cast<uint32_t>(win), src.data());
    if (res.bytesRead < win) {
        // Keep the zero-filled remainder: deterministic for this source and
        // consistent with how the failed read was imaged in the first place.
        why = "source sample read short";
        return false;
    }
    if (std::memcmp(head.data(), src.data(), static_cast<size_t>(win)) != 0) {
        why = "source changed since the cancelled run";
        return false;
    }
    return true;
}

bool statesMatch(const crypto::Md5& live, const crypto::Md5State& sidecarState) {
    crypto::Md5State a;
    if (!live.saveState(a)) return false;
    uint8_t blobA[92], blobB[92];
    serializeMd5State(a, blobA);
    serializeMd5State(sidecarState, blobB);
    return std::memcmp(blobA, blobB, sizeof(blobA)) == 0;
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

    const uint64_t sizeBytes = totalSectors * sectorSize;
    const std::string srcKey = imagingSourceKey(reader, sizeBytes);

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
            std::ifstream side(utf8Path(sidePath), std::ios::binary);
            sidecarPresent = side.is_open();
        }
        uint64_t resumeBytes = 0;
        std::string resumeMd5B64;
        if (sidecarPresent) {
            std::ifstream side(utf8Path(sidePath), std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(side)),
                             std::istreambuf_iterator<char>());
            side.close();
            std::string fmt, sideSrcKey, md5B64;
            uint64_t totalBytes = 0, doneBytes = 0, sectorSizeSide = 0, md5Version = 0;
            if (jsonFindString(json, "format", fmt) &&
                jsonFindString(json, "sourceKey", sideSrcKey) &&
                jsonFindString(json, "md5StateBase64", md5B64) &&
                jsonFindU64(json, "totalBytes", totalBytes) &&
                jsonFindU64(json, "doneBytes", doneBytes) &&
                jsonFindU64(json, "sectorSize", sectorSizeSide) &&
                jsonFindU64(json, "md5StateVersion", md5Version) &&
                fmt == kSidecarFormat && md5Version == kMd5StateVersion &&
                totalBytes == sizeBytes &&
                sectorSizeSide == sectorSize &&
                sideSrcKey == srcKey &&
                doneBytes <= totalBytes &&
                doneBytes % sectorSize == 0) {
                resumeBytes = doneBytes;
                resumeMd5B64 = md5B64;
            }
        }

        if (resumeBytes > 0) {
            std::error_code ec;
            const bool partOk = std::filesystem::exists(utf8Path(partPath), ec) && !ec &&
                                std::filesystem::file_size(utf8Path(partPath), ec) >= resumeBytes && !ec;
            if (partOk) {
                ec.clear();
                std::filesystem::resize_file(utf8Path(partPath), resumeBytes, ec);
                crypto::Md5State st;
                std::vector<uint8_t> blob;
                if (!ec && b64Decode(resumeMd5B64, blob) &&
                    deserializeMd5State(blob.data(), blob.size(), st)) {
                    crypto::Md5 reseeded;
                    std::string why;
                    if (verifyResumePrefix(partPath, resumeBytes, reader, reseeded, why) &&
                        statesMatch(reseeded, st)) {
                        rawOut.open(utf8Path(partPath), std::ios::binary | std::ios::out | std::ios::app);
                        if (rawOut.is_open()) {
                            startSector = resumeBytes / sectorSize;
                            resumed = true;
                            rawMd5 = std::move(reseeded);
                        }
                    } else {
                        std::cerr << "[byteback] imaging resume rejected: "
                                  << (why.empty() ? "prefix does not match sidecar state" : why)
                                  << "; restarting " << partPath << " from zero" << std::endl;
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
            std::filesystem::remove(utf8Path(partPath), ec);
            std::filesystem::remove(utf8Path(sidePath), ec);
            rawOut.open(utf8Path(partPath), std::ios::binary | std::ios::out | std::ios::trunc);
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
            writeResumeSidecar(sidePath, srcKey,
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
            writeResumeSidecar(sidePath, srcKey,
                               totalSectors * sectorSize, sector * sectorSize, sectorSize, rawMd5);
        } else {
            // Success: promote .part over the destination (removing any old
            // destination first — same overwrite policy as before) and clear
            // the resume bookkeeping.
            std::error_code ec;
            std::filesystem::remove(utf8Path(destPath), ec);
            ec.clear();
            std::filesystem::rename(utf8Path(partPath), utf8Path(destPath), ec);
            if (ec) {
                fail();
                return;
            }
            ec.clear();
            std::filesystem::remove(utf8Path(sidePath), ec);
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
