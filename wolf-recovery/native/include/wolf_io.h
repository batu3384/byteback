#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace wolf {

struct DriveInfo {
    int index;
    std::string model;
    std::string serial;
    uint64_t sizeBytes;
    uint32_t sectorSize;
    std::string type; // "HDD", "SSD", "USB", "Unknown"
};

struct ReadResult {
    bool success;
    uint64_t bytesRead;
    std::string error;
};

class DiskReader {
public:
    DiskReader();
    ~DiskReader();

    // Enumerate all physical drives
    std::vector<DriveInfo> enumerateDrives();

    // Open a drive for reading (returns false on failure)
    bool openDrive(int driveIndex);

    // Close current drive
    void closeDrive();

    // Read sectors from current drive
    // offset and size MUST be sector-aligned
    ReadResult readSectors(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer);

    // Get disk geometry of opened drive
    uint64_t getDiskSize() const;
    uint32_t getSectorSize() const;

    bool isOpen() const;

private:
    void* handle_; // HANDLE on Windows
    uint64_t diskSize_;
    uint32_t sectorSize_;
};

} // namespace wolf
