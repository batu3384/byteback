#pragma once

// Pure FAT/exFAT chain and timestamp math, extracted from fat_parser.cpp so
// the correctness-critical pieces (CA-005) are unit-testable without a
// DiskReader: callers inject a FAT-entry reader lambda and get physical
// sector runs back.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace wolf {
namespace fat {

// Convert a DOS date/time pair (FAT and exFAT directory entries) to Unix
// seconds. DOS epoch is 1980-01-01 with 2-second granularity; days-from-civil
// arithmetic (Howard Hinnant) avoids any calendar-library dependence.
// Returns 0 for out-of-range month/day.
int64_t dosTimestampToUnix(uint16_t dosDate, uint16_t dosTime);

struct ChainRun {
    uint64_t startSector;
    uint64_t sectorCount;
};

// FAT entry reader: returns the table value for a cluster. Callers translate
// read failures into EOC (0xFFFFFFF8-class values) so the walk stops cleanly.
using FatEntryReader = std::function<uint32_t(uint32_t cluster)>;

// Walk the cluster chain starting at firstCluster and emit physical sector
// runs relative to the data region: sector(N) = dataStartSector + (N-2)*spc.
//
// Guarantees:
//   - stops at EOC (>=0xFF8/0xFFF8/0x0FFFFFF8 by fatBits) and bad-cluster
//     markers (0xFF7/0xFFF7/0x0FFFFFF7),
//   - stops on entries < 2 (free/reserved),
//   - cycle-safe: a repeated cluster ends the walk with what was built,
//   - bounded by maxClusters (deleted files can have cleared chains).
//
// fatBits: 12, 16 or 32 (exFAT uses 32 with a different entry alphabet but
// the same numeric conventions).
std::vector<ChainRun> chainRuns(FatEntryReader readEntry, int fatBits,
                                uint32_t firstCluster, uint32_t sectorsPerCluster,
                                uint64_t dataStartSector, size_t maxClusters);

} // namespace fat
} // namespace wolf
