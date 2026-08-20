#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <functional>
#include <vector>
#include <atomic>

namespace wolf {

class FileSystemParser {
public:
    virtual ~FileSystemParser() = default;

    // Callback invoked for every discovered file record.
    using FileRecordCallback = std::function<void(const FileRecord&)>;

    // Scan the provided disk and invoke the callback for each file found.
    // isRunning allows cooperative cancellation from the main thread.
    virtual bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) = 0;
};

class NTFSParser : public FileSystemParser {
public:
    NTFSParser();
    ~NTFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes = 0,
                bool carveOrphanMft = true);
};

class FATParser : public FileSystemParser {
public:
    FATParser();
    ~FATParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    // Partition-aware variant: partitionOffsetBytes is the byte offset of the
    // partition start (0 = whole disk / raw image). Pass the partition's
    // start sector * sectorSize when the MBR/GPT layout is known so the boot
    // sector and cluster geometry resolve at the right location.
    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes);

private:
    void parseFAT(DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback, std::atomic<bool>* isRunning);
    void parseExFAT(DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback, std::atomic<bool>* isRunning);
};

class Ext4Parser : public FileSystemParser {
public:
    Ext4Parser();
    ~Ext4Parser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes);
};

class HFSParser : public FileSystemParser {
public:
    HFSParser();
    ~HFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes = 0);
};

class APFSParser : public FileSystemParser {
public:
    APFSParser();
    ~APFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes = 0);
};

} // namespace wolf



