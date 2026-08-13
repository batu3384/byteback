#include "wolf_smart.h"
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <cmath> // For std::exp, std::pow (Weibull/Exponential calculations)

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

namespace wolf {

SmartMonitor::SmartMonitor() {}
SmartMonitor::~SmartMonitor() {}

SmartStatus SmartMonitor::getSmartStatus(int driveIndex) {
    SmartStatus status;
    status.isValid = false;
    status.driveModel = "Unknown";
    status.healthScore = "Unknown";
    status.temperatureC = 0;
    status.powerOnHours = 0;
    status.reallocatedSectors = 0;
    status.pendingSectors = 0;

    wchar_t path[64];
    swprintf_s(path, L"\\\\.\\PhysicalDrive%d", driveIndex);

    HANDLE hDevice = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        return status;
    }

    SENDCMDINPARAMS cmdIn = {0};
    SENDCMDOUTPARAMS cmdOut = {0};
    DWORD bytesReturned = 0;

    cmdIn.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
    cmdIn.irDriveRegs.bSectorCountReg = 1;
    cmdIn.irDriveRegs.bSectorNumberReg = 1;
    cmdIn.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    cmdIn.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    cmdIn.irDriveRegs.bCommandReg = SMART_CMD;

    uint8_t buffer[sizeof(SENDCMDOUTPARAMS) + READ_ATTRIBUTE_BUFFER_SIZE] = {0};
    
    if (DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
        &cmdIn, sizeof(SENDCMDINPARAMS),
        buffer, sizeof(buffer),
        &bytesReturned, NULL)) {
        
        status.isValid = true;
        status.healthScore = "Good";
        
        SENDCMDOUTPARAMS* pOut = reinterpret_cast<SENDCMDOUTPARAMS*>(buffer);
        SMART_DATA* pSmart = reinterpret_cast<SMART_DATA*>(pOut->bBuffer);

        for (int i = 0; i < 30; ++i) {
            auto& attr = pSmart->Attributes[i];
            if (attr.Id == 0) continue;

            uint64_t rawValue = 0;
            for (int j = 0; j < 6; ++j) {
                rawValue |= (static_cast<uint64_t>(attr.RawData[j]) << (j * 8));
            }

            if (attr.Id == 0x05) status.reallocatedSectors = static_cast<int>(rawValue); // Reallocated Sectors
            if (attr.Id == 0x09) status.powerOnHours = static_cast<int>(rawValue); // Power-On Hours
            if (attr.Id == 0xC2) status.temperatureC = static_cast<int>(rawValue & 0xFF); // Temperature
            if (attr.Id == 0xC5) status.pendingSectors = static_cast<int>(rawValue); // Current Pending Sector
        }
        
        // Mathematical Failure Prediction (Physics/Reliability Engineering)
        // Using a simplified Weibull distribution model for mechanical/flash failure probability
        // P(t) = 1 - exp(-(t/eta)^beta)
        // Here we use defect counts as proxies for time/stress
        
        double criticalErrors = status.reallocatedSectors + (status.pendingSectors * 1.5);
        double eta = 500.0; // Characteristic life in errors
        double beta = 1.5;  // Shape parameter (wear-out phase)
        
        double failureProbability = 1.0 - std::exp(-std::pow(criticalErrors / eta, beta));
        double healthPercentage = (1.0 - failureProbability) * 100.0;
        
        if (healthPercentage > 90.0) {
            status.healthScore = "Good";
        } else if (healthPercentage > 50.0) {
            status.healthScore = "Warning";
        } else {
            status.healthScore = "Bad";
        }
    }

    CloseHandle(hDevice);
    return status;
}

} // namespace wolf
