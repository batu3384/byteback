// Bifragmented Gap Carving (BGC) — standalone implementation, separated from
// signature_engine.cpp so the unit tests can link it without pulling in the
// DiskReader / MemoryPool dependencies of the full carving engine.
//
// See wolf_carver.h (bifragmentedGapCarve) for the algorithm rationale.
#include "wolf_carver.h"

#include <cstdint>
#include <vector>

namespace wolf {

size_t bifragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                            size_t headerOffset, size_t footerOffset,
                            size_t maxGapBytes,
                            int (*validator)(const uint8_t*, size_t)) {
    if (!disk || !validator || headerOffset >= diskSize || footerOffset <= headerOffset)
        return SIZE_MAX;

    size_t span = footerOffset - headerOffset;
    if (span < 4) return SIZE_MAX;

    // Cap the search so pathological cases do not scan the whole disk.
    size_t gapLimit = maxGapBytes ? maxGapBytes : (1u << 20); // default 1 MiB
    if (gapLimit > span) gapLimit = span;

    // Reassembly buffer: the span minus one gap. Worst case = full span.
    std::vector<uint8_t> reassembled;
    reassembled.reserve(span);

    // Step the gap start across the span. For each start, grow the gap length
    // up to gapLimit and validate. Early-exit on the first success.
    // ponytail: O(span * gapLimit) worst case; acceptable for deep scan where
    // the number of header/footer pairs is small. Upgrade path: only try gaps
    // aligned to cluster boundaries (sectorsPerCluster * sectorSize) to cut
    // the constant factor by 512x+.
    for (size_t gapStart = headerOffset + 1; gapStart < footerOffset; ++gapStart) {
        size_t localStart = gapStart - headerOffset;
        for (size_t gapLen = 1; gapLen <= gapLimit && gapStart + gapLen <= footerOffset; ++gapLen) {
            // Reassemble: [headerOffset, gapStart) ++ [gapStart+gapLen, footerOffset)
            reassembled.clear();
            reassembled.insert(reassembled.end(),
                               disk + headerOffset, disk + gapStart);
            size_t frag2Start = gapStart + gapLen;
            if (frag2Start < footerOffset) {
                reassembled.insert(reassembled.end(),
                                   disk + frag2Start, disk + footerOffset);
            }
            int score = validator(reassembled.data(), reassembled.size());
            // Require a high-confidence validation: partial scores (e.g. a
            // truncated JPEG with SOS but no EOI) are too easy to hit by
            // accident when reassembling arbitrary byte ranges, so a 85+
            // floor keeps false positives out of the gap search.
            if (score >= 85) {
                return localStart;
            }
        }
    }
    return SIZE_MAX;
}

} // namespace wolf
