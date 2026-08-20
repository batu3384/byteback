#pragma once

#include "wolf_fs.h"
#include "wolf_io.h"
#include <atomic>
#include <cstdint>

namespace wolf {

// Walk an APFS container: NXSB (block size + nx_fs_oid[100]), APSB volumes,
// and btree-leaf dir records (source=apfs_file). Full block count, cancellable.
bool walkApfsContainer(DiskReader& reader, uint64_t partitionOffsetBytes,
                       uint64_t partitionSizeBytes,
                       FileSystemParser::FileRecordCallback callback,
                       std::atomic<bool>* isRunning);

} // namespace wolf
