#include "scan/dedup_index.h"
#include <algorithm>
#include <unordered_map>

namespace byteback {

void DedupIndex::clear() {
    entries_.clear();
    carveEntries_.clear();
    metaPending_.clear();
    carvePending_.clear();
    metaPrefixMaxEnd_.clear();
    carvePrefixMaxEnd_.clear();
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

// AR2-perf: O(1) append. Sorting happens only when the pending tail is merged
// (threshold) or when a query explicitly requires the merged view.
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
    metaPending_.push_back(std::move(e));
    if (metaPending_.size() >= kMergeThreshold) mergeMetaPending();
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
    mergeMetaPending();
    mergeCarvePending();
}

// Merge the (sorted) pending tail into the sorted base and rebuild the
// prefix-max-end array.
void DedupIndex::mergeSorted(std::vector<Entry>& base, std::vector<uint64_t>& prefixMaxEnd,
                             std::vector<Entry> pending) {
    if (pending.empty()) return;
    std::sort(pending.begin(), pending.end(),
              [](const Entry& a, const Entry& b) { return a.startSector < b.startSector; });
    if (base.empty()) {
        base = std::move(pending);
    } else {
        // Both sorted: append pending then inplace_merge keeps O(n) moves.
        const size_t mid = base.size();
        base.insert(base.end(), pending.begin(), pending.end());
        std::inplace_merge(base.begin(), base.begin() + static_cast<long>(mid), base.end(),
                           [](const Entry& a, const Entry& b) { return a.startSector < b.startSector; });
    }
    prefixMaxEnd.resize(base.size());
    uint64_t runningMax = 0;
    for (size_t i = 0; i < base.size(); ++i) {
        runningMax = std::max(runningMax, base[i].endSector);
        prefixMaxEnd[i] = runningMax;
    }
}

void DedupIndex::mergeMetaPending() {
    if (metaPending_.empty()) { sorted_ = true; return; }
    mergeSorted(entries_, metaPrefixMaxEnd_, std::move(metaPending_));
    metaPending_.clear();
    sorted_ = true;
}

void DedupIndex::mergeCarvePending() {
    if (carvePending_.empty()) { carveSorted_ = true; return; }
    mergeSorted(carveEntries_, carvePrefixMaxEnd_, std::move(carvePending_));
    carvePending_.clear();
    carveSorted_ = true;
}

void DedupIndex::ensureSorted() {
    if (!sorted_) mergeMetaPending();
}

void DedupIndex::ensureCarveSorted() {
    if (!carveSorted_) mergeCarvePending();
}

// First entry satisfying overlap*2 >= min(span) against [frStart, frEnd], or
// nullptr. Checks BOTH the merged base (binary-searched window) and the
// unmerged pending tail (linear) so a query never misses a recent record.
// AR2-perf: on real disks the [j0..hi] window can hold hundreds of thousands
// of entries per carve candidate. The scan runs BACKWARD from hi (nearest the
// carve end — the likeliest real duplicate) capped at kMaxWindowScan entries,
// plus a small forward probe from j0 to keep long-span straddlers covered.
// ponytail: a proper interval tree would remove the cap entirely; duplicates
// beyond it are recoverable via the 'Tekrarlar' toggle, never lost.
namespace {
constexpr int kMaxWindowScan = 65536;
constexpr int kEdgeProbe = 1024;
}

const DedupIndex::Entry* DedupIndex::findOverlap(const std::vector<Entry>& entries,
                                                 const std::vector<uint64_t>& prefixMaxEnd,
                                                 uint64_t frStart, uint64_t frEnd, uint64_t frSpan,
                                                 const std::vector<Entry>& pending) {
    // Pending tail first: linear, small by construction. Must run even when
    // the merged base is empty — an observe() record lives here until the
    // threshold merge runs.
    for (const auto& e : pending) {
        if (e.endSector < frStart) continue;
        if (!sectorsOverlap(e.startSector, e.endSector, frStart, frEnd)) continue;
        const uint64_t overlap = overlapSectorCount(e.startSector, e.endSector, frStart, frEnd);
        const uint64_t priorSpan = e.endSector >= e.startSector ? (e.endSector - e.startSector + 1) : 1;
        const uint64_t minSpan = std::max<uint64_t>(1, std::min(frSpan, priorSpan));
        if (overlap * 2 >= minSpan) return &e;
    }

    if (entries.empty() || prefixMaxEnd.size() != entries.size()) return nullptr;

    // Candidates all live in [0, hi]: startSector <= frEnd.
    auto hiIt = std::upper_bound(entries.begin(), entries.end(), frEnd,
                                 [](uint64_t v, const Entry& e) { return v < e.startSector; });
    const int hi = static_cast<int>(hiIt - entries.begin()) - 1;
    if (hi < 0) return nullptr;
    if (prefixMaxEnd[hi] < frStart) return nullptr; // nothing reaches back to the query

    auto j0It = std::lower_bound(prefixMaxEnd.begin(), prefixMaxEnd.begin() + hi + 1, frStart);
    const int j0 = static_cast<int>(j0It - prefixMaxEnd.begin());
    const int windowStart = std::max(j0, std::max(0, hi - kMaxWindowScan));

    const int edgeEnd = std::min(hi, j0 + kEdgeProbe - 1);
    for (int idx = j0; idx <= edgeEnd; ++idx) {
        const Entry& e = entries[idx];
        if (e.endSector < frStart) continue;
        if (!sectorsOverlap(e.startSector, e.endSector, frStart, frEnd)) continue;
        const uint64_t overlap = overlapSectorCount(e.startSector, e.endSector, frStart, frEnd);
        const uint64_t priorSpan = e.endSector >= e.startSector ? (e.endSector - e.startSector + 1) : 1;
        const uint64_t minSpan = std::max<uint64_t>(1, std::min(frSpan, priorSpan));
        if (overlap * 2 >= minSpan) return &e;
    }

    for (int64_t idx = hi; idx >= windowStart; --idx) {
        const Entry& e = entries[static_cast<size_t>(idx)];
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
    const Entry* hit = findOverlap(carveEntries_, carvePrefixMaxEnd_, fr.startSector, carveEnd,
                                   carveSpan, carvePending_);
    if (!hit) return false;
    return true;
}

bool DedupIndex::markDuplicate(FileRecord& fr) {
    if (!isCarveSource(fr.source)) return false;

    // Merge only at threshold — the linear pending check keeps queries correct
    // between merges, so a 500K-record scan does ~500 cheap merges instead of
    // 500K full sorts.
    if (carvePending_.size() >= kMergeThreshold) mergeCarvePending();
    if (metaPending_.size() >= kMergeThreshold) mergeMetaPending();

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
    const Entry* meta = findOverlap(entries_, metaPrefixMaxEnd_, fr.startSector, carveEnd, carveSpan,
                                    metaPending_);
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
    carvePending_.push_back(std::move(tracked));
    if (!fr.contentHash.empty()) contentHashes_.emplace(fr.contentHash, fr.sizeBytes);
    if (carvePending_.size() >= kMergeThreshold) mergeCarvePending();
    return false;
}

} // namespace byteback
