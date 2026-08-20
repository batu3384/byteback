#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wolf {

constexpr size_t kPreviewMaxBytes = 64 * 1024;

struct FilePreviewResult {
    bool success = false;
    std::string error;
    std::string kind; // image, text, pdf, binary
    std::vector<uint8_t> data;
};

bool readRecordPrefix(DiskReader& reader, const FileRecord& record, std::vector<uint8_t>& out,
                      size_t maxBytes = kPreviewMaxBytes);

FilePreviewResult readFilePreview(DiskReader& reader, const FileRecord& record);

} // namespace wolf
