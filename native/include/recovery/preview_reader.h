#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

constexpr size_t kPreviewMaxBytes = 64 * 1024;
constexpr size_t kVideoHeadMaxBytes = 512 * 1024;
constexpr size_t kFfmpegProbeMaxBytes = 4 * 1024 * 1024;

struct FilePreviewResult {
    bool success = false;
    std::string error;
    std::string kind; // image, text, pdf, binary
    std::string mime; // e.g. image/jpeg; empty when unknown
    /** Honest structural hint when no renderable frame (e.g. H.264 IDR sniff). */
    std::string note;
    std::vector<uint8_t> data;
};

bool readRecordPrefix(DiskReader& reader, const FileRecord& record, std::vector<uint8_t>& out,
                      size_t maxBytes = kPreviewMaxBytes);

FilePreviewResult readFilePreview(DiskReader& reader, const FileRecord& record);

} // namespace byteback
