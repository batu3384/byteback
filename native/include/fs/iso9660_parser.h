#pragma once

// Read-only ISO 9660 (ECMA-119) primary volume. Superfloppy and partition
// offset. Joliet SVD (%/@ %/C %/E UCS-2) preferred when present. Rock Ridge
// NM on the ISO tree when Joliet yok. El Torito default boot entry emitted as BOOT.IMG.

#include "byteback_fs.h"

namespace byteback {

class Iso9660Parser : public FileSystemParser {
public:
    bool scan(DiskReader& reader, FileRecordCallback callback,
              std::atomic<bool>* isRunning = nullptr) override;
    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes);
};

} // namespace byteback
