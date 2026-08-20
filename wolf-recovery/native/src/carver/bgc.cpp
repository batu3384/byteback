// Bifragmented Gap Carving (BGC) — standalone implementation, separated from
// signature_engine.cpp so the unit tests can link it without pulling in the
// DiskReader / MemoryPool dependencies of the full carving engine.
//
// See wolf_carver.h (bifragmentedGapCarve) for the algorithm rationale.
#include "wolf_carver.h"

#include <cstdint>
#include <vector>

namespace wolf {

BgcResult bifragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                               size_t headerOffset, size_t footerOffset,
                               size_t maxGapBytes,
                               const std::function<int(const uint8_t*, size_t)>& validator,
                               size_t stepBytes) {
    BgcResult out;
    if (!disk || !validator || headerOffset >= diskSize || footerOffset <= headerOffset)
        return out;

    size_t span = footerOffset - headerOffset;
    if (span < 4) return out;

    // Caller supplies maxGapBytes (0 → 64 KiB default). No absolute clamp.
    // Step on allocation boundaries so the search stays tractable.
    size_t gapLimit = maxGapBytes ? maxGapBytes : (64 * 1024);
    if (gapLimit > span) gapLimit = span;

    if (stepBytes == 0) stepBytes = 1;
    if (stepBytes > span) stepBytes = span;

    // Reassembly buffer: the span minus one gap. Worst case = full span.
    std::vector<uint8_t> reassembled;
    reassembled.reserve(span);

    // Step the gap start on `stepBytes` boundaries (fragment 1 length is a
    // multiple of the allocation unit), and the gap length likewise.
    for (size_t gapStart = headerOffset + stepBytes; gapStart < footerOffset; gapStart += stepBytes) {
        size_t localStart = gapStart - headerOffset;
        for (size_t gapLen = stepBytes; gapLen <= gapLimit && gapStart + gapLen <= footerOffset; gapLen += stepBytes) {
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
                out.found = true;
                out.frag1Len = localStart;
                out.gapLen = gapLen;
                return out;
            }
        }
    }
    return out;
}

} // namespace wolf
