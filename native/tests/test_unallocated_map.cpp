#include "fs/unallocated_map.h"
#include "scan_coordinator.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <vector>

using namespace byteback;

namespace {

bool rangeCovers(const std::vector<SectorRange>& ranges, uint64_t sector) {
    for (const auto& r : ranges) {
        if (sector >= r.start && sector < r.start + r.count) return true;
    }
    return false;
}

} // namespace

TEST(UnallocatedMap, FatExcludesAllocatedCluster) {
    auto fatVol = byteback::testfix::buildFat16Volume();
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
    auto fatVol = byteback::testfix::buildFat16Volume();
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
    auto fatVol2 = byteback::testfix::buildFat16Volume();
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

TEST(UnallocatedMap, FullCarveFindsPngOnRawDisk) {
    // Unknown FS + unallocatedOnly=false scans the whole partition (see UnknownFsUnallocatedOnlySkipsCarve).
    std::vector<uint8_t> disk(512 * 128, 0);
    const auto png = byteback::testfix::buildMinimalValidPng();
    std::memcpy(disk.data() + 64 * 512, png.data(), png.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && !fr.source.empty()) sources.push_back(fr.source);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, false);

    bool carved = false;
    for (const auto& s : sources) {
        if (s == "carver" || s == "carver_bgc") carved = true;
    }
    EXPECT_TRUE(carved);
}

TEST(UnallocatedMap, ExFatExcludesAllocatedCluster) {
    auto exVol = byteback::testfix::buildExFatUnallocatedVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(exVol));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::ExFat, 0, reader.getDiskSize());
    ASSERT_FALSE(ranges.empty());

    // heapOff=25: cluster 3 data at sector 26 (allocated), cluster 4 at sector 27 (free).
    const uint64_t allocatedSector = 26;
    const uint64_t freeSector = 27;
    EXPECT_FALSE(rangeCovers(ranges, allocatedSector));
    EXPECT_TRUE(rangeCovers(ranges, freeSector));
}

TEST(UnallocatedMap, ExFatUnallocatedCarveFindsPngInFreeCluster) {
    auto exVol = byteback::testfix::buildExFatUnallocatedVolume();
    const uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                              0xAE, 0x42, 0x60, 0x82};
    std::memcpy(exVol.data() + 27 * 512, pngSig, sizeof(pngSig));
    std::memcpy(exVol.data() + 27 * 512 + 512 - sizeof(iend), iend, sizeof(iend));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(exVol));

    std::atomic<bool> running{true};
    size_t carved = 0;
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver" || fr.source == "carver_bgc") ++carved;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    EXPECT_GE(carved, 1u);
}

TEST(UnallocatedMap, Ext4ExcludesAllocatedBlock) {
    auto extVol = byteback::testfix::buildExt4CarveVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(extVol));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Ext4, 0, reader.getDiskSize());
    ASSERT_FALSE(ranges.empty());

    const uint64_t allocatedSector = 7 * 2; // block 7 @ 1KiB blocks, 512 B sectors
    const uint64_t freeSector = 8 * 2;
    EXPECT_FALSE(rangeCovers(ranges, allocatedSector));
    EXPECT_TRUE(rangeCovers(ranges, freeSector));
}

TEST(UnallocatedMap, Ext4UnallocatedCarveFindsPngInFreeBlock) {
    auto extVol = byteback::testfix::buildExt4CarveVolume();
    const uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                              0xAE, 0x42, 0x60, 0x82};
    const size_t freeOff = 8 * 1024;
    std::memcpy(extVol.data() + freeOff, pngSig, sizeof(pngSig));
    std::memcpy(extVol.data() + freeOff + 1024 - sizeof(iend), iend, sizeof(iend));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(extVol));

    std::atomic<bool> running{true};
    size_t carved = 0;
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver" || fr.source == "carver_bgc") ++carved;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    EXPECT_GE(carved, 1u);
}
