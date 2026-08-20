#include "scan/dedup_index.h"
#include <algorithm>

namespace byteback {

void DedupIndex::clear() {
    entries_.clear();
    sorted_ = true;
}

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
    sorted_ = false;
}

void DedupIndex::loadFromRecords(const std::vector<FileRecord>& records) {
    for (const auto& fr : records) observe(fr);
    ensureSorted();
}

void DedupIndex::ensureSorted() {
    if (sorted_) return;
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.startSector < b.startSector; });
    sorted_ = true;
}

bool DedupIndex::markDuplicate(FileRecord& fr) {
    if (!isCarveSource(fr.source)) return false;
    ensureSorted();

    uint64_t carveEnd = fr.endSector > 0 ? fr.endSector : fr.startSector;
    uint64_t carveSpan = carveEnd >= fr.startSector ? (carveEnd - fr.startSector + 1) : 1;

    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), fr.startSector,
        [](const Entry& e, uint64_t sector) { return e.endSector < sector; });

    for (; it != entries_.end(); ++it) {
        if (it->startSector > carveEnd) break;
        if (!sectorsOverlap(it->startSector, it->endSector, fr.startSector, carveEnd)) continue;
        uint64_t overlap = overlapSectorCount(it->startSector, it->endSector, fr.startSector, carveEnd);
        uint64_t metaSpan = it->endSector >= it->startSector ? (it->endSector - it->startSector + 1) : 1;
        const uint64_t minSpan = std::max<uint64_t>(1, std::min(carveSpan, metaSpan));
        if (overlap * 2 < minSpan) continue;
        if (it->confidence + 5 < fr.confidence) continue;

        fr.source = "carver_duplicate";
        fr.path = "/dup_of" + (it->path.empty() ? it->name : it->path);
        fr.confidence = std::min(fr.confidence, 35);
        return true;
    }
    return false;
}

} // namespace byteback
