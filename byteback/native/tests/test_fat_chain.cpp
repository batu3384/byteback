// Native unit tests for the pure FAT chain/timestamp math (fs/fat_chain).
// The chain walk is what recovery correctness rests on (CA-002/CA-005):
// a wrong run means the right offset read from the wrong disk region.
#include "fs/fat_chain.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <map>
#include <vector>

using namespace byteback::fat;

// ---- DOS timestamp ----
TEST(FatDosTime, KnownDates) {
    // 1980-01-01 00:00:00 — DOS epoch.
    // date = (0 << 9) | (1 << 5) | 1 = 0x21 ; time = 0
    EXPECT_EQ(dosTimestampToUnix(0x0021, 0x0000), 315532800LL);

    // 2020-02-29 12:30:00 — leap day (2-second granularity).
    // date = (40 << 9) | (2 << 5) | 29 ; time = (12 << 11) | (30 << 5) | 0
    EXPECT_EQ(dosTimestampToUnix((40 << 9) | (2 << 5) | 29, (12 << 11) | (30 << 5)),
              1582979400LL);

    // 2024-12-31 23:59:58 — last representable second of a leap year.
    EXPECT_EQ(dosTimestampToUnix((44 << 9) | (12 << 5) | 31, (23 << 11) | (59 << 5) | 29),
              1735689598LL);

    // 1999-09-09 09:09:08 — odd seconds round DOWN to the 2s grid.
    EXPECT_EQ(dosTimestampToUnix((19 << 9) | (9 << 5) | 9, (9 << 11) | (9 << 5) | 4),
              936868148LL);
}

TEST(FatDosTime, InvalidFieldsReturnZero) {
    EXPECT_EQ(dosTimestampToUnix((0 << 9) | (0 << 5) | 1, 0), 0);  // month 0
    EXPECT_EQ(dosTimestampToUnix((0 << 9) | (13 << 5) | 1, 0), 0); // month 13
    EXPECT_EQ(dosTimestampToUnix((0 << 9) | (1 << 5) | 0, 0), 0);  // day 0
    EXPECT_EQ(dosTimestampToUnix((0 << 9) | (1 << 5) | 32, 0), 0); // day 32
}

// ---- chain walk ----
namespace {
std::vector<ChainRun> walk(const std::map<uint32_t, uint32_t>& table, int bits,
                           uint32_t first, uint32_t spc, uint64_t dataStart,
                           size_t maxClusters = 65536) {
    FatEntryReader r = [&table](uint32_t c) {
        auto it = table.find(c);
        return it == table.end() ? 0xFFFFFFFFu : it->second;
    };
    return chainRuns(std::move(r), bits, first, spc, dataStart, maxClusters);
}
} // namespace

TEST(FatChain, LinearChainFAT16) {
    // 2 -> 3 -> 5 -> EOC; spc=4, data starts at sector 100.
    // sector(N) = 100 + (N-2)*4
    auto runs = walk({{2, 3}, {3, 5}, {5, 0xFFF8}}, 16, 2, 4, 100);
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].startSector, 100u);
    EXPECT_EQ(runs[1].startSector, 104u);
    EXPECT_EQ(runs[2].startSector, 112u);
    for (auto& r : runs) EXPECT_EQ(r.sectorCount, 4u);
}

TEST(FatChain, Fat12PackedEntries) {
    // FAT12 stores 12-bit entries packed 2-per-3-bytes; the reader simulates
    // the unpacking the parser adapter performs. Cluster 2 -> 7 (even slot,
    // low 12 bits), cluster 7 -> EOC.
    // Raw bytes for entries [2..3]: 0x07 0x38 0x00 => entry2=0x307? No —
    // verify by construction: even cluster N uses bytes 3N/2..3N/2+1 low
    // 12 bits; odd uses the high 12. We hand the UNPACKED values via the
    // injected reader (packing itself lives in the adapter's readEntry),
    // so here we assert the walk treats FAT12 thresholds correctly.
    auto runs = walk({{2, 7}, {7, 0xFF8}}, 12, 2, 1, 50);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].startSector, 50u);
    EXPECT_EQ(runs[1].startSector, 55u);
}

TEST(FatChain, StopsAtEocBadAndFree) {
    // First cluster itself is EOC -> nothing.
    EXPECT_TRUE(walk({}, 32, 0x0FFFFFF8, 8, 100).empty());
    // Chain ends at a bad-cluster marker -> keeps prior runs.
    auto badRuns = walk({{2, 0x0FFFFFF7}}, 32, 2, 8, 100);
    ASSERT_EQ(badRuns.size(), 1u);
    // Next entry 0/1 (free/reserved) ends the walk.
    auto freeRuns = walk({{2, 0}}, 32, 2, 8, 100);
    ASSERT_EQ(freeRuns.size(), 1u);
    // firstCluster < 2 -> empty.
    EXPECT_TRUE(walk({}, 16, 1, 8, 100).empty());
}

TEST(FatChain, CycleGuard) {
    // 2 -> 3 -> 2: the repeated cluster ends the walk without looping.
    auto runs = walk({{2, 3}, {3, 2}}, 16, 2, 4, 100);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].startSector, 100u);
    EXPECT_EQ(runs[1].startSector, 104u);
}

TEST(FatChain, MaxClustersBoundsWalk) {
    // A long chain capped at 3 entries.
    std::map<uint32_t, uint32_t> t;
    for (uint32_t c = 2; c < 100; ++c) t[c] = c + 1;
    auto runs = walk(t, 32, 2, 1, 0, 3);
    EXPECT_EQ(runs.size(), 3u);
}

TEST(FatChain, ExFatNumericConventions) {
    // exFAT uses FAT32-style 32-bit entries; same walk applies.
    auto runs = walk({{2, 9}, {9, 0xFFFFFFFF}}, 32, 2, 16, 2048);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].startSector, 2048u);
    EXPECT_EQ(runs[1].startSector, 2048u + 7u * 16u);
}
