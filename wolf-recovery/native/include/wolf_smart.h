#pragma once

#include <string>

namespace wolf {

struct SmartStatus {
    std::string driveModel;
    std::string healthScore; // e.g., "Good", "Warning", "Bad"
    int temperatureC;
    int powerOnHours;
    int reallocatedSectors;
    int pendingSectors;
    bool isValid;
};

class SmartMonitor {
public:
    SmartMonitor();
    ~SmartMonitor();

    // Retrieves SMART data for a given drive index
    SmartStatus getSmartStatus(int driveIndex);
};

} // namespace wolf
