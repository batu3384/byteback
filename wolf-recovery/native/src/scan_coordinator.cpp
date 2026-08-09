#include "scan_coordinator.h"
#include <iostream>
#include <chrono>

namespace wolf {

ScanCoordinator::ScanCoordinator() : isRunning(false) {}

ScanCoordinator::~ScanCoordinator() {
    stopScan();
}

void ScanCoordinator::startScan(const std::string& drivePath, const std::string& scanType, 
                               FileSystemParser::FileRecordCallback onFileFound,
                               ProgressCallback onProgress) {
    if (isRunning) return;
    isRunning = true;
    
    scanThread = std::thread(&ScanCoordinator::scanWorker, this, drivePath, scanType, onFileFound, onProgress);
}

void ScanCoordinator::stopScan() {
    if (isRunning) {
        isRunning = false;
        if (scanThread.joinable()) {
            scanThread.join();
        }
    }
}

void ScanCoordinator::scanWorker(std::string drivePath, std::string scanType, 
                                FileSystemParser::FileRecordCallback onFileFound,
                                ProgressCallback onProgress) {
    DiskReader reader(drivePath);
    if (!reader.open()) {
        isRunning = false;
        return;
    }

    uint64_t totalSectors = reader.getDiskSize() / 512;
    
    auto callbackWrapper = [&](const FileRecord& fr) {
        if (!isRunning) return;
        onFileFound(fr);
        
        // Mock progress update periodically
        static int count = 0;
        if (++count % 100 == 0) {
            onProgress(fr.startSector, totalSectors);
        }
    };

    if (scanType == "quick") {
        NTFSParser ntfs;
        if (!ntfs.scan(reader, callbackWrapper)) {
            FATParser fat;
            fat.scan(reader, callbackWrapper);
        }
    } else if (scanType == "deep") {
        CarvingEngine carver;
        // In a real app, path is constructed robustly
        carver.loadSignatures("signatures.json"); 
        carver.scan(reader, callbackWrapper);
    }
    
    onProgress(totalSectors, totalSectors); // Complete
    isRunning = false;
}

} // namespace wolf



