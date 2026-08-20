#pragma once

#include "byteback_db.h"
#include "byteback_fs.h"
#include "byteback_io.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace byteback {

struct VssSnapshotInfo {
    int index = 0;
    std::string devicePath;
    int64_t discoveredAt = 0;
    int64_t createdAt = 0; // volume object creation time (0 if unknown)
    uint64_t sizeBytes = 0;
};

// Enumerate Windows volume shadow copies (no-op on non-Windows builds).
std::vector<VssSnapshotInfo> enumerateVssSnapshots();

using VssScanProgressFn = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

// Scan one mounted VSS volume; file records use source=vss_ntfs / vss_fat.
void scanVssVolumeFilesystem(DiskReader& volumeReader, const VssSnapshotInfo& snap,
                             FileSystemParser::FileRecordCallback onFileFound,
                             VssScanProgressFn onProgress,
                             std::atomic<bool>* isRunning);

// Emit vss_snapshot markers and scan each enumerated shadow copy.
void scanVssSnapshots(FileSystemParser::FileRecordCallback onFileFound,
                      VssScanProgressFn onProgress,
                      std::atomic<bool>* isRunning);

// Bind snapshots whose boot serial matches the evidence disk (host mix-up closed).
void scanVssSnapshotsBound(DiskReader& evidence,
                           FileSystemParser::FileRecordCallback onFileFound,
                           VssScanProgressFn onProgress,
                           std::atomic<bool>* isRunning);

// Recover/content-search must open this path, not the evidence PhysicalDrive.
// Empty if the record is not a VSS file payload.
std::string vssDevicePathFromRecord(const FileRecord& rec);

} // namespace byteback
