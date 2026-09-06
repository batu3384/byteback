#pragma once

#include "byteback_db.h"
#include <cstdint>
#include <string>
#include <unordered_map>
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

    // AR2-perf: scans emit records sequentially. Sorting the whole vector on
    // EVERY record (observe sets sorted_=false; markDuplicate then ensures)
    // is O(n² log n) — a 500K-file NTFS scan spent minutes inside std::sort
    // and froze the machine. New records land in a small pending tail that is
    // checked linearly; it is merged into the sorted base only when it grows
    // past kMergeThreshold, giving amortized near-linear behavior.
    void mergeMetaPending();
    void mergeCarvePending();
    static void mergeSorted(std::vector<Entry>& base, std::vector<uint64_t>& prefixMaxEnd,
                            std::vector<Entry> pending);

    static constexpr size_t kMergeThreshold = 1024;

    // CA-009: the old query lower_bound'ed a startSector-sorted vector with an
    // endSector comparator. The range is not partitioned for that comparator,
    // so the binary search could land past a long-span entry and never see it.
    // prefixMaxEnd[i] (running max of endSector) is monotone, which bounds the
    // candidate window with two valid binary searches.
    static const Entry* findOverlap(const std::vector<Entry>& entries,
                                    const std::vector<uint64_t>& prefixMaxEnd,
                                    uint64_t frStart, uint64_t frEnd, uint64_t frSpan,
                                    const std::vector<Entry>& pending);

    mutable bool sorted_ = true;
    mutable bool carveSorted_ = true;
    std::vector<Entry> entries_;
    std::vector<Entry> carveEntries_;
    std::vector<uint64_t> metaPrefixMaxEnd_;
    std::vector<uint64_t> carvePrefixMaxEnd_;
    std::vector<Entry> metaPending_;
    std::vector<Entry> carvePending_;
    // P0-6: content hash -> size of accepted carve records, for exact-payload
    // dedup. Size must match too: the hash covers first min(64KB, size) bytes,
    // so equal prefixes of different-sized files are NOT duplicates.
    std::unordered_map<std::string, uint64_t> contentHashes_;
};

} // namespace byteback
