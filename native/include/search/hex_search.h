#pragma once

#include "byteback_io.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace byteback {

struct HexSearchHit {
    uint64_t byteOffset = 0;
};

inline constexpr size_t kHexSearchMaxNeedle = 64;
inline constexpr uint64_t kHexSearchMaxHits = 256;
inline constexpr uint32_t kHexSearchChunk = 1u << 20;

// Raw byte needle on the attached volume. Overlapping chunk reads.
// ioUnread windows set *unread and do not emit hits from those bytes.
std::vector<HexSearchHit> searchRawBytes(DiskReader& reader,
                                         const uint8_t* needle, size_t needleLen,
                                         uint64_t maxHits,
                                         std::atomic<bool>* isRunning,
                                         bool* unread);

} // namespace byteback
