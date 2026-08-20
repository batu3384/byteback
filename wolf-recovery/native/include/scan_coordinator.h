#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <climits>
#include "wolf_io.h"
#include "wolf_fs.h"
#include "wolf_carver.h"

namespace wolf {

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
    uint64_t resumeAtSector = 0; // ponytail: whole-disk resume; partition scope not persisted yet
    ScanBounds bounds() const {
        ScanBounds b;
        if (partitionStartSector >= 0 && partitionSizeSectors > 0) {
            b.startSector = static_cast<uint64_t>(partitionStartSector);
            b.sizeInSectors = partitionSizeSectors;
        }
        return b;
    }
};

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

// Metadata quick scan followed by signature carving (professional "deep" mode).
void runDeepScan(DiskReader& reader,
                 FileSystemParser::FileRecordCallback onFileFound,
                 ScanProgressCallback onProgress,
                 std::atomic<bool>* isRunning,
                 std::vector<uint64_t>* badSectorOut = nullptr,
                 ScanBounds bounds = {},
                 uint64_t resumeAt = 0);

// Metadata + full-partition signature carve (allocated + unallocated).
void runFullCarveScan(DiskReader& reader,
                      FileSystemParser::FileRecordCallback onFileFound,
                      ScanProgressCallback onProgress,
                      std::atomic<bool>* isRunning,
                      std::vector<uint64_t>* badSectorOut = nullptr,
                      ScanBounds bounds = {},
                      uint64_t resumeAt = 0);

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
                   ScanTarget target = {});
    
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
                    ScanTarget target);
};

} // namespace wolf
