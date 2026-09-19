#pragma once

#include "byteback_db.h"
#include <string>

namespace byteback {

// Serialize NTFS/FAT/ext4/ISO data runs for SQLite (JSON array of [start,count] or [start,count,byteCount]).
std::string serializeRuns(const std::vector<FileRecord::DataRun>& runs);

// Parse runs_json; malformed input yields an empty vector (never throws).
std::vector<FileRecord::DataRun> deserializeRuns(const std::string& json);

} // namespace byteback
