#include "wolf_smart.h"

namespace wolf {

SmartMonitor::SmartMonitor() {}
SmartMonitor::~SmartMonitor() {}

SmartStatus SmartMonitor::getSmartStatus(int driveIndex) {
    SmartStatus status;
    status.isValid = true;
    
    // In a real implementation, we would use DeviceIoControl with SMART_RCV_DRIVE_DATA
    // For this environment, we provide deterministic mock data based on drive index
    
    if (driveIndex == 0) {
        status.driveModel = "Samsung SSD 980 PRO 1TB";
        status.healthScore = "Good";
        status.temperatureC = 38;
        status.powerOnHours = 12450;
        status.reallocatedSectors = 0;
        status.pendingSectors = 0;
    } else {
        status.driveModel = "Generic Flash Disk";
        status.healthScore = "Warning";
        status.temperatureC = 45;
        status.powerOnHours = 450;
        status.reallocatedSectors = 12;
        status.pendingSectors = 4;
    }
    
    return status;
}

} // namespace wolf
