#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <functional>
#include <memory>

namespace byteback {

class VirtualRaid;
class EwfReader;
class ByteSource;

struct DriveInfo {
    int index;
    std::string model;
    std::string serial;
    uint64_t sizeBytes;
    uint32_t sectorSize;
    std::string type; // "HDD", "SSD", "USB", "Unknown"
};

struct ReadResult {
    bool success = false;
    uint64_t bytesRead = 0;
    std::string error;
    bool paddedZeros = false; // short/failed read was zero-padded
};

// A2: state of the memory-volume backend. Clones SHARE one instance, so the
// image buffer is copied once and a fault injected via the original's
// setMemoryFaultRange hook is visible to every clone (the buffer truly is the
// same bytes, not a stale copy).
struct MemoryVolumeState {
    std::vector<uint8_t> data;
    uint64_t faultStartSector = 0;
    uint64_t faultSectorCount = 0;
};

class DiskReader {
public:
    DiskReader();
    ~DiskReader();

    DiskReader(const DiskReader&) = delete;
    DiskReader& operator=(const DiskReader&) = delete;

    // Enumerate all physical drives
    std::vector<DriveInfo> enumerateDrives();

    bool openedWithWriteShare() const;

    // Open a drive for reading (returns false on failure)
    bool openDrive(int driveIndex);

    // Open a volume device path (VSS shadow copy, \\.\X:, etc.).
    bool openVolumePath(const std::string& volumePath);

    // Close current drive
    void closeDrive();

    // When set, readSectors/getDiskSize serve the assembled RAID volume instead
    // of the opened physical drive. openDrive clears any prior backend.
    void setRaidBackend(std::shared_ptr<VirtualRaid> raid);
    bool hasRaidBackend() const;

    // Read sectors from current drive
    // offset and size MUST be sector-aligned
    ReadResult readSectors(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer);

    // CA-004: byte-granular read for arbitrary offsets AND sizes (carve
    // headers that start mid-sector, tails that end mid-sector). Backed by an
    // aligned sector read + slice, so XTS decryption and every backend keep
    // working. `buffer` needs exactly sizeBytes of room. Thread-safe: the
    // scratch is call-local.
    ReadResult readBytes(uint64_t byteOffset, uint32_t sizeBytes, uint8_t* buffer) {
        const uint32_t ss = sectorSize_ ? sectorSize_ : 512;
        const uint64_t alignedStart = (byteOffset / ss) * ss;
        const uint32_t skip = static_cast<uint32_t>(byteOffset - alignedStart);
        if (skip == 0 && sizeBytes % ss == 0) return readSectors(byteOffset, sizeBytes, buffer);
        // N1: 64-bit math — 32-bit rounding overflowed for sizes near 4GB.
        const uint64_t aligned64 = ((static_cast<uint64_t>(skip) + sizeBytes + ss - 1) / ss) * ss;
        if (aligned64 > UINT32_MAX) {
            ReadResult res;
            res.error = "readBytes: size too large";
            return res;
        }
        const uint32_t alignedSize = static_cast<uint32_t>(aligned64);
        std::vector<uint8_t> scratch(alignedSize);
        ReadResult res = readSectors(alignedStart, alignedSize, scratch.data());
        if (res.bytesRead > skip) {
            const uint64_t avail = std::min<uint64_t>(res.bytesRead - skip, sizeBytes);
            std::memcpy(buffer, scratch.data() + skip, static_cast<size_t>(avail));
            res.bytesRead = avail;
        } else if (res.success || res.bytesRead > 0) {
            // W1: the aligned read succeeded partially but ended before the
            // requested offset — the caller's buffer must NOT keep stale bytes
            // from a previous read (recovery would write them as clean data).
            res.success = false;
            res.bytesRead = 0;
            if (res.error.empty()) res.error = "read ended before requested offset";
        }
        return res;
    }

    // Get disk geometry of opened drive
    uint64_t getDiskSize() const;
    uint32_t getSectorSize() const;

    // CA-007: read-failure telemetry. Every failed/short read records the
    // affected sectors so the UI's bad-sector map reflects reality instead
    // of a permanently empty list.
    uint64_t getBadSectorReads() const;
    std::vector<uint64_t> getBadSectors() const;
    int getDriveIndex() const;

    bool isOpen() const;

    // In-memory volume for unit tests and pre-loaded disk images (.dd).
    void attachMemoryVolume(std::vector<uint8_t> image, uint32_t sectorSize = 512);
    void detachMemoryVolume();
    bool hasMemoryVolume() const;

    // A2: an independent reader over the SAME source, or nullptr when the
    // backend cannot be cloned (RAID member set, HTTP raw image). Supported
    // sources: physical drive (re-opened by index), volume path (re-opened),
    // raw file (re-opened), EWF image (re-parsed), memory volume (shares the
    // buffer via shared_ptr — no copy). Each clone owns its own handle and
    // locks, so concurrent reads on clones never serialize on the source.
    std::unique_ptr<DiskReader> clone() const {
        auto c = std::make_unique<DiskReader>();
        // Snapshot the source spec under our lock, then act on the clone —
        // copyXtsFvekFrom re-locks ioMutex_, so it must run outside it.
        int driveIndex = -1;
        std::string volumePath, rawPath, ewfPath;
        std::shared_ptr<MemoryVolumeState> mem;
        uint32_t sectorSize = 512;
        bool memoryMode = false;
        bool unclonable = false;
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            if (raidBackend_ || rawBackendIsHttp_) unclonable = true;
            driveIndex = currentDriveIndex_;
            volumePath = volumePath_;
            rawPath = rawFilePath_;
            ewfPath = ewfPath_;
            mem = memoryVolume_;
            memoryMode = memoryMode_ && static_cast<bool>(memoryVolume_);
            sectorSize = sectorSize_ ? sectorSize_ : 512;
        }
        if (unclonable) return nullptr;

        std::string err;
        if (memoryMode) {
            // Same class: share the volume state directly (buffer NOT copied).
            c->closeDriveUnlocked();
            c->memoryVolume_ = std::move(mem);
            c->memoryMode_ = true;
            c->sectorSize_ = sectorSize;
            c->diskSize_ = c->memoryVolume_->data.size();
        } else if (driveIndex >= 0) {
            if (!c->openDrive(driveIndex)) return nullptr;
        } else if (!volumePath.empty()) {
            if (!c->openVolumePath(volumePath)) return nullptr;
        } else if (!rawPath.empty()) {
            if (!c->attachRawFile(rawPath, &err)) return nullptr;
        } else if (!ewfPath.empty()) {
            if (!c->attachEwfImage(ewfPath, &err)) return nullptr;
        } else {
            return nullptr;
        }
        c->copyXtsFvekFrom(*this);
        return c;
    }

    // A2: parallel-carve policy. True only for backends that tolerate several
    // concurrent readers with real random access: memory volumes, local raw
    // files, local EWF images (each clone gets an independent handle; EWF
    // re-inflates per instance). Physical drives and volume devices stay
    // SEQUENTIAL by policy — they are seek-bound, so overlapping workers
    // thrash the head and lose to one well-ordered stream. RAIDs serialize on
    // their member readers, HTTP sources would multiply range traffic; both
    // are rejected here (and clone() itself returns nullptr for them).
    bool supportsParallelScan() const {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (memoryMode_) return true;
        if (rawBackend_ && !rawBackendIsHttp_) return true;
        if (ewfBackend_ && !ewfPathIsHttp_) return true;
        return false;
    }

    // Test hook (memory backend only): make every read touching the given
    // sector range fail outright, simulating a mid-image bad-sector region.
    // Pass count=0 to clear.
    void setMemoryFaultRange(uint64_t startSector, uint64_t sectorCount);

    // EWF (.E01) or raw image over http(s) Range (ponytail: multi-segment EWF local only).
    bool attachEwfImage(const std::string& pathOrUrl, std::string* errOut = nullptr);
    bool attachRawFile(const std::string& path, std::string* errOut = nullptr);
    bool attachHttpRawImage(const std::string& url, std::string* errOut = nullptr);
    void detachImageBackend();
    bool hasImageBackend() const;

    // B2: source identity for imaging-resume sidecars. Empty for backends
    // without a path (drives are identified by index+size, memory volumes by
    // size alone — see DiskImager's sourceKey composition).
    std::string imageSourcePath() const {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (!rawFilePath_.empty()) return rawFilePath_;
        if (!ewfPath_.empty()) return ewfPath_;
        return {};
    }

    // AES-XTS FVEK: 32 bytes (AES-128-XTS) or 64 bytes (AES-256-XTS).
    // Decrypts each sector after read. Not a password cracker — caller supplies FVEK.
    bool setXtsFvek(const uint8_t* key, size_t keyBytes);
    bool setXtsFvek128(const uint8_t* key32, size_t n);
    void clearXtsFvek();
    bool hasXtsFvek() const;
    size_t xtsFvekBytes() const;
    void copyXtsFvekFrom(const DiskReader& src);

private:
    void noteBadRead(uint64_t startSectorBytes, uint32_t sizeBytes);
    void closeDriveUnlocked();

    mutable std::mutex ioMutex_;
    mutable std::mutex badSectorMutex_;
    uint64_t badSectorReads_ = 0;
    std::vector<uint64_t> badSectorList_; // capped; representative samples

    void* handle_; // HANDLE on Windows
    uint64_t diskSize_;
    uint32_t sectorSize_;
    int currentDriveIndex_ = -1;
    bool shareWrite_ = false;
    std::shared_ptr<VirtualRaid> raidBackend_;
    // A2: the source spec each backend was opened from — clone() re-opens it.
    std::string volumePath_;
    std::string rawFilePath_;
    std::string ewfPath_;
    bool ewfPathIsHttp_ = false;
    std::shared_ptr<MemoryVolumeState> memoryVolume_; // shared with clones (A2)
    bool memoryMode_ = false;
    std::unique_ptr<EwfReader> ewfBackend_;
    std::unique_ptr<ByteSource> rawBackend_;
    bool rawBackendIsHttp_ = false;
    uint8_t xtsKey_[64]{};
    uint8_t xtsKeyLen_ = 0; // 0=off, 32=AES-128-XTS, 64=AES-256-XTS

    void maybeDecryptXts(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer);
};

} // namespace byteback
