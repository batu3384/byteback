// Bifragmented Gap Carving (BGC) — standalone implementation, separated from
// signature_engine.cpp so the unit tests can link it without pulling in the
// DiskReader / MemoryPool dependencies of the full carving engine.
//
// See byteback_carver.h (bifragmentedGapCarve) for the algorithm rationale.
#include "byteback_carver.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace byteback {

namespace {
// CA-035: skip reassembly attempts whose copy cost exceeds this bound — with
// a huge candidate span the per-attempt span copy dominated the scan even
// inside the attempt budget.
constexpr size_t kMaxSpanCopyBytes = 64ull * 1024 * 1024;

bool cancelled(std::atomic<bool>* isRunning) {
    return isRunning && !isRunning->load(std::memory_order_relaxed);
}
} // namespace

BgcResult bifragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                               size_t headerOffset, size_t footerOffset,
                               size_t maxGapBytes,
                               const std::function<int(const uint8_t*, size_t)>& validator,
                               size_t stepBytes, size_t attemptBudget,
                               std::atomic<bool>* isRunning) {
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
    reassembled.reserve(std::min(span, kMaxSpanCopyBytes));

    // CA-021: bounded attempts, like the tri-fragmented variant. Span 16MB at
    // 512B steps is ~10^8 reassemblies without a budget — minutes-to-hours
    // inside the scan thread for one junk candidate.
    size_t attempts = 0;
    for (size_t gapStart = headerOffset + stepBytes; gapStart < footerOffset && attempts < attemptBudget;
         gapStart += stepBytes) {
        size_t localStart = gapStart - headerOffset;
        for (size_t gapLen = stepBytes; gapLen <= gapLimit && gapStart + gapLen <= footerOffset; gapLen += stepBytes) {
            if (++attempts >= attemptBudget) return out;
            // CA-035: yield promptly on cancel and refuse to copy spans that
            // can never validate cheaply.
            if (cancelled(isRunning)) return out;
            size_t frag2Start = gapStart + gapLen;
            const size_t copyBytes = localStart + (footerOffset - frag2Start);
            if (copyBytes > kMaxSpanCopyBytes) continue;
            // Reassemble: [headerOffset, gapStart) ++ [gapStart+gapLen, footerOffset)
            reassembled.clear();
            reassembled.insert(reassembled.end(),
                               disk + headerOffset, disk + gapStart);
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
                                size_t stepBytes, size_t attemptBudget,
                                std::atomic<bool>* isRunning) {
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
    reassembled.reserve(std::min(span, kMaxSpanCopyBytes));
    size_t attempts = 0;

    for (size_t g1Start = headerOffset + stepBytes; g1Start < footerOffset && attempts < attemptBudget;
         g1Start += stepBytes) {
        for (size_t g1Len = stepBytes; g1Len <= gapLimit && g1Start + g1Len < footerOffset; g1Len += stepBytes) {
            size_t afterG1 = g1Start + g1Len;
            for (size_t g2Start = afterG1 + stepBytes; g2Start < footerOffset && attempts < attemptBudget;
                 g2Start += stepBytes) {
                for (size_t g2Len = stepBytes; g2Len <= gapLimit && g2Start + g2Len <= footerOffset;
                     g2Len += stepBytes) {
                    // CA-021: check the budget in the innermost loop too —
                    // without this the sweep runs one full gap-length series
                    // past the budget before an outer condition re-checks.
                    if (++attempts >= attemptBudget) return out;
                    // CA-035: yield promptly on cancel and refuse to copy
                    // spans that can never validate cheaply.
                    if (cancelled(isRunning)) return out;
                    const size_t copyBytes = (g1Start - headerOffset) + (g2Start - afterG1) +
                                             (footerOffset - (g2Start + g2Len));
                    if (copyBytes > kMaxSpanCopyBytes) continue;
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

} // namespace byteback
