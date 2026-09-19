// RAID geometry autodetection tests — parity-consistency fixtures built with
// the production raid_layout / raid6_math so detection is scored against the
// exact layout VirtualRaid reads (mirrors test_virtual_raid_io.cpp fixtures).
#include "fs/raid_detect.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/raid_layout.h"
#include "fs/virtual_raid.h"

using namespace byteback;

namespace {

// Non-linear block mix (fmix32). MUST NOT be a GF(2)-linear PRNG (xorshift
// etc.): linear streams make members filled with XOR-related seeds satisfy
// XOR parity identities vacuously — the fixture would fake RAID evidence.
uint32_t mix32(uint32_t z) {
    z ^= z >> 16;
    z *= 0x85EBCA6Bu;
    z ^= z >> 13;
    z *= 0xC2B2AE35u;
    z ^= z >> 16;
    return z;
}

void fillPseudo(std::vector<uint8_t>& img, uint32_t seed) {
    for (size_t i = 0; i < img.size(); ++i)
        img[i] = static_cast<uint8_t>(mix32(static_cast<uint32_t>(i) ^ seed) >> 24);
}

// N-member RAID5 images: pseudorandom data, P = XOR of data blocks, placed
// per raid_layout (left-asymmetric rotating parity), after offsetBytes.
// reservationSeed != 0 fills the pre-data region with vendor-metadata-style
// garbage (real controllers leave boot code/metadata there; zeros would let
// every offset candidate pass trivially).
std::vector<std::vector<uint8_t>> buildRaid5(uint32_t n, size_t stripes, size_t sb,
                                             uint64_t offsetBytes, uint32_t seed,
                                             uint32_t reservationSeed = 0) {
    std::vector<std::vector<uint8_t>> images(n, std::vector<uint8_t>(offsetBytes + stripes * sb, 0));
    if (reservationSeed != 0)
        for (auto& img : images)
            for (uint64_t i = 0; i < offsetBytes; ++i)
                img[i] = static_cast<uint8_t>(mix32(static_cast<uint32_t>(i) ^ reservationSeed) >> 24);
    for (uint64_t s = 0; s < stripes; ++s) {
        const uint32_t p = raid_layout::raid5ParityDisk(s, n);
        std::vector<uint8_t> acc(sb, 0);
        for (uint32_t blk = 0; blk + 1 < n; ++blk) {
            const uint32_t disk = raid_layout::raid5DataDisk(s, blk, n);
            std::vector<uint8_t> data(sb);
            fillPseudo(data, seed + static_cast<uint32_t>(s * 131u + blk * 17u));
            for (size_t b = 0; b < sb; ++b) acc[b] ^= data[b];
            std::memcpy(images[disk].data() + offsetBytes + s * sb, data.data(), sb);
        }
        std::memcpy(images[p].data() + offsetBytes + s * sb, acc.data(), sb);
    }
    return images;
}

// N-member RAID6 images: P = XOR of data, Q = XOR of gfMul(data, g^slot).
std::vector<std::vector<uint8_t>> buildRaid6(uint32_t n, size_t stripes, size_t sb,
                                             uint64_t offsetBytes, uint32_t seed) {
    std::vector<std::vector<uint8_t>> images(n, std::vector<uint8_t>(offsetBytes + stripes * sb, 0));
    for (uint64_t s = 0; s < stripes; ++s) {
        const auto pq = raid_layout::raid6Disks(s, n);
        std::vector<std::vector<uint8_t>> data(n - 2, std::vector<uint8_t>(sb));
        for (uint32_t j = 0; j + 2 < n; ++j) {
            const uint32_t disk = raid_layout::raid6DataDisk(s, j, n);
            fillPseudo(data[j], seed + static_cast<uint32_t>(s * 197u + j * 29u));
            std::memcpy(images[disk].data() + offsetBytes + s * sb, data[j].data(), sb);
        }
        for (size_t b = 0; b < sb; ++b) {
            uint8_t p = 0, q = 0;
            for (uint32_t j = 0; j + 2 < n; ++j) {
                p ^= data[j][b];
                q ^= raid6_math::gfMul(data[j][b], raid6_math::gfPow(static_cast<int>(j)));
            }
            images[pq.pDisk][offsetBytes + s * sb + b] = p;
            images[pq.qDisk][offsetBytes + s * sb + b] = q;
        }
    }
    return images;
}

std::vector<std::shared_ptr<DiskReader>> openMembers(std::vector<std::vector<uint8_t>> images) {
    std::vector<std::shared_ptr<DiskReader>> v;
    v.reserve(images.size());
    for (auto& img : images) {
        auto r = std::make_shared<DiskReader>();
        r->attachMemoryVolume(std::move(img), 512);
        v.push_back(std::move(r));
    }
    return v;
}

std::vector<DiskReader*> raw(const std::vector<std::shared_ptr<DiskReader>>& v) {
    std::vector<DiskReader*> out;
    out.reserve(v.size());
    for (auto& p : v) out.push_back(p.get());
    return out;
}

constexpr size_t kKiB = 1024;
constexpr size_t kMiB = 1024 * kKiB;

} // namespace

TEST(RaidDetect, DetectsRaid5BlockSizeAndConfidence) {
    // ponytail: fixture uses the largest candidate stripe (1MiB) because the
    // RAID5 P-syndrome alone cannot discriminate stripe size — any aligned
    // multiple/divisor scores 1.0 (see raid_detect.cpp ceiling note), and the
    // detector's tie-break emits the largest consistent candidate. RAID6
    // (Q-syndrome) does pin the size exactly — see DetectsRaid6NotRaid5.
    constexpr size_t kStripe = kMiB;
    auto members = openMembers(buildRaid5(3, 12, kStripe, 0, 0xBEEF));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_NE(g.level, RaidLevel::RAID0); // RAID0 is never emitted
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_GE(g.confidence, 0.90);
    EXPECT_LE(g.confidence, 1.0);
    EXPECT_FALSE(g.fsConfirmed);
}

TEST(RaidDetect, DetectsRaid5With1MiBControllerOffset) {
    // Garbage in the reservation region is what makes offset 0 fail: candidate
    // stripes straddling the reservation break the XOR identity, so only the
    // true 1MiB-offset candidates stay consistent.
    constexpr size_t kStripe = kMiB;
    auto members = openMembers(buildRaid5(3, 8, kStripe, kMiB, 0x5EED, 0xF00D));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 2048u);
    EXPECT_GE(g.confidence, 0.90);
}

TEST(RaidDetect, DetectsRaid6NotRaid5) {
    constexpr size_t kStripe = 64 * kKiB;
    auto members = openMembers(buildRaid6(4, 10, kStripe, 0, 0xC0FFEE));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID6); // RAID5 candidates must score below floor
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_GE(g.confidence, 0.90);
}

TEST(RaidDetect, DetectsRaid1Mirror) {
    std::vector<uint8_t> img(kMiB);
    fillPseudo(img, 0x1234);
    auto members = openMembers({img, img});
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID1);
    EXPECT_GE(g.confidence, 0.95);
}

TEST(RaidDetect, TinyRaid5DetectedWithPerfectScore) {
    // 3 stripes -> 3 samples < kMinSamples: accepted only because conf == 1.0.
    constexpr size_t kStripe = 64 * kKiB;
    auto members = openMembers(buildRaid5(3, 3, kStripe, 0, 0x77));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.confidence, 1.0);
}

TEST(RaidDetect, RejectsUnrelatedFourMembers) {
    std::vector<std::vector<uint8_t>> images(4, std::vector<uint8_t>(512 * kKiB));
    for (uint32_t i = 0; i < 4; ++i) fillPseudo(images[i], 0xA000 + i);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    EXPECT_FALSE(detectRaidGeometry(raw(members), g));
}

TEST(RaidDetect, TwoDisjointMembersDoNotImpersonateRaid1) {
    std::vector<std::vector<uint8_t>> images(2, std::vector<uint8_t>(kMiB));
    fillPseudo(images[0], 0x1111);
    fillPseudo(images[1], 0x2222); // disjoint content, not a mirror
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    EXPECT_FALSE(detectRaidGeometry(raw(members), g));
    EXPECT_LT(g.confidence, 0.95); // honest fail leaves floor unmet
}

TEST(RaidDetect, SingleMemberReturnsFalse) {
    auto members = openMembers({std::vector<uint8_t>(256 * kKiB, 0xAB)});
    RaidGeometry g;
    EXPECT_FALSE(detectRaidGeometry(raw(members), g));
}

// Paint a logical payload onto RAID5 members and rebuild P so XOR identity
// still holds at every aligned stripe (64K and 128K both stay parity-perfect).
void overlayRaid5Payload(std::vector<std::vector<uint8_t>>& images, size_t sb,
                         uint64_t offsetBytes, const std::vector<uint8_t>& payload,
                         raid_layout::Raid5Algorithm algo = raid_layout::Raid5Algorithm::LeftAsymmetric) {
    const uint32_t n = static_cast<uint32_t>(images.size());
    for (size_t off = 0; off < payload.size();) {
        const size_t logicalBlock = off / sb;
        const size_t inBlk = off % sb;
        const size_t si = logicalBlock / (n - 1);
        const size_t blk = logicalBlock % (n - 1);
        const uint32_t disk = raid_layout::raid5DataDisk(si, static_cast<uint32_t>(blk), n, algo);
        const size_t ncopy = std::min(payload.size() - off, sb - inBlk);
        const uint64_t dst = offsetBytes + si * sb + inBlk;
        std::memcpy(images[disk].data() + dst, payload.data() + off, ncopy);
        off += ncopy;
    }
    const uint64_t nStripes = (images[0].size() - offsetBytes) / sb;
    for (uint64_t s = 0; s < nStripes; ++s) {
        const uint32_t p = raid_layout::raid5ParityDisk(s, n, algo);
        std::vector<uint8_t> acc(sb, 0);
        for (uint32_t d = 0; d < n; ++d) {
            if (d == p) continue;
            const uint8_t* src = images[d].data() + offsetBytes + s * sb;
            for (size_t b = 0; b < sb; ++b) acc[b] ^= src[b];
        }
        std::memcpy(images[p].data() + offsetBytes + s * sb, acc.data(), sb);
    }
}

// Zero slack XOR-copies a FAT boot onto the parity disk. Fill unused clusters
// and the MBR gap so only the true stripe reconstructs 55AA+"FAT" at LBA 2048.
std::vector<uint8_t> noiseFilledMbrFat(uint32_t seed, uint32_t partStart) {
    auto fat = testfix::buildFat16Volume();
    constexpr uint32_t ss = 512;
    constexpr uint32_t dataStart = 34; // reserved 1 + 2*16 FAT + 1 root
    for (size_t i = (dataStart + 1) * ss; i < fat.size(); ++i)
        fat[i] = static_cast<uint8_t>(mix32(static_cast<uint32_t>(i) ^ seed) >> 24);
    auto disk = testfix::buildMbrDiskWithFatPartition(fat, partStart);
    for (size_t i = ss; i < static_cast<size_t>(partStart) * ss; ++i)
        disk[i] = static_cast<uint8_t>(mix32(static_cast<uint32_t>(i) ^ (seed * 3u)) >> 24);
    return disk;
}

TEST(RaidDetect, FsSignaturePicks64KOverParityTied128K) {
    // Roadmap 3.6: XOR can tie 64K vs 256K; FAT at LBA 128 (one 64K data
    // block) only maps on the true stripe — LBA 2048 collided with 256K.
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripes = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid5(3, kStripes, kStripe, 0, 0xBEE5);
    overlayRaid5Payload(images, kStripe, 0, noiseFilledMbrFat(0xA11, kPartStart));
    auto members = openMembers(std::move(images));
    {
        VirtualRaid vr(RaidLevel::RAID5, members, kStripe, 0);
        auto boot = vr.read(0, 512);
        ASSERT_EQ(boot.size(), 512u);
        ASSERT_EQ(boot[510], 0x55);
        auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
        ASSERT_EQ(fat[510], 0x55);
        ASSERT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
    }
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_TRUE(g.fsConfirmed);
}

TEST(RaidDetect, FsSignaturePicksTrue128KNotDivisor64K) {
    constexpr size_t kStripe = 128 * kKiB;
    constexpr size_t kStripes = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid5(3, kStripes, kStripe, 0, 0xBEE6);
    overlayRaid5Payload(images, kStripe, 0, noiseFilledMbrFat(0xA12, kPartStart));
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_TRUE(g.fsConfirmed);
}

TEST(RaidDetect, AllZeroMembersTieBreakToLeastRedundantAssumption) {
    // Degenerate: zeros satisfy every candidate. Documented tie order keeps
    // the first (mirror) instead of inventing parity geometry.
    auto members = openMembers({std::vector<uint8_t>(256 * kKiB, 0),
                                std::vector<uint8_t>(256 * kKiB, 0),
                                std::vector<uint8_t>(256 * kKiB, 0)});
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID1);
}

TEST(RaidDetect, DetectsRaid6FromEvidenceFiles) {
    constexpr size_t kStripe = 64 * kKiB;
    auto images = buildRaid6(4, 10, kStripe, 0, 0xC0FFEE);
    std::vector<std::string> paths;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto p = bytebackTestTemp("bb_raid6_m" + std::to_string(i), ".img");
        std::filesystem::remove(p);
        {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(images[i].data()),
                    static_cast<std::streamsize>(images[i].size()));
        }
        paths.push_back(p.u8string());
    }
    RaidGeometry g;
    ASSERT_TRUE(detectRaidFromEvidencePaths(paths, g));
    EXPECT_EQ(g.level, RaidLevel::RAID6);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_GE(g.confidence, 0.90);
    for (const auto& p : paths) std::filesystem::remove(p);
}

TEST(RaidDetect, EvidencePathsRejectDevice) {
    RaidGeometry g;
    EXPECT_FALSE(detectRaidFromEvidencePaths(
        {"\\\\.\\PhysicalDrive0", "C:\\cases\\disk.img"}, g));
}

TEST(RaidDetect, DetectsRaid1FromEvidenceFat16Copies) {
    const auto img = testfix::buildFat16Volume();
    const auto p0 = bytebackTestTemp("bb_raid1_fat_a", ".img");
    const auto p1 = bytebackTestTemp("bb_raid1_fat_b", ".img");
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
    {
        std::ofstream a(p0, std::ios::binary | std::ios::trunc);
        a.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
        std::ofstream b(p1, std::ios::binary | std::ios::trunc);
        b.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }
    RaidGeometry g;
    ASSERT_TRUE(detectRaidFromEvidencePaths({p0.u8string(), p1.u8string()}, g));
    EXPECT_EQ(g.level, RaidLevel::RAID1);
    EXPECT_GE(g.confidence, 0.95);
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
}

std::vector<std::vector<uint8_t>> buildRaid10(uint32_t n, size_t stripesPerPair, size_t sb,
                                              uint64_t offsetBytes, uint32_t seed) {
    const uint32_t pairs = n / 2;
    const size_t logicalBlocks = stripesPerPair * pairs;
    std::vector<std::vector<uint8_t>> images(n, std::vector<uint8_t>(offsetBytes + stripesPerPair * sb, 0));
    for (uint64_t b = 0; b < logicalBlocks; ++b) {
        std::vector<uint8_t> data(sb);
        fillPseudo(data, seed + static_cast<uint32_t>(b * 41u));
        const uint32_t a = raid_layout::raid10MemberA(b, n);
        const uint32_t c = raid_layout::raid10MemberB(b, n);
        const uint64_t off = offsetBytes + (b / pairs) * sb;
        std::memcpy(images[a].data() + off, data.data(), sb);
        std::memcpy(images[c].data() + off, data.data(), sb);
    }
    return images;
}

void overlayRaid10Payload(std::vector<std::vector<uint8_t>>& images, size_t sb,
                          uint64_t offsetBytes, const std::vector<uint8_t>& payload) {
    const uint32_t n = static_cast<uint32_t>(images.size());
    const uint32_t pairs = n / 2;
    for (size_t off = 0; off < payload.size();) {
        const uint64_t b = off / sb;
        const size_t inBlk = off % sb;
        const uint32_t a = raid_layout::raid10MemberA(b, n);
        const uint32_t c = raid_layout::raid10MemberB(b, n);
        const size_t ncopy = std::min(payload.size() - off, sb - inBlk);
        const uint64_t dst = offsetBytes + (b / pairs) * sb + inBlk;
        std::memcpy(images[a].data() + dst, payload.data() + off, ncopy);
        std::memcpy(images[c].data() + dst, payload.data() + off, ncopy);
        off += ncopy;
    }
}

std::vector<std::vector<uint8_t>> buildRaid1e(uint32_t n, size_t nData, size_t sb, uint32_t seed) {
    uint64_t maxRow = 0;
    for (uint64_t d = 0; d < nData; ++d) {
        for (uint32_t c = 0; c < 2; ++c)
            maxRow = std::max(maxRow, raid_layout::raid1eCopy(d, c, n).row);
    }
    std::vector<std::vector<uint8_t>> images(n, std::vector<uint8_t>((maxRow + 1) * sb, 0));
    for (uint64_t d = 0; d < nData; ++d) {
        std::vector<uint8_t> data(sb);
        fillPseudo(data, seed + static_cast<uint32_t>(d * 41u));
        for (uint32_t c = 0; c < 2; ++c) {
            const auto loc = raid_layout::raid1eCopy(d, c, n);
            std::memcpy(images[loc.disk].data() + loc.row * sb, data.data(), sb);
        }
    }
    return images;
}

void overlayRaid1ePayload(std::vector<std::vector<uint8_t>>& images, size_t sb,
                          const std::vector<uint8_t>& payload) {
    const uint32_t n = static_cast<uint32_t>(images.size());
    for (size_t off = 0; off < payload.size();) {
        const uint64_t d = off / sb;
        const size_t inBlk = off % sb;
        const size_t ncopy = std::min(payload.size() - off, sb - inBlk);
        for (uint32_t c = 0; c < 2; ++c) {
            const auto loc = raid_layout::raid1eCopy(d, c, n);
            std::memcpy(images[loc.disk].data() + loc.row * sb + inBlk, payload.data() + off, ncopy);
        }
        off += ncopy;
    }
}

TEST(RaidDetect, DetectsRaid10StripeViaFsSignature) {
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripesPerPair = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid10(4, kStripesPerPair, kStripe, 0, 0x10D0);
    overlayRaid10Payload(images, kStripe, 0, noiseFilledMbrFat(0xA13, kPartStart));
    auto members = openMembers(std::move(images));
    {
        VirtualRaid vr(RaidLevel::RAID10, members, kStripe, 0);
        auto boot = vr.read(0, 512);
        ASSERT_EQ(boot.size(), 512u);
        ASSERT_EQ(boot[510], 0x55);
        auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
        ASSERT_EQ(fat[510], 0x55);
        ASSERT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
    }
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID10);
    EXPECT_NE(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_TRUE(g.fsConfirmed);
}

TEST(RaidDetect, FsSignaturePicksRaid10_128KNotDivisor64K) {
    constexpr size_t kStripe = 128 * kKiB;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid10(4, 24, kStripe, 0, 0x10D1);
    overlayRaid10Payload(images, kStripe, 0, noiseFilledMbrFat(0xA14, kPartStart));
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID10);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_TRUE(g.fsConfirmed);
}

TEST(RaidDetect, Raid10PairsWithoutFsDoNotImpersonateRaid5) {
    // Adjacent mirrors with no FS magic: RAID5 XOR is vacuously true on
    // A,A,B,B. Honest fail — a guessed stripe silently interleaves pairs.
    auto images = buildRaid10(4, 16, 64 * kKiB, 0, 0xB10D);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    EXPECT_FALSE(detectRaidGeometry(raw(members), g));
    EXPECT_NE(g.level, RaidLevel::RAID5);
}

TEST(RaidDetect, DetectsRaid1eViaFsSignature) {
    constexpr size_t kStripe = 64 * kKiB;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto payload = noiseFilledMbrFat(0xA1E, kPartStart);
    const size_t nData = std::max<size_t>(32, (payload.size() + kStripe - 1) / kStripe);
    auto images = buildRaid1e(3, nData, kStripe, 0x11E0);
    overlayRaid1ePayload(images, kStripe, payload);
    auto members = openMembers(std::move(images));
    {
        VirtualRaid vr(RaidLevel::RAID1E, members, kStripe, 0);
        auto boot = vr.read(0, 512);
        ASSERT_EQ(boot.size(), 512u);
        ASSERT_EQ(boot[510], 0x55);
        auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
        ASSERT_EQ(fat[510], 0x55);
        ASSERT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
    }
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID1E);
    EXPECT_NE(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_TRUE(g.fsConfirmed);
}

TEST(RaidDetect, Raid1eWithoutFsDoesNotImpersonateRaid5) {
    auto images = buildRaid1e(3, 32, 64 * kKiB, 0xB11E);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    EXPECT_FALSE(detectRaidGeometry(raw(members), g));
    EXPECT_NE(g.level, RaidLevel::RAID5);
}

TEST(RaidDetect, RotatedRaid5MembersStillDetect) {
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripes = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid5(3, kStripes, kStripe, 0, 0xB0E7);
    overlayRaid5Payload(images, kStripe, 0, noiseFilledMbrFat(0xA16, kPartStart));
    auto members = openMembers(std::move(images));
    std::vector<std::shared_ptr<DiskReader>> rotHold = {members[1], members[2], members[0]};
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(rotHold), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_TRUE(g.fsConfirmed);
    ASSERT_EQ(g.memberOrder.size(), 3u);
    std::vector<std::shared_ptr<DiskReader>> ordered;
    ordered.reserve(g.memberOrder.size());
    for (size_t i : g.memberOrder) ordered.push_back(rotHold[i]);
    VirtualRaid vr(g.level, ordered, g.blockSize, 0, g.raid5Algorithm);
    auto boot = vr.read(0, 512);
    ASSERT_EQ(boot.size(), 512u);
    EXPECT_EQ(boot[510], 0x55);
    auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
    ASSERT_EQ(fat.size(), 512u);
    EXPECT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
}

TEST(RaidDetect, PairSwappedRaid10StillDetects) {
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripesPerPair = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>(kStripe / 512);
    auto images = buildRaid10(4, kStripesPerPair, kStripe, 0, 0x10D2);
    overlayRaid10Payload(images, kStripe, 0, noiseFilledMbrFat(0xA15, kPartStart));
    auto members = openMembers(std::move(images));
    std::vector<DiskReader*> rot = {
        members[2].get(), members[3].get(), members[0].get(), members[1].get()};
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(rot, g));
    EXPECT_EQ(g.level, RaidLevel::RAID10);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_TRUE(g.fsConfirmed);
    ASSERT_EQ(g.memberOrder.size(), 4u);
    EXPECT_EQ(g.memberOrder[0], 2u);
    EXPECT_EQ(g.memberOrder[1], 3u);
    EXPECT_EQ(g.memberOrder[2], 0u);
    EXPECT_EQ(g.memberOrder[3], 1u);
}

TEST(RaidDetect, FsSignaturePicksLeftSymmetricNotAsymmetric) {
    // N=3 stripe 0 is identical for left-asym/left-sym. FAT at two data
    // blocks (stripe 1) is where mdadm left-symmetric diverges.
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripes = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>((2 * kStripe) / 512);
    auto images = buildRaid5(3, kStripes, kStripe, 0, 0x15A1);
    overlayRaid5Payload(images, kStripe, 0, noiseFilledMbrFat(0xA17, kPartStart),
                        raid_layout::Raid5Algorithm::LeftSymmetric);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_TRUE(g.fsConfirmed);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftSymmetric);
    VirtualRaid vr(g.level, members, g.blockSize, 0, g.raid5Algorithm);
    auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
    ASSERT_EQ(fat.size(), 512u);
    EXPECT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
}

TEST(RaidDetect, FsSignaturePicksRightAsymmetric) {
    constexpr size_t kStripe = 64 * kKiB;
    constexpr size_t kStripes = 32;
    constexpr uint32_t kPartStart = static_cast<uint32_t>((2 * kStripe) / 512);
    auto images = buildRaid5(3, kStripes, kStripe, 0, 0x51A5);
    overlayRaid5Payload(images, kStripe, 0, noiseFilledMbrFat(0xA18, kPartStart),
                        raid_layout::Raid5Algorithm::RightAsymmetric);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, kStripe);
    EXPECT_TRUE(g.fsConfirmed);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::RightAsymmetric);
    VirtualRaid vr(g.level, members, g.blockSize, 0, g.raid5Algorithm);
    auto fat = vr.read(static_cast<uint64_t>(kPartStart) * 512, 512);
    ASSERT_EQ(fat.size(), 512u);
    EXPECT_EQ(std::memcmp(fat.data() + 0x36, "FAT", 3), 0);
}

namespace {

constexpr uint32_t kMdadmMagic = 0xA92B4EFCu;
constexpr size_t kSb1Size = 256;

void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void wr64(uint8_t* p, uint64_t v) {
    wr32(p, static_cast<uint32_t>(v));
    wr32(p + 4, static_cast<uint32_t>(v >> 32));
}

uint32_t mdadmSb1Csum(uint8_t* sb, uint32_t maxDev) {
    wr32(sb + 216, 0);
    uint64_t sum = 0;
    int size = static_cast<int>(kSb1Size + static_cast<size_t>(maxDev) * 2);
    const uint8_t* p = sb;
    for (; size >= 4; size -= 4, p += 4) {
        sum += static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    }
    if (size == 2) sum += static_cast<uint32_t>(p[0] | (p[1] << 8));
    return static_cast<uint32_t>(sum) + static_cast<uint32_t>(sum >> 32);
}

void plantMdadm1x(std::vector<uint8_t>& img, uint64_t sbByteOff, uint64_t superOffsetSectors,
                  uint32_t level, uint32_t kernelLayout, uint32_t chunkSectors,
                  uint32_t raidDisks, uint64_t dataOff, uint32_t devNumber, uint16_t role,
                  const uint8_t uuid[16]) {
    ASSERT_GE(img.size(), sbByteOff + kSb1Size + raidDisks * 2);
    uint8_t* sb = img.data() + sbByteOff;
    std::memset(sb, 0, kSb1Size + raidDisks * 2);
    wr32(sb, kMdadmMagic);
    wr32(sb + 4, 1);
    std::memcpy(sb + 16, uuid, 16);
    wr32(sb + 72, level);
    wr32(sb + 76, kernelLayout);
    wr32(sb + 88, chunkSectors);
    wr32(sb + 92, raidDisks);
    wr64(sb + 128, dataOff);
    wr64(sb + 144, superOffsetSectors);
    wr32(sb + 160, devNumber);
    wr32(sb + 220, raidDisks);
    wr16(sb + 256 + devNumber * 2, role);
    wr32(sb + 216, mdadmSb1Csum(sb, raidDisks));
}

void plantMdadm12(std::vector<uint8_t>& img, uint32_t level, uint32_t kernelLayout,
                  uint32_t chunkSectors, uint32_t raidDisks, uint64_t dataOff,
                  uint32_t devNumber, uint16_t role, const uint8_t uuid[16]) {
    plantMdadm1x(img, 4096, 8, level, kernelLayout, chunkSectors, raidDisks, dataOff,
                 devNumber, role, uuid);
}

constexpr char kImsmSig[] = "Intel Raid ISM Cfg Sig. ";

uint32_t imsmCsum(const uint8_t* mpb, uint32_t mpbSize) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 4 <= mpbSize; i += 4) {
        if (i == 32) continue;
        sum += static_cast<uint32_t>(mpb[i] | (mpb[i + 1] << 8) | (mpb[i + 2] << 16) |
                                     (mpb[i + 3] << 24));
    }
    return sum;
}

void plantImsmRaid1(std::vector<uint8_t>& img, uint32_t pba, uint16_t stripSectors) {
    ASSERT_GE(img.size(), 2048u);
    uint8_t* mpb = img.data() + img.size() - 1024;
    std::memset(mpb, 0, 512);
    std::memcpy(mpb, kImsmSig, 24);
    std::memcpy(mpb + 24, "1.1.00", 6);
    mpb[56] = 2;
    mpb[57] = 1;
    const uint32_t mpbSize = 480;
    wr32(mpb + 36, mpbSize);
    wr32(mpb + 40, 0xA11AA11A);
    uint8_t* map = mpb + 424;
    wr32(map + 0, pba);
    wr16(map + 12, stripSectors);
    map[15] = 1;
    map[16] = 2;
    map[17] = 1;
    wr32(map + 48, 0);
    wr32(map + 52, 1);
    wr32(mpb + 32, imsmCsum(mpb, mpbSize));
}

void plantImsmRaid5(std::vector<uint8_t>& img, uint32_t pba, uint16_t stripSectors) {
    ASSERT_GE(img.size(), 4096u);
    std::vector<uint8_t> logical(532, 0);
    std::memcpy(logical.data(), kImsmSig, 24);
    std::memcpy(logical.data() + 24, "1.2.02", 6);
    logical[56] = 3;
    logical[57] = 1;
    wr32(logical.data() + 36, 532);
    wr32(logical.data() + 40, 0x5E5E5E5E);
    uint8_t* map = logical.data() + 472;
    wr32(map + 0, pba);
    wr16(map + 12, stripSectors);
    map[15] = 5;
    map[16] = 3;
    map[17] = 1;
    wr32(map + 48, 0);
    wr32(map + 52, 1);
    wr32(map + 56, 2);
    wr32(logical.data() + 32, imsmCsum(logical.data(), 532));
    std::memcpy(img.data() + img.size() - 1024, logical.data(), 512);
    std::memcpy(img.data() + img.size() - 1536, logical.data() + 512, 20);
}

constexpr uint32_t kDdfMagic = 0xDE11DE11u;
constexpr uint32_t kDdfVdMagic = 0xEEEEEEEEu;

void wrBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void wrBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
void wrBe64(uint8_t* p, uint64_t v) {
    wrBe32(p, static_cast<uint32_t>(v >> 32));
    wrBe32(p + 4, static_cast<uint32_t>(v));
}

void sealDdfCrc(uint8_t* sec) {
    wrBe32(sec + 4, 0xFFFFFFFFu);
    wrBe32(sec + 4, carver::crc32(sec, 512));
}

void fillDdfHeader(uint8_t* sec, uint64_t primaryLba, uint32_t cfgOff, uint32_t cfgLen, uint8_t type) {
    std::memset(sec, 0xFF, 512);
    wrBe32(sec, kDdfMagic);
    std::memset(sec + 8, 0x11, 24);
    std::memcpy(sec + 32, "02.00.00", 8);
    wrBe64(sec + 96, primaryLba);
    wrBe64(sec + 104, 0);
    sec[112] = type;
    wrBe16(sec + 116, 16);
    wrBe16(sec + 118, 16);
    wrBe16(sec + 122, 1);
    wrBe32(sec + 188, cfgOff);
    wrBe32(sec + 192, cfgLen);
    sealDdfCrc(sec);
}

void fillDdfVd(uint8_t* sec, uint16_t nmem, uint8_t prl, uint8_t rlq, uint8_t chunkShift) {
    std::memset(sec, 0xFF, 512);
    wrBe32(sec, kDdfVdMagic);
    std::memset(sec + 8, 0x22, 24);
    wrBe16(sec + 64, nmem);
    sec[66] = chunkShift;
    sec[67] = prl;
    sec[68] = rlq;
    sec[69] = 1;
    sealDdfCrc(sec);
}

void plantDdf(std::vector<uint8_t>& img, uint16_t nmem, uint8_t prl, uint8_t rlq, uint8_t chunkShift) {
    ASSERT_GE(img.size(), 4u * 512u);
    const uint64_t secs = img.size() / 512;
    const uint64_t primary = secs - 3;
    fillDdfHeader(img.data() + primary * 512, primary, 1, 1, 1);
    fillDdfVd(img.data() + (primary + 1) * 512, nmem, prl, rlq, chunkShift);
    fillDdfHeader(img.data() + (secs - 1) * 512, primary, 1, 1, 0);
}

} // namespace

TEST(RaidDetect, ParseMdadm12ReadsLeftSymmetricLayout) {
    std::vector<uint8_t> buf(kSb1Size + 8, 0);
    const uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    wr32(buf.data(), kMdadmMagic);
    wr32(buf.data() + 4, 1);
    std::memcpy(buf.data() + 16, uuid, 16);
    wr32(buf.data() + 72, 5);
    wr32(buf.data() + 76, 2); // ALGORITHM_LEFT_SYMMETRIC
    wr32(buf.data() + 88, 128);
    wr32(buf.data() + 92, 3);
    wr64(buf.data() + 128, 16);
    wr64(buf.data() + 144, 8);
    wr32(buf.data() + 160, 1);
    wr32(buf.data() + 220, 3);
    wr16(buf.data() + 256 + 2, 1);
    wr32(buf.data() + 216, mdadmSb1Csum(buf.data(), 3));

    MdadmSuper1 sb;
    ASSERT_TRUE(parseMdadmSuper1(buf.data(), buf.size(), sb));
    EXPECT_EQ(sb.level, 5);
    EXPECT_EQ(sb.kernelLayout, 2u);
    EXPECT_EQ(sb.chunkSectors, 128u);
    EXPECT_EQ(sb.raidDisks, 3u);
    EXPECT_EQ(sb.dataOffsetSectors, 16u);
    EXPECT_EQ(sb.devNumber, 1u);
    EXPECT_EQ(sb.role, 1u);
    EXPECT_EQ(raid5AlgoFromMdadmLayout(sb.kernelLayout), raid_layout::Raid5Algorithm::LeftSymmetric);
}

TEST(RaidDetect, ParseMdadm12RejectsBadCsum) {
    std::vector<uint8_t> buf(kSb1Size + 6, 0);
    wr32(buf.data(), kMdadmMagic);
    wr32(buf.data() + 4, 1);
    wr32(buf.data() + 72, 5);
    wr32(buf.data() + 88, 128);
    wr32(buf.data() + 92, 3);
    wr32(buf.data() + 220, 3);
    wr32(buf.data() + 216, mdadmSb1Csum(buf.data(), 3));
    buf[72] ^= 0x01;
    MdadmSuper1 sb;
    EXPECT_FALSE(parseMdadmSuper1(buf.data(), buf.size(), sb));
}

TEST(RaidDetect, DetectsRaid5FromMdadmWhenDataOffsetNotInControllerSet) {
    // XOR path only tries offsets {0, 2048}. mdadm 1.2 data_offset=16 must win.
    constexpr size_t kMember = 512 * kKiB;
    const uint8_t uuid[16] = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 3; ++i)
        plantMdadm12(images[i], 5, 2, 128, 3, 16, i, static_cast<uint16_t>(i), uuid);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_EQ(g.dataOffsetSectors, 16u);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftSymmetric);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, MdadmRolesReorderMembers) {
    constexpr size_t kMember = 512 * kKiB;
    const uint8_t uuid[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    plantMdadm12(images[0], 5, 2, 128, 3, 16, 0, 1, uuid);
    plantMdadm12(images[1], 5, 2, 128, 3, 16, 1, 2, uuid);
    plantMdadm12(images[2], 5, 2, 128, 3, 16, 2, 0, uuid);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    ASSERT_EQ(g.memberOrder.size(), 3u);
    EXPECT_EQ(g.memberOrder[0], 2u);
    EXPECT_EQ(g.memberOrder[1], 0u);
    EXPECT_EQ(g.memberOrder[2], 1u);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftSymmetric);
}

TEST(RaidDetect, DetectsRaid5FromMdadm10AtEnd) {
    // super-1.0: 8KiB from end, 4KiB aligned (mdadm super1.c case 0).
    constexpr size_t kMember = 512 * kKiB;
    const uint64_t sectors = kMember / 512;
    uint64_t sbSec = (sectors - 16) & ~uint64_t{7};
    const uint8_t uuid[16] = {0x10, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 3; ++i)
        plantMdadm1x(images[i], sbSec * 512, sbSec, 5, 2, 128, 3, 0, i, static_cast<uint16_t>(i),
                     uuid);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_EQ(g.dataOffsetSectors, 0u);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftSymmetric);
}

TEST(RaidDetect, ParseImsmAnchorReadsRaid1) {
    std::vector<uint8_t> img(2048, 0);
    plantImsmRaid1(img, 16, 128);
    const uint8_t* mpb = img.data() + img.size() - 1024;
    ImsmSuper sb;
    ASSERT_TRUE(parseImsmSuper(mpb, 512, sb));
    EXPECT_EQ(sb.level, 1);
    EXPECT_EQ(sb.numMembers, 2u);
    EXPECT_EQ(sb.dataOffsetSectors, 16u);
    EXPECT_EQ(sb.chunkSectors, 128u);
    ASSERT_EQ(sb.diskOrd.size(), 2u);
    EXPECT_EQ(sb.diskOrd[0], 0u);
    EXPECT_EQ(sb.diskOrd[1], 1u);
}

TEST(RaidDetect, ParseImsmRejectsBadCsum) {
    std::vector<uint8_t> img(2048, 0);
    plantImsmRaid1(img, 16, 128);
    uint8_t* mpb = img.data() + img.size() - 1024;
    mpb[56] ^= 0x01;
    ImsmSuper sb;
    EXPECT_FALSE(parseImsmSuper(mpb, 512, sb));
}

TEST(RaidDetect, DetectsRaid1FromImsmWhenMembersDiverge) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(2, std::vector<uint8_t>(kMember, 0));
    fillPseudo(images[0], 0x1111);
    fillPseudo(images[1], 0x2222);
    plantImsmRaid1(images[0], 16, 128);
    plantImsmRaid1(images[1], 16, 128);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID1);
    EXPECT_EQ(g.dataOffsetSectors, 16u);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, DetectsRaid5FromImsmExtendedMpb) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 3; ++i) fillPseudo(images[i], 0x5000 + i);
    for (auto& img : images) plantImsmRaid5(img, 16, 128);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_EQ(g.dataOffsetSectors, 16u);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftAsymmetric);
}

TEST(RaidDetect, ParseDdfAnchorReadsMagic) {
    std::vector<uint8_t> buf(512, 0xFF);
    fillDdfHeader(buf.data(), 100, 2, 1, 0);
    DdfHeader h;
    ASSERT_TRUE(parseDdfHeader(buf.data(), buf.size(), h));
    EXPECT_EQ(h.primaryLba, 100u);
    EXPECT_EQ(h.configSectionOffset, 2u);
    EXPECT_EQ(h.configSectionLength, 1u);
    EXPECT_EQ(h.type, 0);
}

TEST(RaidDetect, ParseDdfRejectsBadCrc) {
    std::vector<uint8_t> buf(512, 0xFF);
    fillDdfHeader(buf.data(), 100, 2, 1, 0);
    buf[20] ^= 0x01;
    DdfHeader h;
    EXPECT_FALSE(parseDdfHeader(buf.data(), buf.size(), h));
}

TEST(RaidDetect, DetectsRaid1FromDdfWhenMembersDiverge) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(2, std::vector<uint8_t>(kMember, 0));
    fillPseudo(images[0], 0xDDF1);
    fillPseudo(images[1], 0xDDF2);
    plantDdf(images[0], 2, 1, 0, 7);
    plantDdf(images[1], 2, 1, 0, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID1);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, DetectsRaid5FromDdfLeftAsym) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 3; ++i) fillPseudo(images[i], 0xDD50 + i);
    for (auto& img : images) plantDdf(img, 3, 5, 2, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::LeftAsymmetric);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, DetectsRaid5FromDdfRightAsym) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(3, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 3; ++i) fillPseudo(images[i], 0xDD60 + i);
    for (auto& img : images) plantDdf(img, 3, 5, 0, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID5);
    EXPECT_EQ(g.raid5Algorithm, raid_layout::Raid5Algorithm::RightAsymmetric);
}

TEST(RaidDetect, DetectsRaid0FromDdf) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(2, std::vector<uint8_t>(kMember, 0));
    fillPseudo(images[0], 0xDD00);
    fillPseudo(images[1], 0xDD01);
    plantDdf(images[0], 2, 0, 0, 7);
    plantDdf(images[1], 2, 0, 0, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID0);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, DetectsRaid6FromDdf) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(4, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 4; ++i) fillPseudo(images[i], 0xDD60 + i);
    for (auto& img : images) plantDdf(img, 4, 6, 0, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID6);
    EXPECT_EQ(g.blockSize, 64 * kKiB);
    EXPECT_GE(g.confidence, 0.99);
}

TEST(RaidDetect, DetectsRaid10FromDdf) {
    constexpr size_t kMember = 512 * kKiB;
    std::vector<std::vector<uint8_t>> images(4, std::vector<uint8_t>(kMember, 0));
    for (uint32_t i = 0; i < 4; ++i) fillPseudo(images[i], 0xDDA0 + i);
    for (auto& img : images) plantDdf(img, 4, 0x11, 0, 7);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::RAID10);
}

TEST(RaidDetect, ParseMdadm12ReadsLinear) {
    std::vector<uint8_t> buf(kSb1Size + 6, 0);
    const uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    wr32(buf.data(), kMdadmMagic);
    wr32(buf.data() + 4, 1);
    std::memcpy(buf.data() + 16, uuid, 16);
    wr32(buf.data() + 72, 0xFFFFFFFFu); // LEVEL_LINEAR
    wr32(buf.data() + 88, 0);           // chunk unused
    wr32(buf.data() + 92, 2);
    wr64(buf.data() + 128, 16);
    wr64(buf.data() + 144, 8);
    wr32(buf.data() + 160, 0);
    wr32(buf.data() + 220, 2);
    wr16(buf.data() + 256, 0);
    wr32(buf.data() + 216, mdadmSb1Csum(buf.data(), 2));
    MdadmSuper1 sb;
    ASSERT_TRUE(parseMdadmSuper1(buf.data(), buf.size(), sb));
    EXPECT_EQ(sb.level, -1);
    EXPECT_EQ(sb.chunkSectors, 0u);
    EXPECT_EQ(sb.raidDisks, 2u);
    EXPECT_EQ(sb.dataOffsetSectors, 16u);
}

TEST(RaidDetect, DetectsLinearFromMdadm) {
    constexpr size_t kMember = 512 * kKiB;
    const uint8_t uuid[16] = {0x11, 0x22, 0x33, 0x44, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::vector<std::vector<uint8_t>> images(2, std::vector<uint8_t>(kMember, 0));
    plantMdadm12(images[0], 0xFFFFFFFFu, 0, 0, 2, 16, 0, 0, uuid);
    plantMdadm12(images[1], 0xFFFFFFFFu, 0, 0, 2, 16, 1, 1, uuid);
    auto members = openMembers(std::move(images));
    RaidGeometry g;
    ASSERT_TRUE(detectRaidGeometry(raw(members), g));
    EXPECT_EQ(g.level, RaidLevel::JBOD);
    EXPECT_EQ(g.blockSize, 0u);
    EXPECT_EQ(g.dataOffsetSectors, 16u);
    EXPECT_GE(g.confidence, 0.99);
}
