#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

struct sqlite3;

namespace byteback {

struct FileRecord {
    int64_t id;
    int64_t parentId;
    std::string name;
    std::string extension;
    std::string path;
    uint64_t sizeBytes;
    uint64_t startSector;
    uint64_t endSector;

    struct DataRun {
        uint64_t startSector;
        uint64_t sectorCount;
    };
    std::vector<DataRun> runs;
    std::vector<uint8_t> residentData; // NTFS resident $DATA bytes (no runs)
    bool compressed = false; // NTFS: $DATA has a compression unit (LZNT1)
    uint64_t integrityChecksum = 0; // ReFS integrity stream CRC64-ECMA; 0 = not checked
    int status;            // 0=deleted/unallocated, 1=in-use/allocated, 2=encrypted/other
    int confidence;        // 0-100
    std::string category;  // "Image", "Document", "Video", etc.
    std::string source;    // "mft", "fat", "carve", "fragment"
    int64_t createdAt;
    int64_t modifiedAt;
};

// One point on the unified timeline: a file event observed in the USN
// journal (or, later, other artifact sources) with its decoded reason.
struct TimelineEvent {
    int64_t id;
    int64_t scanId;
    int64_t timestamp;      // Unix seconds
    std::string eventType;  // "create", "delete", "rename_old", "rename_new", ...
    std::string fileName;
    uint64_t mftRef;
    std::string source;     // "usn_journal", "mft", ...
};

struct ScanState {
    int64_t id;
    int driveIndex;
    std::string scanType;   // "quick", "deep", "full_carve", "carve_only"
    uint64_t totalSectors;
    uint64_t scannedSectors;
    int status;             // 0=Running, 1=Complete, 2=Stopped, 3=Failed, 4=Paused(resumable)
    int64_t startedAt;
    int64_t updatedAt;
    int64_t recoveredFiles = 0;
    int64_t partitionStartSector = -1;
    uint64_t partitionSizeSectors = 0;
    bool metadataComplete = false;
    uint64_t carveResumeSector = 0;
};

// Singleton forensic case metadata (E01 header + audit context).
struct CaseInfo {
    std::string caseNumber;
    std::string investigator;
    std::string agency;
    std::string notes;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

struct FileListFilter {
    int status = -1;          // -1 all, 0 deleted, 1 allocated
    std::string category;     // empty = all (Image, Document, ...)
    std::string query;        // empty = no name/path search
    std::string sourceLike;   // empty = all; e.g. "carver%"
    std::string sourceNotLike; // empty = all; e.g. "carver%" excludes carve from deleted view
    bool includeDuplicates = true;
    bool includeDiscovery = false;
};

class MetadataStore {
public:
    MetadataStore();
    ~MetadataStore();

    bool open(const std::string& dbPath);
    void close();
    bool isOpen() const;

    // File records
    int64_t insertFile(int64_t scanId, const FileRecord& record);
    bool insertFilesBatch(int64_t scanId, const std::vector<FileRecord>& records);
    std::vector<FileRecord> getFiles(int64_t scanId, int offset, int limit,
                                     const FileListFilter& filter = {});
    int64_t getFileCount(int64_t scanId, const FileListFilter& filter = {});

    // Scan state
    int64_t createScan(int driveIndex, const std::string& scanType, uint64_t totalSectors);
    bool setScanTotalSectors(int64_t scanId, uint64_t totalSectors);
    bool updateScanProgress(int64_t scanId, uint64_t scannedSectors);
    bool setScanPartition(int64_t scanId, int64_t partitionStartSector, uint64_t partitionSizeSectors);
    bool updateScanCheckpoint(int64_t scanId, bool metadataComplete, uint64_t carveResumeSector);
    bool setScanRunning(int64_t scanId);
    bool completeScan(int64_t scanId, int status);
    // Previous process died with status=Running. Mark those rows paused so UI can resume.
    int64_t reclaimOrphanRunningScans();
    ScanState getScanState(int64_t scanId);

    struct ScanSummary {
        int64_t totalFiles = 0;
        int64_t deletedFiles = 0;
        int64_t imageFiles = 0;
        int64_t documentFiles = 0;
        int64_t videoFiles = 0;
        int64_t audioFiles = 0;
        int64_t archiveFiles = 0;
        int64_t carvedFiles = 0;
        int64_t timelineEvents = 0;
        int64_t usnCreates = 0;
        int64_t usnDeletes = 0;
        int64_t usnRenames = 0;
    };
    ScanSummary getScanSummary(int64_t scanId);
    std::vector<FileRecord> searchFiles(int64_t scanId, const std::string& query,
                                        int offset, int limit, bool useRegex,
                                        const std::string& categoryFilter = "",
                                        int statusFilter = -1);
    std::vector<FileRecord> searchFiles(int64_t scanId, const std::string& query,
                                        int offset, int limit, bool useRegex,
                                        const FileListFilter& filter);
    int64_t searchFilesCount(int64_t scanId, const std::string& query, bool useRegex,
                             const std::string& categoryFilter = "",
                             int statusFilter = -1);
    int64_t searchFilesCount(int64_t scanId, const std::string& query, bool useRegex,
                             const FileListFilter& filter);

    // Content FTS — full-file windows (chunked), indexed during content search.
    bool upsertContentSample(int64_t scanId, int64_t fileId, const std::string& text);
    bool replaceContentChunks(int64_t fileId, const std::vector<std::string>& chunks);
    std::string getContentSample(int64_t fileId);
    int64_t getContentIndexCount(int64_t scanId);
    bool isContentIndexComplete(int64_t scanId);
    std::vector<int64_t> searchContentFts(int64_t scanId, const std::string& query,
                                          int offset, int limit);
    FileRecord getFileById(int64_t fileId, int64_t scanId = -1);

    // CA-008: real session + recovery bookkeeping.
    int64_t getLatestScanId();
    /** Latest scan with status complete (1) or paused (4); -1 if none. */
    int64_t getLatestUsableScanId();
    bool clearAllScanData();
    bool incrementRecovered(int64_t scanId);

    // Unified timeline (USN journal and other event sources)
    int64_t insertTimelineEvent(int64_t scanId, const TimelineEvent& event);
    std::vector<TimelineEvent> getTimelineEvents(int64_t scanId, int offset, int limit,
                                                 const std::string& eventTypeFilter = "");
    int64_t getTimelineEventCount(int64_t scanId, const std::string& eventTypeFilter = "");

    CaseInfo getCaseInfo();
    bool setCaseInfo(const CaseInfo& info);

private:
    bool createTables();
    sqlite3* db_;
    mutable std::recursive_mutex mu_;
};

} // namespace byteback


