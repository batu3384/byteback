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

TEST(UnallocatedMap, UnknownFsUnallocatedOnlyFallsBackToWholeDisk) {
    // CA-022: an unknown/unsupported filesystem used to produce an empty
    // unallocated range set — deep scan carved zero sectors and still reported
    // 100% complete. The empty map now falls back to whole-partition carving,
    // so a real footer-complete PNG is found even here.
    std::vector<uint8_t> disk(512 * 64, 0);
    const auto png = byteback::testfix::buildMinimalValidPng();
    std::memcpy(disk.data() + 10 * 512, png.data(), png.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::atomic<bool> running{true};
    size_t carved = 0;
    runCarveScan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver" || fr.source == "carver_bgc") ++carved;
    }, [&](uint64_t, uint64_t) {}, &running, nullptr, {}, true);

    EXPECT_GE(carved, 1u);
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

namespace {

// Minimal FAT12 superfloppy: bps 512, spc 1, reserved 1, one FAT,
// 16 root entries (1 root sector). countOfClusters stays < 4085 so the
// volume classifies as FAT12.
std::vector<uint8_t> buildFat12Volume(uint32_t totalSectors, uint32_t fatSectors) {
    constexpr uint32_t ss = 512;
    std::vector<uint8_t> img(totalSectors * ss, 0);
    byteback::testfix::TestFatBpb bpb{};
    bpb.jmp[0] = 0xEB; bpb.jmp[1] = 0x3C; bpb.jmp[2] = 0x90;
    std::memcpy(bpb.oem, "MSWIN4.1", 8);
    bpb.bytesPerSector = static_cast<uint16_t>(ss);
    bpb.sectorsPerCluster = 1;
    bpb.reservedSectors = 1;
    bpb.numFats = 1;
    bpb.rootEntryCount = 16;
    bpb.media = 0xF8;
    bpb.fatSize16 = static_cast<uint16_t>(fatSectors);
    bpb.totalSectors32 = totalSectors;
    bpb.bootSig = 0x29;
    std::memcpy(bpb.fsType, "FAT12   ", 8);
    std::memcpy(img.data(), &bpb, sizeof(bpb));
    img[510] = 0x55; img[511] = 0xAA;
    return img;
}

// Exact FAT12 nibble arithmetic (odd entries use bits 4..15 of the byte
// pair) so marked entries cannot silently allocate their neighbours.
void setFat12Entry(std::vector<uint8_t>& img, uint16_t entry, uint16_t value) {
    constexpr size_t kFatByteOff = 512; // reservedSectors = 1
    value &= 0x0FFF;
    const size_t off = kFatByteOff + (entry * 3) / 2;
    ASSERT_LT(off + 1, img.size());
    if (entry & 1) {
        img[off] = static_cast<uint8_t>((img[off] & 0x0F) | ((value & 0x0F) << 4));
        img[off + 1] = static_cast<uint8_t>(value >> 4);
    } else {
        img[off] = static_cast<uint8_t>(value);
        img[off + 1] = static_cast<uint8_t>((img[off + 1] & 0xF0) | (value >> 8));
    }
}

bool rangeEquals(const std::vector<SectorRange>& ranges, size_t i, uint64_t start, uint64_t count) {
    return i < ranges.size() && ranges[i].start == start && ranges[i].count == count;
}

} // namespace

// CA-030: a BPB whose FAT/reserved span over-declares totalSectors used to
// wrap the u32 subtraction to ~4G dataSectors — the cluster loop then spun
// for hundreds of millions of iterations with a 512B read each (a hang).
// The volume must be refused (empty map) instead.
TEST(UnallocatedMap, HostileFatBpbOverdeclaredFatReturnsEmpty) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    // offsetof(TestFatBpb, fatSize16) == 22; 2 * 0xFFF0 + 2 > 4120 total.
    byteback::testfix::writeLe16(fatVol, 22, 0xFFF0);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(fatVol));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Fat, 0, reader.getDiskSize());
    EXPECT_TRUE(ranges.empty());
}

// CA-030: sectorsPerCluster = 0 divided by zero right after the wrap.
TEST(UnallocatedMap, HostileFatBpbZeroSectorsPerClusterReturnsEmpty) {
    auto fatVol = byteback::testfix::buildFat16Volume();
    byteback::testfix::writeLe16(fatVol, 11, 512); // keep bps sane
    fatVol[13] = 0;                                // offsetof(sectorsPerCluster) == 13

    DiskReader reader;
    reader.attachMemoryVolume(std::move(fatVol));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Fat, 0, reader.getDiskSize());
    EXPECT_TRUE(ranges.empty());
}

// CA-030: bulk FAT read must produce the same cluster map the per-cluster
// walk did — including FAT12 entries whose bytes straddle FAT sector
// boundaries (entry 341 here spans FAT bytes 511|512, i.e. sectors 1|2).
TEST(UnallocatedMap, Fat12BulkReadMatchesExpectedClusterMap) {
    // meta = 1 reserved + 3 FAT + 1 root = 5; 1019 data sectors = clusters
    // 2..1020. The 3-sector FAT (1536 bytes) covers every entry, including
    // entry 341 whose bytes straddle the FAT sector 1|2 boundary.
    auto img = buildFat12Volume(1024, 3);
    for (uint16_t c : {2, 5, 30, 31, 32, 341}) setFat12Entry(img, c, 0xFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Fat, 0, reader.getDiskSize());
    // dataStart = 5; free runs 3-4, 6-29, 33-340, 342-1020 ->
    // sectors {6,2}, {9,24}, {36,308}, {345,679}.
    ASSERT_EQ(ranges.size(), 4u);
    EXPECT_TRUE(rangeEquals(ranges, 0, 6, 2));
    EXPECT_TRUE(rangeEquals(ranges, 1, 9, 24));
    EXPECT_TRUE(rangeEquals(ranges, 2, 36, 308));
    EXPECT_TRUE(rangeEquals(ranges, 3, 345, 679));
}

// CA-030: entries reaching past the declared FAT end are allocated, not
// free — and the read is bounds-checked (the old per-cluster path overread
// the heap by one byte on straddling entries at the table tail).
TEST(UnallocatedMap, Fat12EntryPastTableEndIsNotFree) {
    // FAT = 1 sector = 512 bytes -> entries through index 341 fit; entry
    // 341's second byte lies at the table end and 342+ are past it.
    auto img = buildFat12Volume(800, 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto ranges = buildUnallocatedRanges(reader, VolumeFsKind::Fat, 0, reader.getDiskSize());
    // dataStart = 3; clusters 2..340 free = 339 sectors, everything from
    // cluster 341 on is (conservatively) allocated.
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_TRUE(rangeEquals(ranges, 0, 3, 339));
}
