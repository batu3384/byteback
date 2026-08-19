#include "scan_coordinator.h"
#include "fs/partition_scanner.h"
#include "fs/virtual_raid.h"
#include "fs/vss_scanner.h"
#include "wolf_fs.h"
#include <iostream>
#include <chrono>
#include <cstring>

namespace wolf {

namespace {

using ProgressCallback = ScanCoordinator::ProgressCallback;

void syncBadSectors(DiskReader& reader, std::vector<uint64_t>* badSectorOut) {
    if (!badSectorOut) return;
    *badSectorOut = reader.getBadSectors();
}

} // namespace

void tagRaidScanSource(FileRecord& fr, const DiskReader& reader) {
    if (!reader.hasRaidBackend()) return;
    if (fr.source.empty()) return;
    if (fr.source == "vss_snapshot" || fr.source == "bitlocker_detect") return;
    if (fr.source.rfind("raid_", 0) == 0) return;
    fr.source = "raid_" + fr.source;
}

void runQuickScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = reader.getDiskSize() / sectorSize;

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (isRunning && !(*isRunning)) return;
        FileRecord out = fr;
        tagRaidScanSource(out, reader);
        if (!out.name.empty() || out.id != -1) onFileFound(out);
        onProgress(out.startSector, totalSectors);
        syncBadSectors(reader, badSectorOut);
    };

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
        if (isRunning && !(*isRunning)) break;
        if (part.sizeInSectors == 0) continue;

        uint64_t offsetBytes = part.startSector * sectorSize;
        uint64_t partSizeBytes = part.sizeInSectors * sectorSize;
        VolumeFsKind kind = probeVolumeAt(reader, offsetBytes, sectorSize);

        switch (kind) {
            case VolumeFsKind::Ntfs: {
                NTFSParser ntfs;
                if (ntfs.scanAt(reader, callbackWrapper, isRunning, offsetBytes, partSizeBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            case VolumeFsKind::ExFat:
            case VolumeFsKind::Fat: {
                FATParser fat;
                if (fat.scanAt(reader, callbackWrapper, isRunning, offsetBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            case VolumeFsKind::Ext4: {
                Ext4Parser ext4;
                if (ext4.scanAt(reader, callbackWrapper, isRunning, offsetBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            case VolumeFsKind::Apfs: {
                APFSParser apfs;
                if (apfs.scanAt(reader, callbackWrapper, isRunning, offsetBytes, partSizeBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            case VolumeFsKind::Hfs: {
                HFSParser hfs;
                if (hfs.scanAt(reader, callbackWrapper, isRunning, offsetBytes, partSizeBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            default:
                break;
        }
    }

#ifdef _WIN32
    scanVssSnapshots(onFileFound, onProgress, isRunning);
#endif

    if (!anyFsScanned) {
        VolumeFsKind kind0 = probeVolumeAt(reader, 0, sectorSize);
        switch (kind0) {
            case VolumeFsKind::Ntfs: {
                NTFSParser ntfs;
                ntfs.scanAt(reader, callbackWrapper, isRunning, 0, 0);
                break;
            }
            case VolumeFsKind::ExFat:
            case VolumeFsKind::Fat: {
                FATParser fat;
                fat.scan(reader, callbackWrapper, isRunning);
                break;
            }
            case VolumeFsKind::Ext4: {
                Ext4Parser ext4;
                ext4.scan(reader, callbackWrapper, isRunning);
                break;
            }
            case VolumeFsKind::Apfs: {
                APFSParser apfs;
                apfs.scanAt(reader, callbackWrapper, isRunning, 0, 0);
                break;
            }
            case VolumeFsKind::Hfs: {
                HFSParser hfs;
                hfs.scanAt(reader, callbackWrapper, isRunning, 0, 0);
                break;
            }
            default: {
                NTFSParser ntfs;
                if (!ntfs.scan(reader, callbackWrapper, isRunning)) {
                    FATParser fat;
                    if (!fat.scan(reader, callbackWrapper, isRunning)) {
                        Ext4Parser ext4;
                        ext4.scan(reader, callbackWrapper, isRunning);
                    }
                }
                break;
            }
        }
    }
}

void runCarveScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = reader.getDiskSize() / sectorSize;

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (isRunning && !(*isRunning)) return;
        FileRecord out = fr;
        tagRaidScanSource(out, reader);
        if (!out.name.empty() || out.id != -1) onFileFound(out);
        onProgress(out.startSector, totalSectors);
        syncBadSectors(reader, badSectorOut);
    };
    CarvingEngine carver;
    if (!carver.loadSignatures("resources/signatures.json")) {
        carver.loadSignatures("signatures.json");
    }
    carver.scan(reader, callbackWrapper, isRunning);
}

void runDeepScan(DiskReader& reader,
                 FileSystemParser::FileRecordCallback onFileFound,
                 ProgressCallback onProgress,
                 std::atomic<bool>* isRunning,
                 std::vector<uint64_t>* badSectorOut) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = reader.getDiskSize() / sectorSize;

    auto quickProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        onProgress(current / 2, denom);
    };
    runQuickScan(reader, onFileFound, quickProgress, isRunning, badSectorOut);
    if (isRunning && !(*isRunning)) return;

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        onProgress(denom / 2 + current / 2, denom);
    };
    runCarveScan(reader, onFileFound, carveProgress, isRunning, badSectorOut);
    onProgress(totalSectors, totalSectors);
}

ScanCoordinator::ScanCoordinator() : isRunning(false) {}

ScanCoordinator::~ScanCoordinator() {
    stopScan();
}

void ScanCoordinator::startScan(const std::string& drivePath, const std::string& scanType,
                               FileSystemParser::FileRecordCallback onFileFound,
                               ProgressCallback onProgress,
                               std::vector<uint64_t>* badSectorOut,
                               std::shared_ptr<VirtualRaid> raid,
                               FinishedCallback onFinished) {
    if (isRunning) return;
    isRunning = true;

    scanThread = std::thread(&ScanCoordinator::scanWorker, this, drivePath, scanType,
                             onFileFound, onProgress, badSectorOut, std::move(raid),
                             std::move(onFinished));
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
                                std::shared_ptr<VirtualRaid> raid,
                                FinishedCallback onFinished) {
    DiskReader reader;
    if (raid) {
        reader.setRaidBackend(std::move(raid));
    } else if (drivePath == "raid") {
        if (onFinished) onFinished(3);
        isRunning = false;
        return;
    } else {
        int driveIndex = 0;
        try { driveIndex = std::stoi(drivePath); } catch (...) {
            if (onFinished) onFinished(3);
            isRunning = false;
            return;
        }
        if (!reader.openDrive(driveIndex)) {
            if (onFinished) onFinished(3);
            isRunning = false;
            return;
        }
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = reader.getDiskSize() / sectorSize;

    if (scanType == "quick") {
        runQuickScan(reader, onFileFound, onProgress, &isRunning, badSectorOut);
    } else if (scanType == "deep") {
        runDeepScan(reader, onFileFound, onProgress, &isRunning, badSectorOut);
    }

    syncBadSectors(reader, badSectorOut);
    onProgress(totalSectors, totalSectors);

    const int status = isRunning ? 1 : 2;
    if (onFinished) onFinished(status);
    isRunning = false;
}

} // namespace wolf
