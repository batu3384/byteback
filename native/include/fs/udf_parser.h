#pragma once

// Read-only UDF (ECMA-167 / OSTA). AVDP → VDS → FSD → File Entry / EFE.
// short_ad and long_ad extents. VAT (type 248) at last partition sector.
// Type 2 "*UDF Metadata Partition" maps FSD/ICB through the metadata file.
// Probe is separate (VolumeFsKind::Udf).

#include "byteback_fs.h"

namespace byteback {

uint16_t udfCrc16(const uint8_t* p, size_t n);

class UdfParser : public FileSystemParser {
public:
    bool scan(DiskReader& reader, FileRecordCallback callback,
              std::atomic<bool>* isRunning = nullptr) override;
    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes);
};

} // namespace byteback
