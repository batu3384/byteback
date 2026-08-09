#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <functional>
#include <vector>

namespace wolf {

class FileSystemParser {
public:
    virtual ~FileSystemParser() = default;

    // Callback invoked for every discovered file record.
    using FileRecordCallback = std::function<void(const FileRecord&)>;

    // Scan the provided disk and invoke the callback for each file found.
    virtual bool scan(DiskReader& reader, FileRecordCallback callback) = 0;
};

class NTFSParser : public FileSystemParser {
public:
    NTFSParser();
    ~NTFSParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback) override;
};

class FATParser : public FileSystemParser {
public:
    FATParser();
    ~FATParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback) override;
};

} // namespace wolf
