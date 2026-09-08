#ifndef _WIN32
// ponytail: minimal POSIX disk reader — memory + file/EWF backends; no PhysicalDrive yet.
#include "byteback_io.h"
#include "imager/ewf_reader.h"
#include "io/byte_source.h"
#include "crypto/byteback_aes.h"
#include "fs/virtual_raid.h" // CA-056: raidBackend_->capacity() needs the complete type

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace byteback {

// CA-007: bounded, representative bad-sector sample list (shared by
// noteBadRead and mergeBadSectorTelemetryFrom).
static constexpr size_t kMaxBadSamples = 4096;

DiskReader::DiskReader()
    : handle_(reinterpret_cast<void*>(-1)), diskSize_(0), sectorSize_(512), currentDriveIndex_(-1),
      shareWrite_(false) {}

DiskReader::~DiskReader() { closeDrive(); }

std::vector<DriveInfo> DiskReader::enumerateDrives() { return {}; }

bool DiskReader::openedWithWriteShare() const { return false; }

bool DiskReader::openDrive(int) { return false; }

bool DiskReader::openVolumePath(const std::string&) { return false; }

void DiskReader::closeDrive() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
}

void DiskReader::closeDriveUnlocked() {
    if (handle_ != reinterpret_cast<void*>(-1)) {
        close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
        handle_ = reinterpret_cast<void*>(-1);
    }
    diskSize_ = 0;
    currentDriveIndex_ = -1;
    shareWrite_ = false;
    volumePath_.clear();
    rawFilePath_.clear();
    ewfPath_.clear();
    ewfPathIsHttp_ = false;
    raidBackend_.reset();
    memoryVolume_.reset();
    memoryMode_ = false;
    ewfBackend_.reset();
    rawBackend_.reset();
    rawBackendIsHttp_ = false;
}

void DiskReader::setRaidBackend(std::shared_ptr<VirtualRaid> raid) {
    // Parity with disk_reader_win.cpp: hold ioMutex_, install the backend, and
    // close ONLY the previous raw handle. The old closeDriveUnlocked() call
    // reset the raid backend it had JUST assigned and zeroed diskSize_, so the
    // backend silently vanished right after being set.
    std::lock_guard<std::mutex> lock(ioMutex_);
    raidBackend_ = std::move(raid);
    if (raidBackend_) {
        diskSize_ = raidBackend_->capacity();
        sectorSize_ = 512;
        currentDriveIndex_ = -1;
        if (handle_ != reinterpret_cast<void*>(-1)) {
            close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
            handle_ = reinterpret_cast<void*>(-1);
        }
    }
}

bool DiskReader::hasRaidBackend() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return static_cast<bool>(raidBackend_);
}

void DiskReader::attachMemoryVolume(std::vector<uint8_t> image, uint32_t sectorSize) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
    // A2: the buffer lives in a shared state so clones reference the SAME
    // bytes (no per-clone copy) and fault injections stay coherent.
    memoryVolume_ = std::make_shared<MemoryVolumeState>();
    memoryVolume_->data = std::move(image);
    sectorSize_ = sectorSize ? sectorSize : 512;
    diskSize_ = memoryVolume_->data.size();
    memoryMode_ = true;
}

void DiskReader::detachMemoryVolume() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    memoryVolume_.reset();
    memoryMode_ = false;
    diskSize_ = 0;
}

bool DiskReader::hasMemoryVolume() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return memoryMode_;
}

void DiskReader::setMemoryFaultRange(uint64_t startSector, uint64_t sectorCount) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!memoryVolume_) return;
    // The range lives in the SHARED state: the write must synchronize against
    // reads on clones (their ioMutex_ does not order against this one).
    memoryVolume_->setFaultRange(startSector, sectorCount);
}

bool DiskReader::attachEwfImage(const std::string& pathOrUrl, std::string* errOut) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
    auto reader = std::make_unique<EwfReader>();
    std::string err;
    if (!reader->open(pathOrUrl, err)) {
        if (errOut) *errOut = err;
        return false;
    }
    ewfBackend_ = std::move(reader);
    sectorSize_ = ewfBackend_->bytesPerSector();
    diskSize_ = ewfBackend_->imageBytes();
    ewfPath_ = pathOrUrl; // A2: clone() re-parses from this path
    ewfPathIsHttp_ = isHttpUrl(pathOrUrl);
    return true;
}

bool DiskReader::attachRawFile(const std::string& path, std::string* errOut) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
    std::string err;
    auto src = openFileByteSource(path, err);
    if (!src) {
        if (errOut) *errOut = err;
        return false;
    }
    rawBackend_ = std::move(src);
    sectorSize_ = 512;
    diskSize_ = rawBackend_->size();
    rawFilePath_ = path; // A2: clone() re-opens this path
    return true;
}

bool DiskReader::attachHttpRawImage(const std::string& url, std::string* errOut) {
    if (errOut) *errOut = "http raw image requires Windows WinHTTP build";
    return false;
}

void DiskReader::detachImageBackend() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    ewfBackend_.reset();
    rawBackend_.reset();
}

bool DiskReader::hasImageBackend() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return static_cast<bool>(ewfBackend_) || static_cast<bool>(rawBackend_);
}

bool DiskReader::setXtsFvek(const uint8_t* key, size_t keyBytes) {
    // Parity with disk_reader_win.cpp: key fields are mutated under ioMutex_
    // (readSectors may run concurrently and reads xtsKeyLen_/xtsKey_).
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!key || (keyBytes != 32 && keyBytes != 64)) {
        xtsKeyLen_ = 0;
        return false;
    }
    std::memcpy(xtsKey_, key, keyBytes);
    xtsKeyLen_ = static_cast<uint8_t>(keyBytes);
    return true;
}

bool DiskReader::setXtsFvek128(const uint8_t* key32, size_t n) { return setXtsFvek(key32, n); }

void DiskReader::clearXtsFvek() {
    std::lock_guard<std::mutex> lock(ioMutex_); // parity with disk_reader_win.cpp
    xtsKeyLen_ = 0;
    std::memset(xtsKey_, 0, sizeof(xtsKey_));
}

bool DiskReader::hasXtsFvek() const {
    std::lock_guard<std::mutex> lock(ioMutex_); // parity with disk_reader_win.cpp
    return xtsKeyLen_ == 32 || xtsKeyLen_ == 64;
}

size_t DiskReader::xtsFvekBytes() const {
    std::lock_guard<std::mutex> lock(ioMutex_); // parity with disk_reader_win.cpp
    return xtsKeyLen_;
}

void DiskReader::copyXtsFvekFrom(const DiskReader& src) {
    if (this == &src) return; // parity with disk_reader_win.cpp: self-copy would deadlock
    uint8_t key[64];
    uint8_t len = 0;
    {
        std::lock_guard<std::mutex> lock(src.ioMutex_);
        len = src.xtsKeyLen_;
        if (len == 32 || len == 64) std::memcpy(key, src.xtsKey_, len);
    }
    if (len == 32 || len == 64) setXtsFvek(key, len);
    else clearXtsFvek();
}

void DiskReader::maybeDecryptXts(uint64_t, uint32_t, uint8_t*) {}

ReadResult DiskReader::readSectors(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer) {
    ReadResult result;
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (sectorSize_ == 0) sectorSize_ = 512;
    if (offsetBytes % sectorSize_ != 0 || sizeBytes % sectorSize_ != 0) {
        result.error = "Read offset and size must be sector-aligned";
        return result;
    }
    if (memoryMode_) {
        if (!memoryVolume_) {
            result.error = "No drive opened";
            return result;
        }
        const MemoryVolumeState& mv = *memoryVolume_;
        // Test hook: a read overlapping the injected fault range fails. The
        // range lives in the shared state (clones included) — snapshot it
        // under its own lock so (start,count) is consistent.
        const auto [fStart, fCount] = mv.faultRange();
        if (fCount > 0 && sectorSize_ > 0) {
            const uint64_t first = offsetBytes / sectorSize_;
            const uint64_t last = first + sizeBytes / sectorSize_ - 1;
            if (first <= fStart + fCount - 1 && fStart <= last) {
                result.error = "injected memory fault range";
                noteBadRead(offsetBytes, sizeBytes);
                return result;
            }
        }
        if (offsetBytes + sizeBytes <= mv.data.size()) {
            std::memcpy(buffer, mv.data.data() + offsetBytes, sizeBytes);
            result.success = true;
            result.bytesRead = sizeBytes;
        }
        return result;
    }
    if (ewfBackend_) {
        std::string err;
        if (ewfBackend_->read(offsetBytes, buffer, sizeBytes, err)) {
            result.success = true;
            result.bytesRead = sizeBytes;
        } else {
            result.error = err;
            noteBadRead(offsetBytes, sizeBytes); // parity with disk_reader_win.cpp
        }
        return result;
    }
    if (rawBackend_) {
        if (offsetBytes % sectorSize_ != 0 || sizeBytes % sectorSize_ != 0) {
            result.error = "Read offset and size must be sector-aligned";
            return result;
        }
        if (!rawBackend_->read(offsetBytes, buffer, sizeBytes)) {
            result.error = rawBackend_->lastError();
            noteBadRead(offsetBytes, sizeBytes); // parity with disk_reader_win.cpp
            return result;
        }
        result.success = true;
        result.bytesRead = sizeBytes;
        return result;
    }
    result.error = "No drive opened";
    return result;
}

uint64_t DiskReader::getDiskSize() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return diskSize_;
}

uint32_t DiskReader::getSectorSize() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return sectorSize_;
}

int DiskReader::getDriveIndex() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return currentDriveIndex_;
}

uint64_t DiskReader::getBadSectorReads() const {
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    return badSectorReads_;
}

std::vector<uint64_t> DiskReader::getBadSectors() const {
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    return badSectorList_;
}

void DiskReader::mergeBadSectorTelemetryFrom(const DiskReader& src) {
    if (this == &src) return;
    std::lock_guard<std::mutex> lockSrc(src.badSectorMutex_);
    if (src.badSectorReads_ == 0 && src.badSectorList_.empty()) return;
    std::lock_guard<std::mutex> lockDst(badSectorMutex_);
    badSectorReads_ += src.badSectorReads_;
    for (uint64_t s : src.badSectorList_) {
        if (badSectorList_.size() >= kMaxBadSamples) break;
        badSectorList_.push_back(s);
    }
}

bool DiskReader::isOpen() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return memoryMode_ || static_cast<bool>(raidBackend_) || static_cast<bool>(ewfBackend_) ||
           static_cast<bool>(rawBackend_);
}

void DiskReader::noteBadRead(uint64_t offsetBytes, uint32_t sizeBytes) {
    // Parity with disk_reader_win.cpp: bounded, representative bad-sector
    // samples behind badSectorMutex_. The POSIX no-op blinded the UI's
    // bad-sector map for every non-Windows backend.
    uint32_t ss = sectorSize_ ? sectorSize_ : 512;
    uint64_t startSector = offsetBytes / ss;
    uint64_t count = (sizeBytes + ss - 1) / ss;
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    badSectorReads_ += count;
    // Keep a bounded, representative sample for the map.
    for (uint64_t i = 0; i < count && badSectorList_.size() < kMaxBadSamples; ++i) {
        // Sub-sample large failures so one dead region doesn't fill the cap.
        if (count > kMaxBadSamples && (i % (count / kMaxBadSamples + 1)) != 0) continue;
        badSectorList_.push_back(startSector + i);
    }
}

} // namespace byteback
#endif
