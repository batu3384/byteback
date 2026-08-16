#include "scan_coordinator.h"
#include "fs/partition_scanner.h"
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
    DiskReader reader;
    int driveIndex = 0;
    try { driveIndex = std::stoi(drivePath); } catch(...) {
        isRunning = false;
        return;
    }
    if (!reader.openDrive(driveIndex)) {
        isRunning = false;
        return;
    }

    uint64_t totalSectors = reader.getDiskSize() / reader.getSectorSize();

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (!isRunning) return;

        if (fr.id != -1) {
            onFileFound(fr);
        }

        // Progress update based on sector
        onProgress(fr.startSector, totalSectors);
    };

    if (scanType == "quick") {
        // Partition-aware quick scan: read the MBR/GPT layout first, then run
        // the filesystem parser that matches each partition's type at the
        // partition's own offset. NTFS stays on the raw-carving path (it finds
        // deleted MFT records anywhere on disk); FAT/exFAT must start at the
        // partition's boot sector; ext4 is detected by its superblock.
        PartitionScanner partScanner(&reader);
        std::vector<PartitionInfo> partitions = partScanner.parseMBR();
        std::vector<PartitionInfo> gptParts = partScanner.parseGPT();
        if (!gptParts.empty()) partitions = std::move(gptParts);

        uint32_t sectorSize = reader.getSectorSize();
        if (sectorSize == 0) sectorSize = 512;

        bool anyFsScanned = false;
        for (const auto& part : partitions) {
            if (!isRunning) break;
            if (part.sizeInSectors == 0) continue;
            uint64_t offsetBytes = part.startSector * sectorSize;

            // FAT family (type 0x06/0x0B/0x0C/0x0E/0x1B/0x1C/0x1E MBR,
            // "ms-basic-data" GPT): boot sector lives at the partition start.
            if (part.type.find("FAT") != std::string::npos ||
                part.type.find("exFAT") != std::string::npos) {
                FATParser fat;
                if (fat.scanAt(reader, callbackWrapper, &isRunning, offsetBytes)) {
                    anyFsScanned = true;
                }
                continue;
            }
        }

        // Filesystems without partition metadata (superfloppy) or partition
        // types we do not special-case: fall back to detection at offset 0.
        if (!anyFsScanned) {
            NTFSParser ntfs;
            bool ntfsOk = ntfs.scan(reader, callbackWrapper, &isRunning);
            if (!ntfsOk) {
                FATParser fat;
                if (!fat.scan(reader, callbackWrapper, &isRunning)) {
                    // Neither NTFS nor FAT at offset 0 — try ext4 anywhere in
                    // the first 4 MiB (superblock magic search is built in).
                    Ext4Parser ext4;
                    ext4.scan(reader, callbackWrapper, &isRunning);
                }
            }
        } else if (isRunning) {
            // Partition layout was scanned, but also run the ext4 pass when
            // a Linux partition type is present.
            for (const auto& part : partitions) {
                if (part.type.find("Linux") != std::string::npos) {
                    Ext4Parser ext4;
                    ext4.scan(reader, callbackWrapper, &isRunning);
                    break;
                }
            }
        }
    } else if (scanType == "deep") {
        CarvingEngine carver;
        // ponytail: signatures path resolved relative to executable; upgrade: pass from JS side
        carver.loadSignatures("signatures.json");
        carver.scan(reader, callbackWrapper, &isRunning);
    }

    onProgress(totalSectors, totalSectors); // Complete
    isRunning = false;
}

} // namespace wolf
