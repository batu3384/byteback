#include "fs/unallocated_map.h"
#include "scan_coordinator.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <vector>

using namespace wolf;

namespace {

bool rangeCovers(const std::vector<SectorRange>& ranges, uint64_t sector) {
    for (const auto& r : ranges) {
        if (sector >= r.start && sector < r.start + r.count) return true;
    }
    return false;
}

} // namespace

TEST(UnallocatedMap, FatExcludesAllocatedCluster) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(fatVol));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Fat, 0, reader.getDiskSize());
    ASSERT_FALSE(ranges.empty());

    // buildFat16Volume: dataStart sector = 34, cluster 2 holds TEST.TXT (allocated).
    const uint64_t cluster2Sector = 34;
    EXPECT_FALSE(rangeCovers(ranges, cluster2Sector));
    EXPECT_TRUE(rangeCovers(ranges, cluster2Sector + 1)); // cluster 3 is free
}

TEST(UnallocatedMap, CarveUnallocatedOnlySkipsAllocatedPng) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    const uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                              0xAE, 0x42, 0x60, 0x82};

    // PNG in allocated cluster 2 — unallocated carve must not find it.
    std::memcpy(fatVol.data() + 34 * 512, pngSig, sizeof(pngSig));
    std::memcpy(fatVol.data() + 34 * 512 + 512 - sizeof(iend), iend, sizeof(iend));

    DiskReader readerAlloc;
    readerAlloc.attachMemoryVolume(fatVol);

    std::vector<std::string> sourcesAlloc;
    std::atomic<bool> running{true};
    runCarveScan(readerAlloc, [&](const FileRecord& fr) {
        if (fr.id != -1 && !fr.source.empty()) sourcesAlloc.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    bool carvedAlloc = false;
    for (const auto& s : sourcesAlloc) {
        if (s == "carver" || s == "carver_bgc") carvedAlloc = true;
    }
    EXPECT_FALSE(carvedAlloc);

    // Same PNG in free cluster 3 — unallocated carve should find it.
    auto fatVol2 = wolf::testfix::buildFat16Volume();
    std::memcpy(fatVol2.data() + 35 * 512, pngSig, sizeof(pngSig));
    std::memcpy(fatVol2.data() + 35 * 512 + 512 - sizeof(iend), iend, sizeof(iend));

    DiskReader readerFree;
    readerFree.attachMemoryVolume(std::move(fatVol2));

    std::vector<std::string> sourcesFree;
    runCarveScan(readerFree, [&](const FileRecord& fr) {
        if (fr.id != -1 && !fr.source.empty()) sourcesFree.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    bool carvedFree = false;
    for (const auto& s : sourcesFree) {
        if (s == "carver" || s == "carver_bgc") carvedFree = true;
    }
    EXPECT_TRUE(carvedFree);
}

TEST(UnallocatedMap, UnknownFsUnallocatedOnlySkipsCarve) {
    std::vector<uint8_t> disk(512 * 64, 0);
    const uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(disk.data() + 10 * 512, pngSig, sizeof(pngSig));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::atomic<bool> running{true};
    size_t carved = 0;
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver" || fr.source == "carver_bgc") ++carved;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    EXPECT_EQ(carved, 0u);
}

TEST(UnallocatedMap, FullCarveFindsPngInAllocatedCluster) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    const uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                              0xAE, 0x42, 0x60, 0x82};
    std::memcpy(fatVol.data() + 34 * 512, pngSig, sizeof(pngSig));
    std::memcpy(fatVol.data() + 34 * 512 + 512 - sizeof(iend), iend, sizeof(iend));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(fatVol));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runFullCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && !fr.source.empty()) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    bool carvedAlloc = false;
    for (const auto& s : sources) {
        if (s == "carver" || s == "carver_bgc") carvedAlloc = true;
    }
    EXPECT_TRUE(carvedAlloc);
}
