#pragma once

#include "wolf_fs.h"
#include <cstdint>

namespace wolf {

// Scan NTFS $LogFile (MFT #2) for UTF-16 filename hints left in the journal.
// Emits low-confidence FileRecord hints (source=ntfs_logfile).
void scanNtfsLogFileHints(DiskReader& reader, uint64_t partitionOffsetBytes,
                          FileSystemParser::FileRecordCallback callback,
                          std::atomic<bool>* isRunning);

} // namespace wolf
