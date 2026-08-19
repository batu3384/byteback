#pragma once

#include "wolf_fs.h"
#include "wolf_io.h"
#include <atomic>
#include <cstdint>

namespace wolf {

// Walk an APFS container (NXSB) and enumerate embedded volume superblocks (APSB).
// ponytail: linear block scan for APSB magic; upgrade path: omap/volume tree walk.
bool walkApfsContainer(DiskReader& reader, uint64_t partitionOffsetBytes,
                       uint64_t partitionSizeBytes,
                       FileSystemParser::FileRecordCallback callback,
                       std::atomic<bool>* isRunning);

} // namespace wolf
