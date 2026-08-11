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
};

class FATParser : public FileSystemParser {
public:
    FATParser();
    ~FATParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;
};

class Ext4Parser : public FileSystemParser {
public:
    Ext4Parser();
    ~Ext4Parser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;
};

class ExFATParser : public FileSystemParser {
public:
    ExFATParser();
    ~ExFATParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;
};

class HFSParser : public FileSystemParser {
public:
    HFSParser();
    ~HFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;
};

class APFSParser : public FileSystemParser {
public:
    APFSParser();
    ~APFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;
};

} // namespace wolf



