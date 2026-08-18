#include "scan_coordinator.h"
#include "fs/partition_scanner.h"
#include "fs/virtual_raid.h"
#include <iostream>
#include <chrono>
#include <cstring>

namespace wolf {

ScanCoordinator::ScanCoordinator() : isRunning(false) {}

ScanCoordinator::~ScanCoordinator() {
    stopScan();
}

void ScanCoordinator::startScan(const std::string& drivePath, const std::string& scanType,
                               FileSystemParser::FileRecordCallback onFileFound,
                               ProgressCallback onProgress,
                               std::vector<uint64_t>* badSectorOut,
                               std::shared_ptr<VirtualRaid> raid) {
    if (isRunning) return;
    isRunning = true;

    scanThread = std::thread(&ScanCoordinator::scanWorker, this, drivePath, scanType,
                             onFileFound, onProgress, badSectorOut, std::move(raid));
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
                                ProgressCallback onProgress,
                                std::vector<uint64_t>* badSectorOut,
                                std::shared_ptr<VirtualRaid> raid) {
    DiskReader reader;
    if (raid) {
        reader.setRaidBackend(std::move(raid));
    } else {
        int driveIndex = 0;
        try { driveIndex = std::stoi(drivePath); } catch (...) {
            isRunning = false;
            return;
        }
        if (!reader.openDrive(driveIndex)) {
            isRunning = false;
            return;
        }
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = reader.getDiskSize() / sectorSize;

    auto syncBadSectors = [&]() {
        if (!badSectorOut) return;
        *badSectorOut = reader.getBadSectors();
    };

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (!isRunning) return;
        if (fr.id != -1) onFileFound(fr);
        onProgress(fr.startSector, totalSectors);
        syncBadSectors();
    };

    if (scanType == "quick") {
        {
            std::vector<uint8_t> boot(sectorSize);
            if (reader.readSectors(0, sectorSize, boot.data()).success &&
                boot.size() >= 16 &&
                std::memcmp(boot.data() + 3, "-FVEF-SYS-", 10) == 0) {
                FileRecord fr;
                fr.id = -1;
                fr.name = "[BitLocker] Birim sifreli - kurtarma anahtari gerekli";
                fr.path = "/";
                fr.sizeBytes = 0;
                fr.status = 2;
                fr.confidence = 100;
                fr.category = "Encrypted";
                fr.source = "bitlocker_detect";
                onFileFound(fr);
            }
        }

        PartitionScanner partScanner(&reader);
        std::vector<PartitionInfo> partitions = partScanner.parseMBR();
        std::vector<PartitionInfo> gptParts = partScanner.parseGPT();
        if (!gptParts.empty()) partitions = std::move(gptParts);

        bool anyFsScanned = false;
        for (const auto& part : partitions) {
            if (!isRunning) break;
            if (part.sizeInSectors == 0) continue;

            uint64_t offsetBytes = part.startSector * sectorSize;
            uint64_t partSizeBytes = part.sizeInSectors * sectorSize;
            VolumeFsKind kind = probeVolumeAt(reader, offsetBytes, sectorSize);

            switch (kind) {
                case VolumeFsKind::Ntfs: {
                    NTFSParser ntfs;
                    if (ntfs.scanAt(reader, callbackWrapper, &isRunning, offsetBytes, partSizeBytes)) {
                        anyFsScanned = true;
                    }
                    break;
                }
                case VolumeFsKind::ExFat:
                case VolumeFsKind::Fat: {
                    FATParser fat;
                    if (fat.scanAt(reader, callbackWrapper, &isRunning, offsetBytes)) {
                        anyFsScanned = true;
                    }
                    break;
                }
                case VolumeFsKind::Ext4: {
                    Ext4Parser ext4;
                    if (ext4.scanAt(reader, callbackWrapper, &isRunning, offsetBytes)) {
                        anyFsScanned = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (!anyFsScanned) {
            VolumeFsKind kind0 = probeVolumeAt(reader, 0, sectorSize);
            switch (kind0) {
                case VolumeFsKind::Ntfs: {
                    NTFSParser ntfs;
                    ntfs.scanAt(reader, callbackWrapper, &isRunning, 0, 0);
                    break;
                }
                case VolumeFsKind::ExFat:
                case VolumeFsKind::Fat: {
                    FATParser fat;
                    fat.scan(reader, callbackWrapper, &isRunning);
                    break;
                }
                case VolumeFsKind::Ext4: {
                    Ext4Parser ext4;
                    ext4.scan(reader, callbackWrapper, &isRunning);
                    break;
                }
                default: {
                    NTFSParser ntfs;
                    if (!ntfs.scan(reader, callbackWrapper, &isRunning)) {
                        FATParser fat;
                        if (!fat.scan(reader, callbackWrapper, &isRunning)) {
                            Ext4Parser ext4;
                            ext4.scan(reader, callbackWrapper, &isRunning);
                        }
                    }
                    break;
                }
            }
        }
    } else if (scanType == "deep") {
        CarvingEngine carver;
        if (!carver.loadSignatures("resources/signatures.json")) {
            carver.loadSignatures("signatures.json");
        }
        carver.scan(reader, callbackWrapper, &isRunning);
    }

    syncBadSectors();
    onProgress(totalSectors, totalSectors);
    isRunning = false;
}

} // namespace wolf
