#include "scan/dedup_index.h"
#include <algorithm>

namespace wolf {

void DedupIndex::clear() { entries_.clear(); }

bool DedupIndex::isMetadataSource(const std::string& source) {
    if (source.empty()) return false;
    if (isCarveSource(source) || source == "carver_duplicate") return false;
    if (source == "usn_journal" || source == "ntfs_logfile" || source == "ntfs_logfile_restart") {
        return false;
    }
    return true;
}

bool DedupIndex::isCarveSource(const std::string& source) {
    return source == "carver" || source == "carver_bgc";
}

bool DedupIndex::sectorsOverlap(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd) {
    if (aEnd < aStart) aEnd = aStart;
    if (bEnd < bStart) bEnd = bStart;
    return aStart <= bEnd && bStart <= aEnd;
}

uint64_t DedupIndex::overlapSectorCount(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd) {
    if (!sectorsOverlap(aStart, aEnd, bStart, bEnd)) return 0;
    uint64_t start = std::max(aStart, bStart);
    uint64_t end = std::min(aEnd, bEnd);
    return end >= start ? (end - start + 1) : 0;
}

void DedupIndex::observe(const FileRecord& fr) {
    if (!isMetadataSource(fr.source)) return;
    if (fr.startSector == 0 && fr.endSector == 0 && fr.sizeBytes == 0) return;
    Entry e;
    e.startSector = fr.startSector;
    e.endSector = fr.endSector > 0 ? fr.endSector : fr.startSector;
    e.sizeBytes = fr.sizeBytes;
    e.confidence = fr.confidence;
    e.path = fr.path;
    e.name = fr.name;
    entries_.push_back(std::move(e));
}

bool DedupIndex::markDuplicate(FileRecord& fr) const {
    if (!isCarveSource(fr.source)) return false;
    uint64_t carveEnd = fr.endSector > 0 ? fr.endSector : fr.startSector;
    uint64_t carveSpan = carveEnd >= fr.startSector ? (carveEnd - fr.startSector + 1) : 1;

    for (const auto& e : entries_) {
        if (!sectorsOverlap(e.startSector, e.endSector, fr.startSector, carveEnd)) continue;
        uint64_t overlap = overlapSectorCount(e.startSector, e.endSector, fr.startSector, carveEnd);
        uint64_t metaSpan = e.endSector >= e.startSector ? (e.endSector - e.startSector + 1) : 1;
        const uint64_t minSpan = std::max<uint64_t>(1, std::min(carveSpan, metaSpan));
        if (overlap * 2 < minSpan) continue;
        if (e.confidence + 5 < fr.confidence) continue;

        fr.source = "carver_duplicate";
        fr.path = "/dup_of" + (e.path.empty() ? e.name : e.path);
        fr.confidence = std::min(fr.confidence, 35);
        return true;
    }
    return false;
}

} // namespace wolf
