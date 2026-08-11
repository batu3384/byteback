#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct sqlite3;

namespace wolf {

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
    int status;            // 0=Intact, 1=PartialOverwrite, 2=FullOverwrite, 3=Unknown
    int confidence;        // 0-100
    std::string category;  // "Image", "Document", "Video", etc.
    std::string source;    // "mft", "fat", "carve", "fragment"
    int64_t createdAt;
    int64_t modifiedAt;
};

struct ScanState {
    int64_t id;
    int driveIndex;
    std::string scanType;   // "quick", "deep", "fragment"
    uint64_t totalSectors;
    uint64_t scannedSectors;
    int status;             // 0=Running, 1=Paused, 2=Complete, 3=Failed
    int64_t startedAt;
    int64_t updatedAt;
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
    std::vector<FileRecord> getFiles(int64_t scanId, int offset, int limit);
    int64_t getFileCount(int64_t scanId);

    // Scan state
    int64_t createScan(int driveIndex, const std::string& scanType, uint64_t totalSectors);
    bool updateScanProgress(int64_t scanId, uint64_t scannedSectors);
    bool completeScan(int64_t scanId, int status);
    ScanState getScanState(int64_t scanId);

private:
    bool createTables();
    sqlite3* db_;
};

} // namespace wolf


