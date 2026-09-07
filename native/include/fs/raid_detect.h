#pragma once

// RAID geometry autodetection over member disk images. For each candidate
// (level, stripe size, parity start/rotation) the detector samples stripes
// and scores parity consistency:
//   RAID5 — XOR of data blocks == P block
//   RAID6 — XOR == P and GF(2^8) Reed-Solomon syndrome == Q
//   RAID1 — member contents identical
// Best-scoring candidate wins; below the confidence floor the result is
// "unknown" (honest fail, never a guess — a wrong geometry silently
// interleaves unrelated sectors, which is worse than no array).

#include <cstddef>
#include <cstdint>
#include <vector>
#include "fs/virtual_raid.h" // RaidLevel

namespace byteback {

class DiskReader;

struct RaidGeometry {
    RaidLevel level = RaidLevel::RAID0;
    size_t blockSize = 0;          // stripe/chunk size in bytes
    uint64_t dataOffsetSectors = 0; // per-member start offset (512B sectors)
    double confidence = 0.0;        // 0..1 parity-consistency score
};

// members must be opened and equal-sized (caller validates sizes).
// Returns false when no candidate reaches the confidence floor.
bool detectRaidGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out);

} // namespace byteback
