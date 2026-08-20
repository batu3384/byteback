#include "scan_coordinator.h"
#include "fs/partition_scanner.h"
#include "fs/virtual_raid.h"
#include "fs/vss_scanner.h"
#include "fs/bitlocker_fve.h"
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
    if (fr.source == "vss_snapshot" || fr.source == "vss_unbound" || fr.source == "vss_bind" ||
        fr.source == "bitlocker_detect" || fr.source == "bitlocker_fve") return;
    if (fr.source.rfind("raid_", 0) == 0) return;
    fr.source = "raid_" + fr.source;
}

bool looksLikeBitLocker(const uint8_t* boot, size_t n) {
    // OEM name at offset 3 is "-FVE-FS-" (8 bytes). Older code matched a
    // 10-byte fake "-FVEF-SYS-" that never appears on real BitLocker volumes.
    return n >= 11 && std::memcmp(boot + 3, "-FVE-FS-", 8) == 0;
}

void emitBitLockerFromBoot(DiskReader& reader, const uint8_t* boot, size_t bootLen,
                           uint64_t volumeOffsetBytes, uint64_t startSector,
                           const FileSystemParser::FileRecordCallback& onFileFound) {
    BitLockerFveInfo info;
    parseBitLockerFve(boot, bootLen, nullptr, 0, info);
    if (info.metadataOffset > 0) {
        std::vector<uint8_t> meta(256, 0);
        uint64_t metaOff = volumeOffsetBytes + info.metadataOffset;
        if (reader.readSectors(metaOff, static_cast<uint32_t>(meta.size()), meta.data()).success) {
            parseBitLockerFve(boot, bootLen, meta.data(), meta.size(), info);
        }
    }
    emitBitLockerRecord(info, startSector, onFileFound);
}

void runQuickScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut,
                  bool ntfsCarveOrphans) {
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

    bool bitlockerVolume = false;
    {
        std::vector<uint8_t> boot(sectorSize);
        if (reader.readSectors(0, sectorSize, boot.data()).success &&
            looksLikeBitLocker(boot.data(), boot.size())) {
            emitBitLockerFromBoot(reader, boot.data(), boot.size(), 0, 0, onFileFound);
            bitlockerVolume = true;
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

        {
            std::vector<uint8_t> boot(sectorSize);
            if (reader.readSectors(offsetBytes, sectorSize, boot.data()).success &&
                looksLikeBitLocker(boot.data(), boot.size())) {
                emitBitLockerFromBoot(reader, boot.data(), boot.size(), offsetBytes, part.startSector, onFileFound);
                continue;
            }
        }

        VolumeFsKind kind = probeVolumeAt(reader, offsetBytes, sectorSize);

        switch (kind) {
            case VolumeFsKind::Ntfs: {
                NTFSParser ntfs;
                if (ntfs.scanAt(reader, callbackWrapper, isRunning, offsetBytes, partSizeBytes,
                                ntfsCarveOrphans)) {
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
    scanVssSnapshotsBound(reader, callbackWrapper, onProgress, isRunning);
#endif

    if (!anyFsScanned && !bitlockerVolume) {
        VolumeFsKind kind0 = probeVolumeAt(reader, 0, sectorSize);
        switch (kind0) {
            case VolumeFsKind::Ntfs: {
                NTFSParser ntfs;
                ntfs.scanAt(reader, callbackWrapper, isRunning, 0, 0, ntfsCarveOrphans);
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

namespace {
void joinNotSelf(std::thread& t) {
    if (!t.joinable()) return;
    if (t.get_id() == std::this_thread::get_id()) {
        t.detach();
        return;
    }
    t.join();
}
}

void ScanCoordinator::requestStop() {
    isRunning = false;
}

void ScanCoordinator::startScan(const std::string& drivePath, const std::string& scanType,
                               FileSystemParser::FileRecordCallback onFileFound,
                               ProgressCallback onProgress,
                               std::vector<uint64_t>* badSectorOut,
                               std::shared_ptr<VirtualRaid> raid,
                               FinishedCallback onFinished) {
    stopScan();
    isRunning = true;

    scanThread = std::thread(&ScanCoordinator::scanWorker, this, drivePath, scanType,
                             onFileFound, onProgress, badSectorOut, std::move(raid),
                             std::move(onFinished));
}

void ScanCoordinator::stopScan() {
    requestStop();
    joinNotSelf(scanThread);
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
        runQuickScan(reader, onFileFound, onProgress, &isRunning, badSectorOut, false);
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
