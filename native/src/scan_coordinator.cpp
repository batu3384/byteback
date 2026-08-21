#include "scan_coordinator.h"
#include "scan_progress.h"
#include "fs/partition_scanner.h"
#include "fs/refs_parser.h"
#include "fs/unallocated_map.h"
#include "fs/virtual_raid.h"
#include "fs/vss_scanner.h"
#include "fs/bitlocker_fve.h"
#include "byteback_fs.h"
#include <iostream>
#include <exception>
#include <chrono>
#include <cstring>
#include <climits>
#include <algorithm>
#include <atomic>

namespace byteback {

std::atomic<const char*> g_scanPhase{"metadata"};

namespace {

using ProgressCallback = ScanCoordinator::ProgressCallback;

void syncBadSectors(DiskReader& reader, std::vector<uint64_t>* badSectorOut) {
    if (!badSectorOut) return;
    *badSectorOut = reader.getBadSectors();
}

std::vector<SectorRange> prepareCarveRanges(DiskReader& reader, ScanBounds bounds, bool unallocatedOnly) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t diskSectors = reader.getDiskSize() / sectorSize;

    uint64_t boundFirst = 0;
    uint64_t boundLast = diskSectors;
    if (bounds.active()) {
        boundFirst = bounds.startSector;
        boundLast = std::min(diskSectors, bounds.startSector + bounds.sizeInSectors);
    }

    std::vector<SectorRange> carveRanges;
    if (unallocatedOnly) {
        int64_t pStart = bounds.active() ? static_cast<int64_t>(bounds.startSector) : -1;
        uint64_t pSize = bounds.active() ? bounds.sizeInSectors : 0;
        carveRanges = collectUnallocatedForScan(reader, pStart, pSize);
    }
    if (carveRanges.empty()) {
        if (unallocatedOnly) return {};
        carveRanges.push_back({boundFirst, boundLast > boundFirst ? boundLast - boundFirst : 0});
    }
    return carveRanges;
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
                  bool ntfsCarveOrphans,
                  ScanBounds bounds) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t diskSectors = reader.getDiskSize() / sectorSize;
    uint64_t totalSectors = diskSectors;
    if (bounds.active()) totalSectors = bounds.sizeInSectors;

    g_scanPhase.store("metadata", std::memory_order_relaxed);
    MonotonicMeter meter;
    uint64_t workUnits = 0;
    const uint64_t progressTotal = totalSectors > 0 ? totalSectors : 1;

    auto emitProgress = [&](uint64_t done, uint64_t estimated) {
        uint64_t mapped = mapWorkToBudget(done, estimated, progressTotal);
        if (done > 0 && mapped == 0) mapped = 1;
        onProgress(meter.tick(mapped), progressTotal);
    };

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (isRunning && !(*isRunning)) return;
        FileRecord out = fr;
        tagRaidScanSource(out, reader);
        const bool progressOnly = out.id == -1 && out.name.empty();
        if (progressOnly) {
            if (out.sizeBytes > 0) {
                emitProgress(out.startSector, out.sizeBytes);
            } else {
                ++workUnits;
                emitProgress(workUnits, std::max<uint64_t>(workUnits, 64));
            }
        } else {
            if (!out.name.empty() || out.id != -1) onFileFound(out);
            ++workUnits;
            emitProgress(workUnits, std::max<uint64_t>(workUnits, 64));
        }
        syncBadSectors(reader, badSectorOut);
    };

    bool bitlockerVolume = false;
    if (!bounds.active()) {
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

    auto scanPartition = [&](const PartitionInfo& part) {
        if (isRunning && !(*isRunning)) return;
        if (part.sizeInSectors == 0) return;

        uint64_t offsetBytes = part.startSector * sectorSize;
        uint64_t partSizeBytes = part.sizeInSectors * sectorSize;

        {
            std::vector<uint8_t> boot(sectorSize);
            if (reader.readSectors(offsetBytes, sectorSize, boot.data()).success &&
                looksLikeBitLocker(boot.data(), boot.size())) {
                emitBitLockerFromBoot(reader, boot.data(), boot.size(), offsetBytes, part.startSector, onFileFound);
                return;
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
            case VolumeFsKind::Refs: {
                RefsParser refs;
                if (refs.scanAt(reader, callbackWrapper, isRunning, offsetBytes, partSizeBytes)) {
                    anyFsScanned = true;
                }
                break;
            }
            default:
                break;
        }
    };

    if (bounds.active()) {
        bool matched = false;
        for (const auto& part : partitions) {
            if (part.startSector != bounds.startSector) continue;
            matched = true;
            PartitionInfo scoped = part;
            if (bounds.sizeInSectors < scoped.sizeInSectors) {
                scoped.sizeInSectors = bounds.sizeInSectors;
            }
            scanPartition(scoped);
            break;
        }
        if (!matched) {
            PartitionInfo synthetic;
            synthetic.startSector = bounds.startSector;
            synthetic.sizeInSectors = bounds.sizeInSectors;
            scanPartition(synthetic);
        }
    } else {
        for (const auto& part : partitions) {
            scanPartition(part);
        }
    }

#ifdef _WIN32
    if (!bounds.active()) {
        scanVssSnapshotsBound(reader, callbackWrapper,
            [&](uint64_t current, uint64_t) {
                onProgress(meter.tick(std::min(current, progressTotal)), progressTotal);
            },
            isRunning);
    }
#endif

    if (!anyFsScanned && !bitlockerVolume && !bounds.active()) {
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
            case VolumeFsKind::Refs: {
                RefsParser refs;
                refs.scanAt(reader, callbackWrapper, isRunning, 0, 0);
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
                  std::vector<uint64_t>* badSectorOut,
                  ScanBounds bounds,
                  bool unallocatedOnly,
                  uint64_t resumeCarveSector) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    g_scanPhase.store("carve", std::memory_order_relaxed);
    std::vector<SectorRange> carveRanges = prepareCarveRanges(reader, bounds, unallocatedOnly);
    if (carveRanges.empty()) return;

    uint64_t totalCarveSectors = totalSectorCount(carveRanges);
    const uint64_t progressTotal = totalCarveSectors > 0 ? totalCarveSectors : 1;
    MonotonicMeter meter;

    auto callbackWrapper = [&](const FileRecord& fr) {
        if (isRunning && !(*isRunning)) return;
        FileRecord out = fr;
        tagRaidScanSource(out, reader);
        if (!out.name.empty() || out.id != -1) onFileFound(out);
        syncBadSectors(reader, badSectorOut);
    };

    CarvingEngine carver;
    if (!carver.loadSignatures("")) {
        std::cerr << "Failed to load carving signatures" << std::endl;
    }

    uint64_t carvedSectors = 0;
    for (const auto& rg : carveRanges) {
        if (isRunning && !(*isRunning)) break;
        if (rg.count == 0) continue;

        uint64_t rangeStart = rg.start;
        uint64_t rangeEnd = rg.start + rg.count;

        if (resumeCarveSector > 0 && carvedSectors + rg.count <= resumeCarveSector) {
            carvedSectors += rg.count;
            onProgress(meter.tick(carvedSectors), progressTotal);
            continue;
        }
        if (resumeCarveSector > carvedSectors) {
            uint64_t skip = resumeCarveSector - carvedSectors;
            rangeStart += skip;
            if (rangeStart >= rangeEnd) {
                carvedSectors += rg.count;
                onProgress(meter.tick(carvedSectors), progressTotal);
                continue;
            }
        }

        carver.scanRange(reader, rangeStart, rangeEnd,
                         [&](const FileRecord& fr) {
                             callbackWrapper(fr);
                             if (fr.id == -1 && fr.startSector > 0) {
                                 uint64_t rel = fr.startSector >= rg.start ? fr.startSector - rg.start : 0;
                                 onProgress(meter.tick(carvedSectors + rel), progressTotal);
                             }
                         },
                         isRunning);
        carvedSectors += rg.count;
        onProgress(meter.tick(carvedSectors), progressTotal);
    }
}

void runDeepScan(DiskReader& reader,
                 FileSystemParser::FileRecordCallback onFileFound,
                 ProgressCallback onProgress,
                 std::atomic<bool>* isRunning,
                 std::vector<uint64_t>* badSectorOut,
                 ScanBounds bounds,
                 ScanTarget target,
                 ScanCheckpointCallback onCheckpoint) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = bounds.active() ? bounds.sizeInSectors : reader.getDiskSize() / sectorSize;

    const uint64_t carveTotal = totalSectorCount(prepareCarveRanges(reader, bounds, true));
    const bool unallocCarve = carveTotal > 0;
    const uint64_t carveBudget =
        unallocCarve ? std::min(carveTotal, totalSectors) : totalSectors / 4;
    const uint64_t metaBudget =
        target.metadataComplete ? 0 : (totalSectors > carveBudget ? totalSectors - carveBudget : totalSectors / 2);

    if (!target.metadataComplete) {
        auto quickProgress = [&](uint64_t current, uint64_t total) {
            uint64_t denom = total > 0 ? total : 1;
            onProgress(mulDivU64(current, metaBudget, denom), totalSectors);
        };
        runQuickScan(reader, onFileFound, quickProgress, isRunning, badSectorOut, true, bounds);
        if (isRunning && !(*isRunning)) return;
        if (onCheckpoint) onCheckpoint(true, 0);
        onProgress(metaBudget, totalSectors);
    }

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        uint64_t slice = carveBudget > 0 ? mulDivU64(current, carveBudget, denom) : current;
        onProgress(std::min(totalSectors, metaBudget + slice), totalSectors);
        if (onCheckpoint) onCheckpoint(true, current);
    };
    runCarveScan(reader, onFileFound, carveProgress, isRunning, badSectorOut, bounds, unallocCarve,
                 target.carveResumeSector);
    onProgress(totalSectors, totalSectors);
}

void runFullCarveScan(DiskReader& reader,
                      FileSystemParser::FileRecordCallback onFileFound,
                      ProgressCallback onProgress,
                      std::atomic<bool>* isRunning,
                      std::vector<uint64_t>* badSectorOut,
                      ScanBounds bounds,
                      ScanTarget target,
                      ScanCheckpointCallback onCheckpoint) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t totalSectors = bounds.active() ? bounds.sizeInSectors : reader.getDiskSize() / sectorSize;

    const uint64_t fullCarveTotal =
        totalSectorCount(prepareCarveRanges(reader, bounds, false));
    const uint64_t carveBudget = fullCarveTotal > 0 ? std::min(fullCarveTotal, totalSectors) : totalSectors / 2;
    const uint64_t metaBudget =
        target.metadataComplete ? 0 : (totalSectors > carveBudget ? totalSectors - carveBudget : totalSectors / 2);

    if (!target.metadataComplete) {
        auto quickProgress = [&](uint64_t current, uint64_t total) {
            uint64_t denom = total > 0 ? total : 1;
            onProgress(mulDivU64(current, metaBudget, denom), totalSectors);
        };
        runQuickScan(reader, onFileFound, quickProgress, isRunning, badSectorOut, true, bounds);
        if (isRunning && !(*isRunning)) return;
        if (onCheckpoint) onCheckpoint(true, 0);
        onProgress(metaBudget, totalSectors);
    }

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        uint64_t slice = carveBudget > 0 ? mulDivU64(current, carveBudget, denom) : current;
        onProgress(std::min(totalSectors, metaBudget + slice), totalSectors);
        if (onCheckpoint) onCheckpoint(true, current);
    };
    runCarveScan(reader, onFileFound, carveProgress, isRunning, badSectorOut, bounds, false,
                 target.carveResumeSector);
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
                               FinishedCallback onFinished,
                               const DiskReader* fvekSource,
                               ScanTarget target,
                               ScanCheckpointCallback onCheckpoint) {
    stopScan();
    isRunning = true;

    scanThread = std::thread(&ScanCoordinator::scanWorker, this, drivePath, scanType,
                             onFileFound, onProgress, badSectorOut, std::move(raid),
                             std::move(onFinished), fvekSource, std::move(target),
                             std::move(onCheckpoint));
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
                                FinishedCallback onFinished,
                                const DiskReader* fvekSource,
                                ScanTarget target,
                                ScanCheckpointCallback onCheckpoint) {
    int status = 3;
    try {
        DiskReader reader;
        bool opened = false;
        if (raid) {
            reader.setRaidBackend(std::move(raid));
            opened = true;
        } else if (drivePath == "raid") {
            opened = false;
        } else {
            int driveIndex = 0;
            bool parsed = true;
            try {
                driveIndex = std::stoi(drivePath);
            } catch (...) {
                parsed = false;
            }
            opened = parsed && reader.openDrive(driveIndex);
        }

        if (opened) {
            if (fvekSource) reader.copyXtsFvekFrom(*fvekSource);

            ScanBounds bounds = target.bounds();
            uint32_t sectorSize = reader.getSectorSize();
            if (sectorSize == 0) sectorSize = 512;
            uint64_t totalSectors = bounds.active() ? bounds.sizeInSectors : reader.getDiskSize() / sectorSize;

            if (scanType == "quick") {
                runQuickScan(reader, onFileFound, onProgress, &isRunning, badSectorOut, false, bounds);
                status = isRunning ? 1 : 2;
            } else if (scanType == "deep") {
                runDeepScan(reader, onFileFound, onProgress, &isRunning, badSectorOut, bounds, target, onCheckpoint);
                status = isRunning ? 1 : 2;
            } else if (scanType == "full_carve") {
                runFullCarveScan(reader, onFileFound, onProgress, &isRunning, badSectorOut, bounds, target, onCheckpoint);
                status = isRunning ? 1 : 2;
            } else {
                status = 3;
            }

            if (status != 3) {
                syncBadSectors(reader, badSectorOut);
                onProgress(totalSectors, totalSectors);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[byteback] scanWorker exception: " << e.what() << std::endl;
        status = 3;
    } catch (...) {
        std::cerr << "[byteback] scanWorker unknown exception" << std::endl;
        status = 3;
    }
    try {
        if (onFinished) onFinished(status);
    } catch (const std::exception& e) {
        std::cerr << "[byteback] scan onFinished exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[byteback] scan onFinished unknown exception" << std::endl;
    }
    isRunning = false;
}

} // namespace byteback
