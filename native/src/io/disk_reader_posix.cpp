#ifndef _WIN32
// ponytail: minimal POSIX disk reader — memory + file/EWF backends; no PhysicalDrive yet.
#include "byteback_io.h"
#include "imager/ewf_reader.h"
#include "io/byte_source.h"
#include "crypto/byteback_aes.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace byteback {

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
    raidBackend_.reset();
    memoryImage_.clear();
    memoryMode_ = false;
    ewfBackend_.reset();
    rawBackend_.reset();
    rawBackendIsHttp_ = false;
}

void DiskReader::setRaidBackend(std::shared_ptr<VirtualRaid> raid) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    raidBackend_ = std::move(raid);
    if (raidBackend_) {
        diskSize_ = raidBackend_->capacity();
        sectorSize_ = 512;
        currentDriveIndex_ = -1;
        closeDriveUnlocked();
    }
}

bool DiskReader::hasRaidBackend() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return static_cast<bool>(raidBackend_);
}

void DiskReader::attachMemoryVolume(std::vector<uint8_t> image, uint32_t sectorSize) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
    memoryImage_ = std::move(image);
    sectorSize_ = sectorSize ? sectorSize : 512;
    diskSize_ = memoryImage_.size();
    memoryMode_ = true;
}

void DiskReader::detachMemoryVolume() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    memoryImage_.clear();
    memoryMode_ = false;
    diskSize_ = 0;
}

bool DiskReader::hasMemoryVolume() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return memoryMode_;
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
    xtsKeyLen_ = 0;
    std::memset(xtsKey_, 0, sizeof(xtsKey_));
}

bool DiskReader::hasXtsFvek() const { return xtsKeyLen_ == 32 || xtsKeyLen_ == 64; }

size_t DiskReader::xtsFvekBytes() const { return xtsKeyLen_; }

void DiskReader::copyXtsFvekFrom(const DiskReader& src) {
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
        if (offsetBytes + sizeBytes <= memoryImage_.size()) {
            std::memcpy(buffer, memoryImage_.data() + offsetBytes, sizeBytes);
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
        }
        return result;
    }
    if (rawBackend_ && rawBackend_->read(offsetBytes, buffer, sizeBytes)) {
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

uint64_t DiskReader::getBadSectorReads() const { return badSectorReads_; }

std::vector<uint64_t> DiskReader::getBadSectors() const { return badSectorList_; }

bool DiskReader::isOpen() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return memoryMode_ || static_cast<bool>(raidBackend_) || static_cast<bool>(ewfBackend_) ||
           static_cast<bool>(rawBackend_);
}

void DiskReader::noteBadRead(uint64_t, uint32_t) {}

} // namespace byteback
#endif
