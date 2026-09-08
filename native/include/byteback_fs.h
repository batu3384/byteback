#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include <functional>
#include <vector>
#include <atomic>

namespace byteback {

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

private:
    // FAZ 1.1: compact per-record index built by the Pass-1 MFT walk. Pass-1
    // retains ONLY these entries — the previous design accumulated a full
    // FileRecord (strings, runs, resident blob) per record and scaled with the
    // record count into the gigabyte range. Pass-2 re-reads each record's bytes
    // from disk (byteOffset/byteLen), re-parses it with the shared attribute
    // parser and streams it to the callback, so at most one record is
    // materialized at a time.
    struct MftEntry {
        uint64_t byteOffset = 0;  // absolute byte offset of the record start
        uint32_t byteLen = 0;     // record length used while parsing in Pass-1
        uint64_t mftRef = 0;      // MFT record number; UINT64_MAX when unknown
        uint8_t flags = 0;        // MFT record flags snapshot (0x01 in-use, 0x02 dir)
        // runs_codec serializeRuns() of the unnamed non-resident $DATA
        // attribute. Resident $DATA bytes are NOT retained — Pass-2 re-reads
        // them from the on-disk record (K1: nothing is dropped; the orphan
        // sweep emits any entry Pass-2 could not re-read).
        std::vector<uint8_t> packedRuns;
    };
    // Map key tag for entries whose MFT record number is unknown (records
    // found outside the $MFT runs). Real refs are 48-bit, so the tag bit
    // cannot collide.
    static constexpr uint64_t kOrphanEntryKeyBit = 0x8000000000000000ULL;
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

} // namespace byteback



