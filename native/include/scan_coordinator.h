#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <climits>
#include "byteback_io.h"
#include "byteback_fs.h"
#include "byteback_carver.h"
#include "fs/unallocated_map.h" // SectorRange (runCarveScanRanges, A2)

namespace byteback {

class VirtualRaid;

using ScanProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

// When startSector != UINT64_MAX, scan is limited to that partition range.
struct ScanBounds {
    uint64_t startSector = UINT64_MAX;
    uint64_t sizeInSectors = 0;
    bool active() const { return startSector != UINT64_MAX && sizeInSectors > 0; }
};

struct ScanTarget {
    int64_t partitionStartSector = -1;
    uint64_t partitionSizeSectors = 0;
    // Overall scanned_sectors from DB at resume; floors progress so UI never rewinds.
    uint64_t resumeAtSector = 0;
    bool metadataComplete = false;
    uint64_t carveResumeSector = 0;
    // A2: opt-out for the parallel carve phase (workers over disjoint carve
    // ranges on clonable readers). Default ON; results are identical to the
    // sequential path as a multiset — only emission order may differ, and the
    // DB insert assigns ids by insertion order (documented, accepted).
    bool parallelCarve = true;
    ScanBounds bounds() const {
        ScanBounds b;
        if (partitionStartSector >= 0 && partitionSizeSectors > 0) {
            b.startSector = static_cast<uint64_t>(partitionStartSector);
            b.sizeInSectors = partitionSizeSectors;
        }
        return b;
    }
};

// A2 test/perf hook: worker count for the parallel carve phase. 0 = auto
// (clamp(hardware_concurrency, 2, 4)); 1 forces the sequential path.
void setParallelCarveWorkers(unsigned workers);
unsigned parallelCarveWorkers();

void runQuickScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ScanProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut = nullptr,
                  bool ntfsCarveOrphans = true,
                  ScanBounds bounds = {});

void runCarveScan(DiskReader& reader,
                  FileSystemParser::FileRecordCallback onFileFound,
                  ScanProgressCallback onProgress,
                  std::atomic<bool>* isRunning,
                  std::vector<uint64_t>* badSectorOut = nullptr,
                  ScanBounds bounds = {},
                  bool unallocatedOnly = true,
                  uint64_t resumeCarveSector = 0);

// A2: carve an explicit, caller-supplied range list. Ranges are processed as
// given — NOT merged (merging changes results at the seam: a file straddling
// two ranges is flushed at its range end by design). This is the seam the
// parallel carve phase (workers over disjoint ranges, clonable reader) hangs
// on, and the test entry point for determinism/cancel coverage.
void runCarveScanRanges(DiskReader& reader,
                        FileSystemParser::FileRecordCallback onFileFound,
                        ScanProgressCallback onProgress,
                        std::atomic<bool>* isRunning,
                        std::vector<uint64_t>* badSectorOut,
                        std::vector<SectorRange> ranges,
                        uint64_t resumeCarveSector = 0,
                        bool allowParallelCarve = true);

// Metadata quick scan followed by signature carving (professional "deep" mode).
using ScanCheckpointCallback = std::function<void(bool metadataComplete, uint64_t carveResumeSector)>;

void runDeepScan(DiskReader& reader,
                 FileSystemParser::FileRecordCallback onFileFound,
                 ScanProgressCallback onProgress,
                 std::atomic<bool>* isRunning,
                 std::vector<uint64_t>* badSectorOut = nullptr,
                 ScanBounds bounds = {},
                 ScanTarget target = {},
                 ScanCheckpointCallback onCheckpoint = nullptr);

// Metadata + full-partition signature carve (allocated + unallocated).
void runFullCarveScan(DiskReader& reader,
                      FileSystemParser::FileRecordCallback onFileFound,
                      ScanProgressCallback onProgress,
                      std::atomic<bool>* isRunning,
                      std::vector<uint64_t>* badSectorOut = nullptr,
                      ScanBounds bounds = {},
                      ScanTarget target = {},
                      ScanCheckpointCallback onCheckpoint = nullptr);

// Signature carve only — skips filesystem metadata (PhotoRec-style).
void runCarveOnlyScan(DiskReader& reader,
                      FileSystemParser::FileRecordCallback onFileFound,
                      ScanProgressCallback onProgress,
                      std::atomic<bool>* isRunning,
                      std::vector<uint64_t>* badSectorOut = nullptr,
                      ScanBounds bounds = {},
                      ScanTarget target = {},
                      ScanCheckpointCallback onCheckpoint = nullptr);

// Prefix file sources with raid_ when scanning through VirtualRaid.
void tagRaidScanSource(FileRecord& fr, const DiskReader& reader);

class ScanCoordinator {
public:
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;
    // status: 1=complete, 2=stopped, 3=failed
    using FinishedCallback = std::function<void(int status)>;
    
    ScanCoordinator();
    ~ScanCoordinator();

    // badSectorOut (optional): receives the reader's current bad-sector
    // sample list on every progress tick so the UI map can render it.
    void startScan(const std::string& drivePath, const std::string& scanType,
                   FileSystemParser::FileRecordCallback onFileFound,
                   ProgressCallback onProgress,
                   std::vector<uint64_t>* badSectorOut = nullptr,
                   std::shared_ptr<VirtualRaid> raid = nullptr,
                   FinishedCallback onFinished = nullptr,
                   const DiskReader* fvekSource = nullptr,
                   ScanTarget target = {},
                   ScanCheckpointCallback onCheckpoint = nullptr);
    
    void requestStop();
    void stopScan();

private:
    std::thread scanThread;
    std::atomic<bool> isRunning;

    void scanWorker(std::string drivePath, std::string scanType,
                    FileSystemParser::FileRecordCallback onFileFound,
                    ProgressCallback onProgress,
                    std::vector<uint64_t>* badSectorOut,
                    std::shared_ptr<VirtualRaid> raid,
                    FinishedCallback onFinished,
                    const DiskReader* fvekSource,
                    ScanTarget target,
                    ScanCheckpointCallback onCheckpoint);
};

} // namespace byteback
