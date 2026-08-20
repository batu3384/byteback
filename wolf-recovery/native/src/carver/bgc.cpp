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

BgcResult triFragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                                size_t headerOffset, size_t footerOffset,
                                size_t maxGapBytes,
                                const std::function<int(const uint8_t*, size_t)>& validator,
                                size_t stepBytes, size_t attemptBudget) {
    BgcResult out;
    if (!disk || !validator || headerOffset >= diskSize || footerOffset <= headerOffset) {
        return out;
    }
    size_t span = footerOffset - headerOffset;
    if (span < 8) return out;
    size_t gapLimit = maxGapBytes ? maxGapBytes : (64 * 1024);
    if (gapLimit > span) gapLimit = span;
    if (stepBytes == 0) stepBytes = 1;
    if (stepBytes > span) stepBytes = span;

    std::vector<uint8_t> reassembled;
    reassembled.reserve(span);
    size_t attempts = 0;

    for (size_t g1Start = headerOffset + stepBytes; g1Start < footerOffset && attempts < attemptBudget;
         g1Start += stepBytes) {
        for (size_t g1Len = stepBytes; g1Len <= gapLimit && g1Start + g1Len < footerOffset; g1Len += stepBytes) {
            size_t afterG1 = g1Start + g1Len;
            for (size_t g2Start = afterG1 + stepBytes; g2Start < footerOffset && attempts < attemptBudget;
                 g2Start += stepBytes) {
                for (size_t g2Len = stepBytes; g2Len <= gapLimit && g2Start + g2Len <= footerOffset;
                     g2Len += stepBytes) {
                    ++attempts;
                    reassembled.clear();
                    reassembled.insert(reassembled.end(), disk + headerOffset, disk + g1Start);
                    reassembled.insert(reassembled.end(), disk + afterG1, disk + g2Start);
                    reassembled.insert(reassembled.end(), disk + (g2Start + g2Len), disk + footerOffset);
                    int score = validator(reassembled.data(), reassembled.size());
                    if (score >= 85) {
                        out.found = true;
                        out.frag1Len = g1Start - headerOffset;
                        out.gapLen = g1Len;
                        out.frag2Len = g2Start - afterG1;
                        out.gap2Len = g2Len;
                        return out;
                    }
                }
            }
        }
    }
    return out;
}

} // namespace wolf
