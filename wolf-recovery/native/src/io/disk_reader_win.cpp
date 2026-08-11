#include "wolf_io.h"

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
                case BusTypeSata:
                case BusTypeAta: info.type = "HDD"; break;
                case BusTypeNvme: info.type = "SSD"; break;
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
    closeDrive();

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

void DiskReader::closeDrive() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = INVALID_HANDLE_VALUE;
    }
    diskSize_ = 0;
    currentDriveIndex_ = -1;
}

ReadResult DiskReader::readSectors(uint64_t offsetBytes, uint32_t sizeBytes, uint8_t* buffer) {
    ReadResult result = { false, 0, "" };

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
        DWORD waitRes = WaitForSingleObject(hEvent, 500);
        if (waitRes == WAIT_TIMEOUT) {
            CancelIoEx(static_cast<HANDLE>(handle_), &ol);
            GetOverlappedResult(static_cast<HANDLE>(handle_), &ol, &bytesRead, TRUE);
            result.error = "Read timed out (500ms)";
            result.success = false;
            CloseHandle(hEvent);
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
    }

    CloseHandle(hEvent);
    return result;
}

uint64_t DiskReader::getDiskSize() const { return diskSize_; }
uint32_t DiskReader::getSectorSize() const { return sectorSize_; }
int DiskReader::getDriveIndex() const { return currentDriveIndex_; }
bool DiskReader::isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

} // namespace wolf
#endif

