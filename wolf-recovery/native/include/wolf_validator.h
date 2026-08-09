#pragma once

#include "wolf_fs.h"
#include <vector>
#include <string>

namespace wolf {

class FileValidator {
public:
    FileValidator();
    ~FileValidator();

    // Validates a file record by reading its content from the disk
    // Updates the record's confidence score and status
    void validateFile(FileRecord& record, DiskReader& reader);

private:
    int calculateConfidence(const FileRecord& record, const std::vector<uint8_t>& header, const std::vector<uint8_t>& footer);
};

} // namespace wolf
