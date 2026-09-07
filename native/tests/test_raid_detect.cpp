// RAID geometry autodetection tests — parity-consistency fixtures built with
// the production raid_layout / raid6_math so detection is scored against the
// exact layout VirtualRaid reads (mirrors test_virtual_raid_io.cpp fixtures).
#include "fs/raid_detect.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "byteback_io.h"
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
