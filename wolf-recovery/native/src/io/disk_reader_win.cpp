#include "wolf_io.h"
#include "fs/virtual_raid.h"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <ntddstor.h>
#include <devguid.h>
#include <winioctl.h>
#include <cstdio>

namespace wolf {

DiskReader::DiskReader()
    : handle_(INVALID_HANDLE_VALUE), diskSize_(0), sectorSize_(512), currentDriveIndex_(-1) {}

DiskReader::~DiskReader() {
    closeDrive();
}

std::vector<DriveInfo> DiskReader::enumerateDrives() {
    std::vector<DriveInfo> drives;

    for (int i = 0; i < 32; ++i) {
        wchar_t path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", i);

        HANDLE h = CreateFileW(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h == INVALID_HANDLE_VALUE) continue;

        DriveInfo info;
        info.index = i;
        info.type = "Unknown";

        // Get geometry
        DISK_GEOMETRY_EX geo;
        DWORD bytesReturned = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                NULL, 0, &geo, sizeof(geo), &bytesReturned, NULL)) {
            info.sizeBytes = geo.DiskSize.QuadPart;
            info.sectorSize = geo.Geometry.BytesPerSector;
        } else {
            info.sizeBytes = 0;
            info.sectorSize = 512;
        }

        // Get model/serial via STORAGE_PROPERTY_QUERY
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        uint8_t buffer[4096];
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                &query, sizeof(query), buffer, sizeof(buffer),
                &bytesReturned, NULL)) {
            auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);

            if (desc->VendorIdOffset > 0) {
                std::string vendor(reinterpret_cast<char*>(buffer + desc->VendorIdOffset));
                info.model = vendor;
            }
            if (desc->ProductIdOffset > 0) {
                std::string product(reinterpret_cast<char*>(buffer + desc->ProductIdOffset));
                if (!info.model.empty()) info.model += " ";
                info.model += product;
            }
            if (desc->SerialNumberOffset > 0) {
                info.serial = std::string(
                    reinterpret_cast<char*>(buffer + desc->SerialNumberOffset));
            }

            // Detect bus type for SSD/HDD/USB
            switch (desc->BusType) {
                case BusTypeUsb: info.type = "USB"; break;
                case BusTypeNvme: info.type = "SSD"; break;
                case BusTypeSata:
                case BusTypeAta: {
                    info.type = "HDD";
                    STORAGE_PROPERTY_QUERY spq = {};
                    spq.PropertyId = StorageDeviceSeekPenaltyProperty;
                    spq.QueryType = PropertyStandardQuery;
                    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty = {};
                    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &spq, sizeof(spq), &penalty, sizeof(penalty),
                            &bytesReturned, NULL) && penalty.Version >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
                        if (!penalty.IncursSeekPenalty) info.type = "SSD";
                    }
                    break;
                }
                default: info.type = "Unknown"; break;
            }
        }

        // Trim whitespace from model/serial
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
            size_t start = 0;
            while (start < s.length() && (s[start] == ' ' || s[start] == '\0')) start++;
            if (start > 0) s = s.substr(start);
        };
        trim(info.model);
        trim(info.serial);

        drives.push_back(info);
        CloseHandle(h);
    }

    return drives;
}

bool DiskReader::openDrive(int driveIndex) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();

    // Fresh session, fresh telemetry (closeDrive may keep the handle null
    // but the counters belong to the previous volume).
    {
        std::lock_guard<std::mutex> lock(badSectorMutex_);
        badSectorReads_ = 0;
        badSectorList_.clear();
    }

    wchar_t path[64];
    swprintf_s(path, L"\\\\.\\PhysicalDrive%d", driveIndex);

    handle_ = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
        NULL);

    if (handle_ == INVALID_HANDLE_VALUE) return false;

    currentDriveIndex_ = driveIndex;

    // Get geometry
    DISK_GEOMETRY_EX geo;
    DWORD bytesReturned = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            NULL, 0, &geo, sizeof(geo), &bytesReturned, NULL)) {
        diskSize_ = geo.DiskSize.QuadPart;
        sectorSize_ = geo.Geometry.BytesPerSector;
    }

    return true;
}

bool DiskReader::openVolumePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
    {
        std::lock_guard<std::mutex> lock(badSectorMutex_);
        badSectorReads_ = 0;
        badSectorList_.clear();
    }

    std::wstring wpath(path.begin(), path.end());
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    handle_ = h;
    currentDriveIndex_ = -1;
    sectorSize_ = 512;
    diskSize_ = 0;

    DISK_GEOMETRY_EX geo{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr, 0, &geo, sizeof(geo), &bytesReturned, nullptr)) {
        diskSize_ = geo.DiskSize.QuadPart;
        if (geo.Geometry.BytesPerSector) sectorSize_ = geo.Geometry.BytesPerSector;
        return true;
    }

    GET_LENGTH_INFORMATION len{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &len, sizeof(len), &bytesReturned, nullptr)) {
        diskSize_ = len.Length.QuadPart;
        return true;
    }

    CloseHandle(h);
    handle_ = INVALID_HANDLE_VALUE;
    return false;
}

void DiskReader::closeDrive() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    closeDriveUnlocked();
}

void DiskReader::closeDriveUnlocked() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = INVALID_HANDLE_VALUE;
    }
    diskSize_ = 0;
    currentDriveIndex_ = -1;
    raidBackend_.reset();
    memoryImage_.clear();
    memoryMode_ = false;
}

void DiskReader::setRaidBackend(std::shared_ptr<VirtualRaid> raid) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    raidBackend_ = std::move(raid);
    if (raidBackend_) {
        diskSize_ = raidBackend_->capacity();
        sectorSize_ = 512;
        currentDriveIndex_ = -1;
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle_));
            handle_ = INVALID_HANDLE_VALUE;
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
    memoryImage_ = std::move(image);
    sectorSize_ = sectorSize ? sectorSize : 512;
    diskSize_ = memoryImage_.size();
    memoryMode_ = true;
    currentDriveIndex_ = -1;
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

void DiskReader::noteBadRead(uint64_t offsetBytes, uint32_t sizeBytes) {
    uint32_t ss = sectorSize_ ? sectorSize_ : 512;
    uint64_t startSector = offsetBytes / ss;
    uint64_t count = (sizeBytes + ss - 1) / ss;
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    badSectorReads_ += count;
    // Keep a bounded, representative sample for the map.
    constexpr size_t kMaxBadSamples = 4096;
    for (uint64_t i = 0; i < count && badSectorList_.size() < kMaxBadSamples; ++i) {
        // Sub-sample large failures so one dead region doesn't fill the cap.
        if (count > kMaxBadSamples && (i % (count / kMaxBadSamples + 1)) != 0) continue;
        badSectorList_.push_back(startSector + i);
    }
}

uint64_t DiskReader::getBadSectorReads() const {
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    return badSectorReads_;
}

std::vector<uint64_t> DiskReader::getBadSectors() const {
    std::lock_guard<std::mutex> lock(badSectorMutex_);
    return badSectorList_;
}

ReadResult DiskReader::readSectors(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    ReadResult result;

    if (sectorSize_ == 0) sectorSize_ = 512;

    if (memoryMode_) {
        if (offsetBytes % sectorSize_ != 0 || sizeBytes % sectorSize_ != 0) {
            result.error = "Read offset and size must be sector-aligned";
            return result;
        }
        if (offsetBytes + sizeBytes > memoryImage_.size()) {
            std::memset(buffer, 0, sizeBytes);
            if (offsetBytes < memoryImage_.size()) {
                size_t avail = static_cast<size_t>(
                    std::min<uint64_t>(sizeBytes, memoryImage_.size() - offsetBytes));
                std::memcpy(buffer, memoryImage_.data() + offsetBytes, avail);
                result.bytesRead = static_cast<uint64_t>(avail);
            }
            noteBadRead(offsetBytes, sizeBytes);
            result.success = true;
            result.paddedZeros = true;
            return result;
        }
        std::memcpy(buffer, memoryImage_.data() + offsetBytes, sizeBytes);
        result.success = true;
        result.bytesRead = sizeBytes;
        return result;
    }

    if (raidBackend_) {
        if (offsetBytes % sectorSize_ != 0 || sizeBytes % sectorSize_ != 0) {
            result.error = "Read offset and size must be sector-aligned";
            return result;
        }
        try {
            auto data = raidBackend_->read(static_cast<size_t>(offsetBytes),
                                           static_cast<size_t>(sizeBytes));
            if (data.size() < sizeBytes) {
                std::memset(buffer, 0, sizeBytes);
                if (!data.empty()) {
                    std::memcpy(buffer, data.data(), data.size());
                }
                noteBadRead(offsetBytes, sizeBytes);
                result.success = true;
                result.paddedZeros = true;
                result.bytesRead = sizeBytes;
                return result;
            }
            std::memcpy(buffer, data.data(), sizeBytes);
            result.success = true;
            result.bytesRead = sizeBytes;
            return result;
        } catch (const std::exception& e) {
            result.error = e.what();
            std::memset(buffer, 0, sizeBytes);
            noteBadRead(offsetBytes, sizeBytes);
            result.paddedZeros = true;
            return result;
        }
    }

    if (handle_ == INVALID_HANDLE_VALUE) {
        result.error = "No drive opened";
        return result;
    }

    // Validate alignment
    if (offsetBytes % sectorSize_ != 0 || sizeBytes % sectorSize_ != 0) {
        result.error = "Read offset and size must be sector-aligned";
        return result;
    }

    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hEvent) {
        result.error = "Failed to create event";
        return result;
    }

    OVERLAPPED ol = {0};
    ol.Offset = static_cast<DWORD>(offsetBytes & 0xFFFFFFFF);
    ol.OffsetHigh = static_cast<DWORD>(offsetBytes >> 32);
    ol.hEvent = hEvent;

    BOOL bRead = ReadFile(static_cast<HANDLE>(handle_), buffer, sizeBytes, NULL, &ol);
    DWORD err = bRead ? ERROR_SUCCESS : GetLastError();
    DWORD bytesRead = 0;

    if (!bRead && err == ERROR_IO_PENDING) {
        DWORD waitRes = WaitForSingleObject(hEvent, 5000);
        if (waitRes == WAIT_TIMEOUT) {
            CancelIoEx(static_cast<HANDLE>(handle_), &ol);
            GetOverlappedResult(static_cast<HANDLE>(handle_), &ol, &bytesRead, TRUE);
            result.error = "Read timed out (5000ms)";
            result.success = false;
            CloseHandle(hEvent);
            noteBadRead(offsetBytes, sizeBytes);
            return result;
        } else if (waitRes == WAIT_OBJECT_0) {
            bRead = GetOverlappedResult(static_cast<HANDLE>(handle_), &ol, &bytesRead, FALSE);
            if (!bRead) err = GetLastError();
        } else {
            CancelIoEx(static_cast<HANDLE>(handle_), &ol);
            GetOverlappedResult(static_cast<HANDLE>(handle_), &ol, &bytesRead, TRUE);
            result.error = "Wait failed: " + std::to_string(GetLastError());
            result.success = false;
            CloseHandle(hEvent);
            return result;
        }
    } else if (bRead) {
        bRead = GetOverlappedResult(static_cast<HANDLE>(handle_), &ol, &bytesRead, FALSE);
        if (!bRead) err = GetLastError();
    }

    if (bRead) {
        result.success = true;
        result.bytesRead = bytesRead;
    } else {
        result.error = "ReadFile failed: error " + std::to_string(err);
        if (err == ERROR_CRC || err == ERROR_IO_DEVICE) {
            result.bytesRead = 0;
            result.error += " (bad sector)";
        }
        noteBadRead(offsetBytes, sizeBytes);
    }

    CloseHandle(hEvent);
    return result;
}

uint64_t DiskReader::getDiskSize() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (raidBackend_) return raidBackend_->capacity();
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
bool DiskReader::isOpen() const {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return handle_ != INVALID_HANDLE_VALUE || memoryMode_ || static_cast<bool>(raidBackend_);
}

} // namespace wolf
#endif

