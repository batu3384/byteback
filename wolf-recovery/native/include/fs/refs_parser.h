#pragma once

#include "wolf_fs.h"
#include "wolf_io.h"
#include <atomic>
#include <cstdint>

namespace wolf {

class RefsParser : public FileSystemParser {
public:
    RefsParser();
    ~RefsParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes = 0);
};

// Boot VBR + SUPB at cluster 30. Directory listing is a ministore B+-tree walk
// when checkpoint refs resolve; otherwise a metadata-page scan for entry records.
// Integrity: optional CRC64-ECMA trailer on entry records; SUPB self-check at +48.
bool probeRefsBoot(const uint8_t* boot, size_t bootLen, uint32_t& bytesPerSector,
                   uint32_t& sectorsPerCluster);

} // namespace wolf
