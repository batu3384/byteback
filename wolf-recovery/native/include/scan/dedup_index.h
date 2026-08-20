#pragma once

#include "wolf_db.h"
#include <cstdint>
#include <string>
#include <vector>

namespace wolf {

class DedupIndex {
public:
    void clear();
    void observe(const FileRecord& fr);
    bool markDuplicate(FileRecord& fr) const;

private:
    struct Entry {
        uint64_t startSector = 0;
        uint64_t endSector = 0;
        uint64_t sizeBytes = 0;
        int confidence = 0;
        std::string path;
        std::string name;
    };

    static bool isMetadataSource(const std::string& source);
    static bool isCarveSource(const std::string& source);
    static bool sectorsOverlap(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd);
    static uint64_t overlapSectorCount(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd);

    std::vector<Entry> entries_;
};

} // namespace wolf
