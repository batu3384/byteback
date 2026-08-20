#pragma once

#include <string>
#include <vector>
#include <cstdint>
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

    // EWF (.E01) or raw image over http(s) Range (ponytail: multi-segment EWF local only).
    bool attachEwfImage(const std::string& pathOrUrl, std::string* errOut = nullptr);
    bool attachRawFile(const std::string& path, std::string* errOut = nullptr);
    bool attachHttpRawImage(const std::string& url, std::string* errOut = nullptr);
    void detachImageBackend();
    bool hasImageBackend() const;

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
    std::vector<uint8_t> memoryImage_;
    bool memoryMode_ = false;
    std::unique_ptr<EwfReader> ewfBackend_;
    std::unique_ptr<ByteSource> rawBackend_;
    bool rawBackendIsHttp_ = false;
    uint8_t xtsKey_[64]{};
    uint8_t xtsKeyLen_ = 0; // 0=off, 32=AES-128-XTS, 64=AES-256-XTS

    void maybeDecryptXts(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer);
};

} // namespace byteback
