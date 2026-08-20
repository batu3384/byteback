#pragma once

#include <string>
#include <cstdint>

// windows.h is deliberately NOT included here: its kernel32 declarations
// (GetVersion et al.) collide with same-named NAPI bridge functions in
// translation units that include both. The private helpers take the handle
// as void* and cast to HANDLE inside smart_monitor.cpp.

namespace byteback {

struct SmartStatus {
    std::string driveModel;
    std::string healthScore; // "Good", "Warning", "Bad"
    int temperatureC = 0;
    int powerOnHours = 0;
    int reallocatedSectors = 0;
    int pendingSectors = 0;
    bool isValid = false;

    // --- NVMe Health Info Log (Log ID 0x02) fields; ATA drives leave these
    // at their defaults. ---
    bool isNvme = false;
    // 0..100+% of rated endurance consumed (NVMe Percentage Used).
    int percentageUsed = -1;
    // Available spare as % of capacity; -1 when not reported.
    int availableSpare = -1;
    int availableSpareThreshold = -1;
    // Bitmap: bit0 readonly, bit1 volatile memory backup failed,
    // bit2 media readonly, bit3 volatile memory degraded.
    uint8_t criticalWarning = 0;
    uint64_t unsafeShutdowns = 0;
    uint64_t mediaErrors = 0;
    // Total bytes written (ATA: from host-writes attribute when available;
    // NVMe: Data Units Written * 512000).
    uint64_t totalBytesWritten = 0;

    // Rotational-media detection via StorageDeviceSeekPenaltyProperty.
    // SSDs with TRIM make deleted-file recovery largely impossible once
    // garbage collection runs — the UI must warn about this honestly.
    bool isSsd = false;
    // True when the drive reports no seek penalty (rotational = false).
    bool seekPenaltyKnown = false;
};

// ATA ACS / Backblaze triage: pre-fail counters 0x05 (reallocated) and
// 0xC5 (pending). Any pending+realloc together is Bad; either alone is
// Warning. No Weibull, no uncalibrated eta/beta.
inline const char* ataHealthFromDefects(int reallocatedSectors, int pendingSectors) {
    if (reallocatedSectors < 0) reallocatedSectors = 0;
    if (pendingSectors < 0) pendingSectors = 0;
    if (reallocatedSectors > 0 && pendingSectors > 0) return "Bad";
    if (reallocatedSectors > 0 || pendingSectors > 0) return "Warning";
    return "Good";
}

class SmartMonitor {
public:
    SmartMonitor();
    ~SmartMonitor();

    // Retrieves SMART/health data for a given drive index. Dispatches to the
    // NVMe health-log path or the ATA SMART path depending on bus type.
    SmartStatus getSmartStatus(int driveIndex);

private:
    // All helpers take the raw device handle as void* (see the windows.h note
    // above) and cast internally.
    bool queryDescriptor(void* hDevice, std::string& modelOut, uint8_t& busTypeOut);
    bool querySeekPenalty(void* hDevice, bool& incursSeekPenaltyOut);
    bool queryNvmeHealth(void* hDevice, SmartStatus& status);
    bool queryAtaSmart(void* hDevice, SmartStatus& status, std::string& modelOut);
};

} // namespace byteback
