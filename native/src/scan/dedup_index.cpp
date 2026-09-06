#include "scan/dedup_index.h"
#include <algorithm>

namespace byteback {

void DedupIndex::clear() {
    entries_.clear();
    carveEntries_.clear();
    metaPrefixMaxEnd_.clear();
    carvePrefixMaxEnd_.clear();
    contentHashes_.clear();
    sorted_ = true;
    carveSorted_ = true;
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
    for (const auto& fr : records) {
        if (isCarveSource(fr.source)) {
            Entry e;
            e.startSector = fr.startSector;
            e.endSector = fr.endSector > 0 ? fr.endSector : fr.startSector;
            e.sizeBytes = fr.sizeBytes;
            e.confidence = fr.confidence;
            e.path = fr.path;
            e.name = fr.name;
            carveEntries_.push_back(std::move(e));
            carveSorted_ = false;
            // Rehydrate exact-content dedup too: without the persisted hashes a
            // re-carved identical payload at disjoint sectors is stored twice.
            if (!fr.contentHash.empty()) contentHashes_.emplace(fr.contentHash, fr.sizeBytes);
        } else {
            observe(fr);
        }
    }
    ensureSorted();
}

void DedupIndex::ensureSorted() {
    if (sorted_) return;
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.startSector < b.startSector; });
    metaPrefixMaxEnd_.resize(entries_.size());
    uint64_t runningMax = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        runningMax = std::max(runningMax, entries_[i].endSector);
        metaPrefixMaxEnd_[i] = runningMax;
    }
    sorted_ = true;
}

void DedupIndex::ensureCarveSorted() {
    if (carveSorted_) return;
    std::sort(carveEntries_.begin(), carveEntries_.end(),
              [](const Entry& a, const Entry& b) { return a.startSector < b.startSector; });
    carvePrefixMaxEnd_.resize(carveEntries_.size());
    uint64_t runningMax = 0;
    for (size_t i = 0; i < carveEntries_.size(); ++i) {
        runningMax = std::max(runningMax, carveEntries_[i].endSector);
        carvePrefixMaxEnd_[i] = runningMax;
    }
    carveSorted_ = true;
}

// First entry satisfying overlap*2 >= min(span) against [frStart, frEnd], or
// nullptr. `entries` must be sorted by startSector with a matching prefixMaxEnd.
const DedupIndex::Entry* DedupIndex::findOverlap(const std::vector<Entry>& entries,
                                                 const std::vector<uint64_t>& prefixMaxEnd,
                                                 uint64_t frStart, uint64_t frEnd, uint64_t frSpan) {
    if (entries.empty() || prefixMaxEnd.size() != entries.size()) return nullptr;

    // Candidates all live in [0, hi]: startSector <= frEnd.
    auto hiIt = std::upper_bound(entries.begin(), entries.end(), frEnd,
                                 [](uint64_t v, const Entry& e) { return v < e.startSector; });
    const int hi = static_cast<int>(hiIt - entries.begin()) - 1;
    if (hi < 0) return nullptr;
    if (prefixMaxEnd[hi] < frStart) return nullptr; // nothing reaches back to the query

    // Entries before j0 all have endSector < frStart (prefixMaxEnd is
    // monotone), so they cannot overlap; scan only [j0, hi].
    auto j0It = std::lower_bound(prefixMaxEnd.begin(), prefixMaxEnd.begin() + hi + 1, frStart);
    for (int idx = static_cast<int>(j0It - prefixMaxEnd.begin()); idx <= hi; ++idx) {
        const Entry& e = entries[idx];
        if (e.endSector < frStart) continue;
        if (!sectorsOverlap(e.startSector, e.endSector, frStart, frEnd)) continue;
        const uint64_t overlap = overlapSectorCount(e.startSector, e.endSector, frStart, frEnd);
        const uint64_t priorSpan = e.endSector >= e.startSector ? (e.endSector - e.startSector + 1) : 1;
        const uint64_t minSpan = std::max<uint64_t>(1, std::min(frSpan, priorSpan));
        if (overlap * 2 >= minSpan) return &e;
    }
    return nullptr;
}

bool DedupIndex::overlapsExistingCarve(const FileRecord& fr) {
    ensureCarveSorted();
    const uint64_t carveEnd = fr.endSector > 0 ? fr.endSector : fr.startSector;
    const uint64_t carveSpan = carveEnd >= fr.startSector ? (carveEnd - fr.startSector + 1) : 1;
    return findOverlap(carveEntries_, carvePrefixMaxEnd_, fr.startSector, carveEnd, carveSpan) != nullptr;
}

bool DedupIndex::markDuplicate(FileRecord& fr) {
    if (!isCarveSource(fr.source)) return false;
    ensureSorted();
    // P0-6: exact-content dedup beats sector-overlap heuristics — identical
    // payloads at different sectors are duplicates regardless of layout.
    // Hash AND size must match: the hash covers the first min(64KB, size)
    // bytes, so a prefix collision between two different-sized files (shared
    // EXIF headers, zero-padded DB pages) must not demote a distinct file.
    if (!fr.contentHash.empty()) {
        auto it = contentHashes_.find(fr.contentHash);
        if (it != contentHashes_.end() && it->second == fr.sizeBytes) {
            fr.source = "carver_duplicate";
            fr.path = "/dup_of/content";
            fr.confidence = std::min(fr.confidence, 30);
            return true;
        }
    }
    if (overlapsExistingCarve(fr)) {
        fr.source = "carver_duplicate";
        fr.path = "/dup_of/carve";
        fr.confidence = std::min(fr.confidence, 30);
        return true;
    }

    const uint64_t carveEnd = fr.endSector > 0 ? fr.endSector : fr.startSector;
    const uint64_t carveSpan = carveEnd >= fr.startSector ? (carveEnd - fr.startSector + 1) : 1;
    const Entry* meta = findOverlap(entries_, metaPrefixMaxEnd_, fr.startSector, carveEnd, carveSpan);
    if (meta) {
        // Metadata wins on substantial sector overlap. Carve confidence can be
        // inflated (header-only / weak validators) and must not keep a second
        // "file" that is the same payload as an MFT/FAT hit.
        fr.source = "carver_duplicate";
        fr.path = "/dup_of" + (meta->path.empty() ? meta->name : meta->path);
        fr.confidence = std::min(fr.confidence, 35);
        return true;
    }

    Entry tracked;
    tracked.startSector = fr.startSector;
    tracked.endSector = fr.endSector > 0 ? fr.endSector : fr.startSector;
    tracked.sizeBytes = fr.sizeBytes;
    tracked.confidence = fr.confidence;
    tracked.path = fr.path;
    tracked.name = fr.name;
    carveEntries_.push_back(std::move(tracked));
    carveSorted_ = false;
    if (!fr.contentHash.empty()) contentHashes_.emplace(fr.contentHash, fr.sizeBytes);
    return false;
}

} // namespace byteback
