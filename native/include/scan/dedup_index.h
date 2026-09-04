#pragma once

#include "byteback_db.h"
#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

class DedupIndex {
public:
    void clear();
    void observe(const FileRecord& fr);
    void loadFromRecords(const std::vector<FileRecord>& records);
    void ensureSorted();
    bool markDuplicate(FileRecord& fr);

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
    void ensureCarveSorted();
    bool overlapsExistingCarve(const FileRecord& fr);

    // CA-009: the old query lower_bound'ed a startSector-sorted vector with an
    // endSector comparator. The range is not partitioned for that comparator,
    // so the binary search could land past a long-span entry and never see it.
    // prefixMaxEnd[i] (running max of endSector) is monotone, which bounds the
    // candidate window with two valid binary searches.
    static const Entry* findOverlap(const std::vector<Entry>& entries,
                                    const std::vector<uint64_t>& prefixMaxEnd,
                                    uint64_t frStart, uint64_t frEnd, uint64_t frSpan);

    mutable bool sorted_ = true;
    mutable bool carveSorted_ = true;
    std::vector<Entry> entries_;
    std::vector<Entry> carveEntries_;
    std::vector<uint64_t> metaPrefixMaxEnd_;
    std::vector<uint64_t> carvePrefixMaxEnd_;
};

} // namespace byteback
