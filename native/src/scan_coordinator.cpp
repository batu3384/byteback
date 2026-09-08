#include "scan_coordinator.h"
#include "scan_progress.h"
#include "fs/partition_scanner.h"
#include "fs/refs_parser.h"
#include "fs/unallocated_map.h"
#include "fs/virtual_raid.h"
#include "fs/vss_scanner.h"
#include "fs/bitlocker_fve.h"
#include "fs/xfs_parser.h"
#include "byteback_fs.h"
#include <iostream>
#include <exception>
#include <chrono>
#include <cstring>
#include <climits>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace byteback {

std::atomic<const char*> g_scanPhase{"metadata"};
std::atomic<uint64_t> g_phaseCurrent{0};
std::atomic<uint64_t> g_phaseTotal{0};

namespace {

using ProgressCallback = ScanCoordinator::ProgressCallback;

// A2: worker count for the parallel carve phase; 0 = auto.
std::atomic<unsigned> g_parallelCarveWorkers{0};

// A2: bounded hand-off between carve workers and the single consumer thread.
// Cap bounds memory when a worker refines candidates faster than the emit
// path (DB insert via the bridge callback) can consume them.
class BoundedRecordQueue {
public:
    explicit BoundedRecordQueue(size_t cap) : cap_(cap) {}

    enum class PopStatus { Got, Timeout, ClosedAndDrained };

    // Blocks while full. Returns false once the queue is closed (a producer
    // must then stop producing; the record is dropped).
    bool push(FileRecord fr) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cvNotFull_.wait(lock, [&] { return closed_ || q_.size() < cap_; });
            if (closed_) return false;
            q_.push_back(std::move(fr));
        }
        cvNotEmpty_.notify_one();
        return true;
    }

    // Blocks while empty (up to timeoutMs). ClosedAndDrained is terminal.
    PopStatus popFor(FileRecord& out, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cvNotEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [&] { return closed_ || !q_.empty(); })) {
            return PopStatus::Timeout;
        }
        if (q_.empty()) return PopStatus::ClosedAndDrained;
        out = std::move(q_.front());
        q_.pop_front();
        lock.unlock();
        cvNotFull_.notify_one();
        return PopStatus::Got;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        cvNotEmpty_.notify_all();
        cvNotFull_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.empty();
    }

private:
    mutable std::mutex mu_;
    mutable std::condition_variable cvNotEmpty_;
    mutable std::condition_variable cvNotFull_;
    std::deque<FileRecord> q_;
    size_t cap_;
    bool closed_ = false;
};

} // namespace

void setParallelCarveWorkers(unsigned workers) {
    g_parallelCarveWorkers.store(workers, std::memory_order_relaxed);
}

unsigned parallelCarveWorkers() {
    return g_parallelCarveWorkers.load(std::memory_order_relaxed);
}

namespace {

void syncBadSectors(DiskReader& reader, std::vector<uint64_t>* badSectorOut) {
    if (!badSectorOut) return;
    *badSectorOut = reader.getBadSectors();
}

// XfsParser emits (path, ino, size, isDir, runs) with runs as byte offsets
// relative to the partition — adapt to FileRecord like the other FS parsers.
// No progress ticks: the file records themselves drive emitProgress via
// callbackWrapper (XFS metadata walks are fast; a tick-per-N-files hook would
// need parser plumbing for little gain).
void emitXfsRecord(const std::string& path, uint64_t inodeNo, uint64_t sizeBytes, bool isDirectory,
                   const std::vector<std::pair<uint64_t, uint64_t>>& runs,
                   uint64_t partitionOffsetBytes, uint32_t sectorSize,
                   const FileSystemParser::FileRecordCallback& cb) {
    if (isDirectory) return;
    FileRecord fr;
    fr.id = static_cast<int64_t>(inodeNo & 0x7FFFFFFFFFFFFFFFLL);
    const size_t slash = path.find_last_of('/');
    fr.name = slash == std::string::npos ? path : path.substr(slash + 1);
    fr.path = path;
    const size_t dotPos = fr.name.find_last_of('.');
    fr.extension = (dotPos != std::string::npos && dotPos + 1 < fr.name.size())
                       ? fr.name.substr(dotPos + 1)
                       : "";
    fr.sizeBytes = sizeBytes;
    for (const auto& [byteOff, len] : runs) {
        FileRecord::DataRun r;
        r.startSector = (partitionOffsetBytes + byteOff) / sectorSize;
        r.sectorCount = (len + sectorSize - 1) / sectorSize;
        fr.runs.push_back(r);
    }
    if (!fr.runs.empty()) {
        fr.startSector = fr.runs.front().startSector;
        fr.endSector = fr.runs.back().startSector + fr.runs.back().sectorCount;
        fr.startByteOffset = (partitionOffsetBytes + runs.front().first) % sectorSize;
    }
    fr.status = 1; // live tree
    fr.confidence = 90;
    fr.category = "File";
    fr.source = "xfs_inode";
    cb(fr);
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
        g_phaseCurrent.store(mapped, std::memory_order_relaxed);
        g_phaseTotal.store(progressTotal, std::memory_order_relaxed);
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
            case VolumeFsKind::Xfs: {
                XfsParser xfs;
                if (xfs.open(reader, offsetBytes)) {
                    anyFsScanned = xfs.walkTree([&](const std::string& path, uint64_t inodeNo,
                                                    uint64_t sizeBytes, bool isDirectory,
                                                    const std::vector<std::pair<uint64_t, uint64_t>>& runs) {
                        emitXfsRecord(path, inodeNo, sizeBytes, isDirectory, runs, offsetBytes,
                                      sectorSize, callbackWrapper);
                    });
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
            case VolumeFsKind::Xfs: {
                XfsParser xfs;
                if (xfs.open(reader, 0)) {
                    xfs.walkTree([&](const std::string& path, uint64_t inodeNo,
                                     uint64_t sizeBytes, bool isDirectory,
                                     const std::vector<std::pair<uint64_t, uint64_t>>& runs) {
                        emitXfsRecord(path, inodeNo, sizeBytes, isDirectory, runs, 0, sectorSize,
                                      callbackWrapper);
                    });
                }
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

// CA-033: carve ranges computed once. runDeepScan builds the unallocated map
// for progress budgeting and runCarveScan rebuilt it for carving — on an 8TB
// FAT volume that meant the FAT walk (I/O-bound) ran twice per deep scan.
// The public runCarveScan keeps its header signature; runDeepScan hands the
// already-built ranges through the impl (by value/reference down the call —
// no shared mutable globals).
void runCarveScanRangesImpl(DiskReader& reader,
                            FileSystemParser::FileRecordCallback onFileFound,
                            ProgressCallback onProgress,
                            std::atomic<bool>* isRunning,
                            std::vector<uint64_t>* badSectorOut,
                            std::vector<SectorRange> carveRanges,
                            uint64_t resumeCarveSector,
                            bool allowParallelCarve);

void runCarveScanImpl(DiskReader& reader,
                      FileSystemParser::FileRecordCallback onFileFound,
                      ProgressCallback onProgress,
                      std::atomic<bool>* isRunning,
                      std::vector<uint64_t>* badSectorOut,
                      ScanBounds bounds,
                      bool unallocatedOnly,
                      uint64_t resumeCarveSector,
                      std::vector<SectorRange>* precomputedRanges,
                      bool allowParallelCarve) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    g_scanPhase.store("carve", std::memory_order_relaxed);
    std::vector<SectorRange> owned;
    std::vector<SectorRange>* ranges = precomputedRanges;
    // Precomputed ranges win whenever the caller supplies them (CA-033); an
    // empty/null pointer recomputes from the bounds with the requested mode.
    if (!ranges || ranges->empty()) {
        owned = prepareCarveRanges(reader, bounds, unallocatedOnly);
        ranges = &owned;
    }
    std::vector<SectorRange> carveRanges = *ranges; // copy: caller may still hold it
    if (carveRanges.empty()) {
        g_scanPhase.store("carve_skipped", std::memory_order_relaxed);
        return;
    }
    runCarveScanRangesImpl(reader, onFileFound, onProgress, isRunning, badSectorOut,
                           std::move(carveRanges), resumeCarveSector, allowParallelCarve);
}

// A2: carve a caller-supplied range list. Ranges are processed exactly as
// given (NOT merged — merging would change carve results at the seam: a file
// straddling two ranges is flushed at the range end by design). This is the
// shared core of runCarveScan and the seam the parallel phase hangs on.
void runCarveScanRangesImpl(DiskReader& reader,
                            FileSystemParser::FileRecordCallback onFileFound,
                            ProgressCallback onProgress,
                            std::atomic<bool>* isRunning,
                            std::vector<uint64_t>* badSectorOut,
                            std::vector<SectorRange> carveRanges,
                            uint64_t resumeCarveSector,
                            bool allowParallelCarve) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t totalCarveSectors = totalSectorCount(carveRanges);
    const uint64_t progressTotal = totalCarveSectors > 0 ? totalCarveSectors : 1;
    MonotonicMeter meter;
    g_phaseCurrent.store(0, std::memory_order_relaxed);
    g_phaseTotal.store(progressTotal, std::memory_order_relaxed);

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

    // ------------------------------------------------------------------
    // A2: parallel carve over disjoint ranges.
    //
    // Parallelizes ONLY over ranges — never band-splits inside a range.
    // CA-006 documented why banding corrupts results: a candidate opened near
    // a band edge cannot see its footer past the edge, so records truncate.
    // Whole ranges preserve sequential semantics exactly: every record a
    // range produces depends only on that range's content, so the parallel
    // result set equals the sequential one as a MULTISET (emission order may
    // differ; DB ids follow insertion order — documented, accepted).
    //
    // Physical drives and volume devices stay SEQUENTIAL even though clone()
    // would succeed: they are seek-bound, so overlapping workers thrash the
    // head and lose to one well-ordered stream (supportsParallelScan()).
    // The dedup/emit path stays single-threaded by construction: workers only
    // produce into the bounded queue; the one consumer runs the SAME
    // callbackWrapper as the sequential path (bridge-side markDuplicate etc.
    // unchanged).
    // ------------------------------------------------------------------
    struct CarveWork { uint64_t start; uint64_t end; };
    bool parallelViable = allowParallelCarve && reader.supportsParallelScan() &&
                          reader.clone() != nullptr;
    unsigned workerCount = 0;
    if (parallelViable) {
        unsigned n = g_parallelCarveWorkers.load(std::memory_order_relaxed);
        if (n == 0) {
            n = std::thread::hardware_concurrency();
            n = n == 0 ? 2u : std::min(n, 4u);
            n = std::max(n, 2u);
        }
        workerCount = n;
        if (workerCount < 2) parallelViable = false;
    }

    // Resume skip is applied identically in both paths (same accounting, same
    // progress shape) so DB checkpoints stay meaningful across the toggle.
    // `cursor` mirrors the sequential loop's carvedSectors (resume math);
    // `beforeWork` is ONLY the progress base — skipped sectors — so the
    // parallel phase reports beforeWork + scanned without double counting.
    uint64_t beforeWork = 0;    // skipped sectors (progress base)
    uint64_t cursor = 0;        // cumulative sector cursor for resume math
    uint64_t parallelTotal = 0; // sectors in the work list
    std::vector<CarveWork> work;
    bool cancelledDuringSkip = false;
    if (parallelViable) {
        for (const auto& rg : carveRanges) {
            if (isRunning && !(*isRunning)) { cancelledDuringSkip = true; break; }
            if (rg.count == 0) continue;
            uint64_t rangeStart = rg.start;
            uint64_t rangeEnd = rg.start + rg.count;

            if (resumeCarveSector > 0 && cursor + rg.count <= resumeCarveSector) {
                cursor += rg.count;
                beforeWork += rg.count;
                onProgress(meter.tick(beforeWork), progressTotal);
                continue;
            }
            if (resumeCarveSector > cursor) {
                const uint64_t skip = resumeCarveSector - cursor;
                rangeStart += skip;
                cursor += skip;
                beforeWork += skip;
                if (rangeStart >= rangeEnd) {
                    cursor += rg.count - skip;
                    onProgress(meter.tick(beforeWork), progressTotal);
                    continue;
                }
            }
            work.push_back({rangeStart, rangeEnd});
            parallelTotal += rangeEnd - rangeStart;
            cursor += rangeEnd - rangeStart;
        }
        // Parallel needs at least 2 non-trivial ranges and enough work to
        // amortize the thread/clone startup (4 MiB floor).
        if (work.size() < 2 || parallelTotal * sectorSize < 4ull * 1024 * 1024) {
            parallelViable = false;
        }
    }

    if (parallelViable && !cancelledDuringSkip) {
        // Largest ranges first so workers finish close together (ranges are
        // pulled dynamically; this is only a scheduling hint).
        std::stable_sort(work.begin(), work.end(),
                         [](const CarveWork& a, const CarveWork& b) {
                             return (a.end - a.start) > (b.end - b.start);
                         });

        workerCount = std::min(workerCount, static_cast<unsigned>(work.size()));
        std::vector<std::unique_ptr<DiskReader>> clones;
        clones.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) {
            clones.push_back(reader.clone());
            if (!clones.back()) { // cannot happen behind supportsParallelScan(); be honest anyway
                parallelViable = false;
                break;
            }
        }

        if (parallelViable) {
            BoundedRecordQueue queue(10000);
            std::atomic<uint64_t> scannedSectors{0};
            // One shared BGC budget for all workers: same total as the
            // sequential path, and scanRangeSingle's atomic path is race-free.
            std::atomic<int> sharedBgcBudget(32);
            // Set when the emit consumer dies (callback exception): workers
            // must stop at the next range boundary instead of scanning on
            // into a queue nobody drains.
            std::atomic<bool> consumerGone{false};

            auto emitParallelProgress = [&]() {
                const uint64_t s = std::min<uint64_t>(scannedSectors.load(std::memory_order_relaxed),
                                                      parallelTotal);
                onProgress(meter.tick(beforeWork + s), progressTotal);
            };

            std::atomic<size_t> nextRange{0};
            std::atomic<bool> workerFailed{false};
            // Producers-done signal: the consumer runs on THIS thread, so the
            // workers must announce termination through a counter — joining
            // them before close() would deadlock against the drain loop.
            // RAII decrement: workers exit via `return` deep inside the try
            // block, which would skip a plain trailing fetch_sub.
            struct LiveWorkersGuard {
                std::atomic<int>& counter;
                ~LiveWorkersGuard() { counter.fetch_sub(1, std::memory_order_release); }
            };
            std::atomic<int> liveWorkers(static_cast<int>(clones.size()));
            std::vector<std::thread> workers;
            workers.reserve(clones.size());
            for (size_t w = 0; w < clones.size(); ++w) {
                workers.emplace_back([&, w]() {
                    LiveWorkersGuard guard{liveWorkers};
                    try {
                        for (;;) {
                            if (consumerGone.load(std::memory_order_relaxed)) return;
                            if (isRunning && !(*isRunning)) return;
                            const size_t i = nextRange.fetch_add(1, std::memory_order_relaxed);
                            if (i >= work.size()) return;
                            const uint64_t wStart = work[i].start;
                            const uint64_t wEnd = work[i].end;
                            auto sink = [&](const FileRecord& fr) {
                                if (fr.id == -1 && fr.name.empty()) {
                                    // Progress tick: chunk-level sector progress.
                                    const uint64_t done = std::min(fr.startSector, wEnd);
                                    const uint64_t rel = done > wStart ? done - wStart : 0;
                                    scannedSectors.fetch_add(rel, std::memory_order_relaxed);
                                    return;
                                }
                                // Blocks while full; returns false once the
                                // queue is closed (consumer died) — drop, the
                                // scan outcome is already lost.
                                if (!queue.push(fr)) return;
                            };
                            carver.scanRangeSingle(*clones[w], wStart, wEnd, sink, isRunning,
                                                   0, 0, &sharedBgcBudget);
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[byteback] parallel carve worker exception: "
                                  << e.what() << std::endl;
                        workerFailed = true;
                    } catch (...) {
                        std::cerr << "[byteback] parallel carve worker unknown exception" << std::endl;
                        workerFailed = true;
                    }
                });
            }

            // The consumer runs on THIS thread, so the worker threads and the
            // queue must be shut down HERE in every path. If the emit callback
            // throws, unwinding through `workers` while its threads are still
            // joinable would call std::terminate and take down the process —
            // the sequential path propagates the same exception cleanly to
            // ScanCoordinator::scanWorker, and so must this one.
            try {
                // Single consumer: the ONLY thread running the emit path, so
                // downstream dedup/DB insertion order logic stays
                // single-threaded exactly as in the sequential path.
                for (;;) {
                    FileRecord fr;
                    auto st = queue.popFor(fr, 200);
                    if (st == BoundedRecordQueue::PopStatus::Got) {
                        callbackWrapper(fr);
                        continue;
                    }
                    emitParallelProgress();
                    if (liveWorkers.load(std::memory_order_acquire) == 0 && queue.empty()) break;
                }
                for (auto& t : workers) t.join();
                // A2/B: clone reads recorded bad sectors on the CLONES
                // (telemetry is per-reader); merge them or the parallel phase
                // loses every sector its workers hit.
                for (const auto& c : clones) reader.mergeBadSectorTelemetryFrom(*c);
                queue.close();
                // Drain whatever is still queued, then a final progress tick.
                for (;;) {
                    FileRecord fr;
                    if (queue.popFor(fr, 0) != BoundedRecordQueue::PopStatus::Got) break;
                    callbackWrapper(fr);
                }
                if (workerFailed) {
                    // Ceiling: a worker that died mid-range leaves that range
                    // partially scanned (matches a cancelled scan's honesty).
                    std::cerr << "[byteback] parallel carve: a worker failed; "
                                 "results may be a subset" << std::endl;
                }
                emitParallelProgress();
                onProgress(meter.tick(progressTotal), progressTotal);
                return;
            } catch (...) {
                // Unblock the workers (a full queue would deadlock the join
                // below), stop them at the next range boundary, join, and let
                // the exception continue exactly as the sequential path does.
                consumerGone.store(true, std::memory_order_relaxed);
                queue.close();
                for (auto& t : workers) {
                    if (t.joinable()) t.join();
                }
                throw;
            }
        }
        // Cloning failed after all — fall through to the sequential loop
        // (the resume-skip progress above replays as monotonic no-ops).
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

void runCarveScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut,
                  ScanBounds bounds,
                  bool unallocatedOnly,
                  uint64_t resumeCarveSector) {
    runCarveScanImpl(reader, onFileFound, onProgress, isRunning, badSectorOut, bounds,
                     unallocatedOnly, resumeCarveSector, nullptr, /*allowParallelCarve=*/true);
}

void runCarveScanRanges(DiskReader& reader,
                        FileSystemParser::FileRecordCallback onFileFound,
                        ProgressCallback onProgress,
                        std::atomic<bool>* isRunning,
                        std::vector<uint64_t>* badSectorOut,
                        std::vector<SectorRange> ranges,
                        uint64_t resumeCarveSector,
                        bool allowParallelCarve) {
    g_scanPhase.store("carve", std::memory_order_relaxed);
    if (ranges.empty()) {
        g_scanPhase.store("carve_skipped", std::memory_order_relaxed);
        return;
    }
    runCarveScanRangesImpl(reader, onFileFound, onProgress, isRunning, badSectorOut,
                           std::move(ranges), resumeCarveSector, allowParallelCarve);
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

    // CA-033: build the unallocated map once and hand it to the carve phase —
    // it used to be built here for the budget and rebuilt inside runCarveScan.
    std::vector<SectorRange> carveRanges = prepareCarveRanges(reader, bounds, true);
    const uint64_t carveTotal = totalSectorCount(carveRanges);
    const bool unallocCarve = carveTotal > 0;
    const uint64_t carveBudget =
        unallocCarve ? std::min(carveTotal, totalSectors) : totalSectors / 4;
    // Metadata share of the overall bar. Kept even when metadataComplete so
    // resume mid-carve maps onto [metaShare, total] instead of rewinding to 0.
    const uint64_t metaShare =
        totalSectors > carveBudget ? totalSectors - carveBudget : totalSectors / 2;
    const uint64_t metaBudget = target.metadataComplete ? 0 : metaShare;
    const uint64_t carveProgressBase = target.metadataComplete ? metaShare : metaBudget;

    MonotonicMeter overall;
    if (target.resumeAtSector > 0) overall.tick(std::min(totalSectors, target.resumeAtSector));
    auto emit = [&](uint64_t current) {
        onProgress(overall.tick(std::min(totalSectors, current)), totalSectors);
    };

    if (!target.metadataComplete) {
        auto quickProgress = [&](uint64_t current, uint64_t total) {
            uint64_t denom = total > 0 ? total : 1;
            g_phaseCurrent.store(current, std::memory_order_relaxed);
            g_phaseTotal.store(denom, std::memory_order_relaxed);
            emit(mulDivU64(current, metaBudget, denom));
        };
        runQuickScan(reader, onFileFound, quickProgress, isRunning, badSectorOut, true, bounds);
        if (isRunning && !(*isRunning)) return;
        if (onCheckpoint) onCheckpoint(true, 0);
        emit(metaBudget);
    }

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        g_phaseCurrent.store(current, std::memory_order_relaxed);
        g_phaseTotal.store(denom, std::memory_order_relaxed);
        uint64_t slice = carveBudget > 0 ? mulDivU64(current, carveBudget, denom) : current;
        emit(carveProgressBase + slice);
        if (onCheckpoint) onCheckpoint(true, current);
    };
    runCarveScanImpl(reader, onFileFound, carveProgress, isRunning, badSectorOut, bounds,
                     unallocCarve, target.carveResumeSector,
                     unallocCarve ? &carveRanges : nullptr, target.parallelCarve);
    if (unallocCarve) {
        emit(totalSectors);
    } else {
        emit(carveProgressBase);
    }
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

    // A2: compute the full carve ranges once and hand them down so
    // target.parallelCarve governs the carve phase here too.
    std::vector<SectorRange> fullCarveRanges = prepareCarveRanges(reader, bounds, false);
    const uint64_t fullCarveTotal = totalSectorCount(fullCarveRanges);
    const uint64_t carveBudget = fullCarveTotal > 0 ? std::min(fullCarveTotal, totalSectors) : totalSectors / 2;
    const uint64_t metaShare =
        totalSectors > carveBudget ? totalSectors - carveBudget : totalSectors / 2;
    const uint64_t metaBudget = target.metadataComplete ? 0 : metaShare;
    const uint64_t carveProgressBase = target.metadataComplete ? metaShare : metaBudget;

    MonotonicMeter overall;
    if (target.resumeAtSector > 0) overall.tick(std::min(totalSectors, target.resumeAtSector));
    auto emit = [&](uint64_t current) {
        onProgress(overall.tick(std::min(totalSectors, current)), totalSectors);
    };

    if (!target.metadataComplete) {
        auto quickProgress = [&](uint64_t current, uint64_t total) {
            uint64_t denom = total > 0 ? total : 1;
            g_phaseCurrent.store(current, std::memory_order_relaxed);
            g_phaseTotal.store(denom, std::memory_order_relaxed);
            emit(mulDivU64(current, metaBudget, denom));
        };
        runQuickScan(reader, onFileFound, quickProgress, isRunning, badSectorOut, true, bounds);
        if (isRunning && !(*isRunning)) return;
        if (onCheckpoint) onCheckpoint(true, 0);
        emit(metaBudget);
    }

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        g_phaseCurrent.store(current, std::memory_order_relaxed);
        g_phaseTotal.store(denom, std::memory_order_relaxed);
        uint64_t slice = carveBudget > 0 ? mulDivU64(current, carveBudget, denom) : current;
        emit(carveProgressBase + slice);
        if (onCheckpoint) onCheckpoint(true, current);
    };
    runCarveScanImpl(reader, onFileFound, carveProgress, isRunning, badSectorOut, bounds,
                     false, target.carveResumeSector, &fullCarveRanges, target.parallelCarve);
    if (fullCarveTotal > 0) {
        emit(totalSectors);
    } else {
        emit(carveProgressBase);
    }
}
void runCarveOnlyScan(DiskReader& reader,
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

    g_scanPhase.store("carve_only", std::memory_order_relaxed);
    MonotonicMeter overall;
    if (target.resumeAtSector > 0) overall.tick(std::min(totalSectors, target.resumeAtSector));

    auto carveProgress = [&](uint64_t current, uint64_t total) {
        uint64_t denom = total > 0 ? total : 1;
        const uint64_t capped = std::min(current, denom);
        onProgress(overall.tick(mulDivU64(capped, totalSectors, denom)), totalSectors);
        if (onCheckpoint) onCheckpoint(true, current);
    };
    // A2: explicit range list so target.parallelCarve applies here as well.
    std::vector<SectorRange> carveOnlyRanges = prepareCarveRanges(reader, bounds, false);
    runCarveScanRanges(reader, onFileFound, carveProgress, isRunning, badSectorOut,
                       std::move(carveOnlyRanges), target.carveResumeSector,
                       target.parallelCarve);
    onProgress(overall.tick(totalSectors), totalSectors);
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
        // Phase-local progress is process-global; an errored or cancelled
        // previous scan must not leak "carve @ 736k/1M" into this scan's
        // first progress event. Reset before any phase runs.
        g_scanPhase.store("metadata", std::memory_order_relaxed);
        g_phaseCurrent.store(0, std::memory_order_relaxed);
        g_phaseTotal.store(0, std::memory_order_relaxed);
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
            } else if (scanType == "carve_only") {
                runCarveOnlyScan(reader, onFileFound, onProgress, &isRunning, badSectorOut, bounds, target, onCheckpoint);
                status = isRunning ? 1 : 2;
            } else {
                status = 3;
            }

            if (status == 1) {
                syncBadSectors(reader, badSectorOut);
                onProgress(totalSectors, totalSectors);
            } else if (status != 3) {
                syncBadSectors(reader, badSectorOut);
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
