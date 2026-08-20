#include "byteback_smart.h"
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <cstring>

#pragma pack(push, 1)
struct SMART_ATTRIBUTE {
    uint8_t Id;
    uint16_t StatusFlags;
    uint8_t CurrentValue;
    uint8_t WorstValue;
    uint8_t RawData[6];
    uint8_t Reserved;
};
struct SMART_DATA {
    uint16_t Version;
    SMART_ATTRIBUTE Attributes[30];
};
#pragma pack(pop)

namespace byteback {

SmartMonitor::SmartMonitor() {}
SmartMonitor::~SmartMonitor() {}

bool SmartMonitor::queryDescriptor(void* hDeviceRaw, std::string& modelOut, uint8_t& busTypeOut) {
    HANDLE hDevice = static_cast<HANDLE>(hDeviceRaw);
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    // The descriptor is variable-length; a 4 KiB buffer is comfortably enough
    // for the fixed part + vendor/product/revision strings.
    uint8_t buffer[4096] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         buffer, sizeof(buffer), &returned, NULL)) {
        return false;
    }

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    busTypeOut = desc->BusType;

    auto stringAt = [&](DWORD offset) -> const char* {
        if (offset == 0 || offset >= sizeof(buffer)) return nullptr;
        return reinterpret_cast<const char*>(buffer + offset);
    };

    const char* product = stringAt(desc->ProductIdOffset);
    const char* vendor = stringAt(desc->VendorIdOffset);
    std::string model;
    if (product) model = product;
    if (vendor && *vendor) {
        model = std::string(vendor) + (model.empty() ? "" : " ") + model;
    }
    // Trim trailing/leading whitespace.
    while (!model.empty() && (model.back() == ' ' || model.back() == '\n')) model.pop_back();
    size_t start = model.find_first_not_of(' ');
    if (start != std::string::npos) model = model.substr(start);
    if (!model.empty()) modelOut = model;
    return true;
}

bool SmartMonitor::querySeekPenalty(void* hDeviceRaw, bool& incursSeekPenaltyOut) {
    HANDLE hDevice = static_cast<HANDLE>(hDeviceRaw);
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR desc = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         &desc, sizeof(desc), &returned, NULL)) {
        return false;
    }
    incursSeekPenaltyOut = desc.IncursSeekPenalty != 0;
    return true;
}

bool SmartMonitor::queryNvmeHealth(void* hDeviceRaw, SmartStatus& status) {
    HANDLE hDevice = static_cast<HANDLE>(hDeviceRaw);
    // Query layout: STORAGE_PROPERTY_QUERY header followed by
    // STORAGE_PROTOCOL_SPECIFIC_DATA followed by a 512-byte log buffer.
    struct {
        STORAGE_PROPERTY_QUERY query;
        STORAGE_PROTOCOL_SPECIFIC_DATA protocol;
        uint8_t log[512];
    } in = {};

    in.query.PropertyId = StorageDeviceProtocolSpecificProperty;
    in.query.QueryType = PropertyStandardQuery;
    in.protocol.ProtocolType = ProtocolTypeNvme;
    in.protocol.DataType = NVMeDataTypeLogPage;
    in.protocol.ProtocolDataRequestValue = 0x02; // Health Info log
    in.protocol.ProtocolDataRequestSubValue = 0; // controller 0
    in.protocol.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    in.protocol.ProtocolDataLength = sizeof(in.log);

    DWORD returned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                         &in, sizeof(in),
                         &in, sizeof(in), &returned, NULL)) {
        return false;
    }
    const uint8_t* log = in.log;
    if (returned < sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512) {
        return false;
    }

    auto readU16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(log[off]) | (static_cast<uint16_t>(log[off + 1]) << 8);
    };
    // 128-bit counters (we keep the low 64 bits — the high half is zero for
    // any realistic drive lifetime).
    auto readU128lo = [&](size_t off) -> uint64_t {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | log[off + i];
        return v;
    };

    status.criticalWarning = log[0];
    // Composite temperature is Kelvin (u16).
    int kelvin = readU16(1);
    status.temperatureC = kelvin > 273 ? kelvin - 273 : 0;
    status.availableSpare = log[3];
    status.availableSpareThreshold = log[4];
    status.percentageUsed = log[5];

    uint64_t duWritten = readU128lo(48);
    // One data unit = 512,000 bytes (1000 * 512) per NVMe spec.
    status.totalBytesWritten = duWritten * 500000ull;
    status.unsafeShutdowns = readU128lo(144);
    status.powerOnHours = static_cast<int>(readU128lo(128));
    status.mediaErrors = readU128lo(160);

    status.isNvme = true;
    return true;
}

bool SmartMonitor::queryAtaSmart(void* hDeviceRaw, SmartStatus& status, std::string& modelOut) {
    HANDLE hDevice = static_cast<HANDLE>(hDeviceRaw);
    // ---- IDENTIFY DEVICE for the model string (words 27..46, byte-swapped) ----
    {
        SENDCMDINPARAMS cmdIn = {};
        cmdIn.irDriveRegs.bCommandReg = 0xEC; // ID_CMD
        uint8_t buffer[sizeof(SENDCMDOUTPARAMS) + 512] = {};
        DWORD returned = 0;
        if (DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
                            &cmdIn, sizeof(SENDCMDINPARAMS),
                            buffer, sizeof(buffer), &returned, NULL)) {
            auto* pOut = reinterpret_cast<SENDCMDOUTPARAMS*>(buffer);
            const uint8_t* id = pOut->bBuffer;
            char model[41] = {};
            for (int w = 0; w < 20; ++w) {
                model[w * 2] = static_cast<char>(id[27 * 2 + w * 2 + 1]);
                model[w * 2 + 1] = static_cast<char>(id[27 * 2 + w * 2]);
            }
            std::string m(model, 40);
            while (!m.empty() && m.back() == ' ') m.pop_back();
            size_t s = m.find_first_not_of(' ');
            if (s != std::string::npos && !m.empty()) modelOut = m.substr(s);
        }
    }

    // ---- SMART attribute table ----
    SENDCMDINPARAMS cmdIn = {};
    SENDCMDOUTPARAMS cmdOut = {};
    DWORD returned = 0;

    cmdIn.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
    cmdIn.irDriveRegs.bSectorCountReg = 1;
    cmdIn.irDriveRegs.bSectorNumberReg = 1;
    cmdIn.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    cmdIn.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    cmdIn.irDriveRegs.bCommandReg = SMART_CMD;

    uint8_t buffer[sizeof(SENDCMDOUTPARAMS) + READ_ATTRIBUTE_BUFFER_SIZE] = {};

    if (!DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
                         &cmdIn, sizeof(SENDCMDINPARAMS),
                         buffer, sizeof(buffer), &returned, NULL)) {
        return false;
    }

    auto* pOut = reinterpret_cast<SENDCMDOUTPARAMS*>(buffer);
    auto* pSmart = reinterpret_cast<SMART_DATA*>(pOut->bBuffer);

    for (int i = 0; i < 30; ++i) {
        auto& attr = pSmart->Attributes[i];
        if (attr.Id == 0) continue;

        uint64_t rawValue = 0;
        for (int j = 0; j < 6; ++j) {
            rawValue |= (static_cast<uint64_t>(attr.RawData[j]) << (j * 8));
        }

        switch (attr.Id) {
            case 0x05: status.reallocatedSectors = static_cast<int>(rawValue); break;
            case 0x09: status.powerOnHours = static_cast<int>(rawValue); break;
            case 0xC2: status.temperatureC = static_cast<int>(rawValue & 0xFF); break;
            case 0xC5: status.pendingSectors = static_cast<int>(rawValue); break;
            // 0xF1 Total Host Writes (in GB on most vendors; some use sectors).
            // Treated as GB — the dominant vendor convention.
            case 0xF1: status.totalBytesWritten = rawValue * 1000000000ull; break;
            default: break;
        }
    }
    return true;
}

SmartStatus SmartMonitor::getSmartStatus(int driveIndex) {
    SmartStatus status;
    status.isValid = false;

    wchar_t path[64];
    swprintf_s(path, L"\\\\.\\PhysicalDrive%d", driveIndex);

    HANDLE hDevice = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        return status;
    }

    std::string model = "Unknown";
    uint8_t busType = 0;
    bool haveDescriptor = queryDescriptor(hDevice, model, busType);

    // Rotational detection: no seek penalty -> SSD (TRIM applies).
    bool incursSeekPenalty = true;
    if (querySeekPenalty(hDevice, incursSeekPenalty)) {
        status.seekPenaltyKnown = true;
        status.isSsd = !incursSeekPenalty;
    }

    bool ok = false;
    if (haveDescriptor && busType == 0x11 /* BusTypeNvme */) {
        ok = queryNvmeHealth(hDevice, status);
        if (ok && model == "Unknown") model = "NVMe Controller";
    }
    if (!ok) {
        std::string ataModel;
        ok = queryAtaSmart(hDevice, status, ataModel);
        if (ok && !ataModel.empty()) model = ataModel;
    }

    CloseHandle(hDevice);

    if (!ok) return status; // isValid stays false

    status.isValid = true;
    status.driveModel = model;

    // ---- Health scoring ----
    if (status.isNvme) {
        // NVMe: critical warning bits are authoritative; spare/percentageUsed
        // carry wear information.
        if (status.criticalWarning != 0) {
            status.healthScore = "Bad";
        } else if ((status.availableSpare >= 0 && status.availableSpareThreshold >= 0 &&
                    status.availableSpare <= status.availableSpareThreshold) ||
                   status.percentageUsed > 100) {
            status.healthScore = "Warning";
        } else {
            status.healthScore = "Good";
        }
        return status;
    }

    // ATA ACS / Backblaze: SMART 0x05 realloc + 0xC5 pending. No Weibull.
    status.healthScore = ataHealthFromDefects(status.reallocatedSectors, status.pendingSectors);
    return status;
}

} // namespace byteback
