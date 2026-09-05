#include "fs/virtual_raid.h"
#include "fs/raid_layout.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

TEST(VirtualRaidIo, Raid0StripesAcrossMembers) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    constexpr size_t kBlock = 64 * 1024;
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, kBlock);

    auto chunkA = raid.read(0, kBlock);
    ASSERT_EQ(chunkA.size(), kBlock);
    EXPECT_EQ(chunkA[0], static_cast<uint8_t>('A'));
    EXPECT_EQ(chunkA[kBlock - 1], static_cast<uint8_t>('A'));

    auto chunkB = raid.read(kBlock, kBlock);
    ASSERT_EQ(chunkB.size(), kBlock);
    EXPECT_EQ(chunkB[0], static_cast<uint8_t>('B'));
}

TEST(VirtualRaidIo, CapacityIsSumForRaid0) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 65536);
    EXPECT_EQ(raid.capacity(), d0.size() + d1.size());
}

TEST(VirtualRaidIo, Raid0FailedDiskZerosInsteadOfThrow) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    constexpr size_t kBlock = 64 * 1024;
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, kBlock);
    raid.fail_disk(0);
    EXPECT_FALSE(raid.is_disk_active(0));
    auto chunk = raid.read(0, 16);
    ASSERT_EQ(chunk.size(), 16u);
    for (uint8_t b : chunk) EXPECT_EQ(b, 0);
}

// AR2 vector gap: RAID6 P/Q stripe math and the GF(2^8) 2-disk reconstruction
// were only unit-tested in isolation. This drives the real VirtualRaid::read
// path: build a 4-disk RAID6 image (data + P + Q per stripe via the production
// raid_layout placement and raid6_math), then fail two disks and require
// byte-identical reads.
TEST(VirtualRaidIo, Raid6RecoversAfterTwoDataDiskFailures) {
    constexpr size_t N = 4;
    constexpr size_t BS = 4096;
    constexpr size_t STRIPES = 6;
    const size_t logicalSize = (N - 2) * STRIPES * BS;

    std::vector<std::vector<uint8_t>> images(N, std::vector<uint8_t>(STRIPES * BS, 0));
    std::vector<uint8_t> logical(logicalSize);

    for (uint64_t s = 0; s < STRIPES; ++s) {
        const auto pq = raid_layout::raid6Disks(s, static_cast<uint32_t>(N));
        std::vector<std::vector<uint8_t>> data(N - 2, std::vector<uint8_t>(BS));
        for (size_t j = 0; j < N - 2; ++j) {
            const uint32_t disk = raid_layout::raid6DataDisk(s, static_cast<uint32_t>(j), static_cast<uint32_t>(N));
            for (size_t b = 0; b < BS; ++b) {
                data[j][b] = static_cast<uint8_t>((s * 37 + j * 11 + b * 5) & 0xFF);
            }
            std::memcpy(images[disk].data() + s * BS, data[j].data(), BS);
            std::memcpy(logical.data() + (s * (N - 2) + j) * BS, data[j].data(), BS);
        }
        // P = XOR of data blocks; Q = XOR of gfMul(data, gfPow(data slot)).
        for (size_t b = 0; b < BS; ++b) {
            uint8_t p = 0, q = 0;
            for (size_t j = 0; j < N - 2; ++j) {
                p ^= data[j][b];
                q ^= raid6_math::gfMul(data[j][b], raid6_math::gfPow(static_cast<int>(j)));
            }
            images[pq.pDisk][s * BS + b] = p;
            images[pq.qDisk][s * BS + b] = q;
        }
    }

    VirtualRaid raid = VirtualRaid::fromImages(RaidLevel::RAID6, images, BS);
    auto healthy = raid.read(0, logicalSize);
    ASSERT_EQ(healthy.size(), logicalSize);
    EXPECT_EQ(healthy, logical);

    // Fail the two data-carrying disks of stripe 0 (P/Q stay live) and read
    // across all stripes: every stripe loses exactly one data block, so both
    // the single-failure XOR path and the 2-unknown GF solve must run.
    raid.fail_disk(0);
    raid.fail_disk(1);
    EXPECT_FALSE(raid.is_disk_active(0));
    EXPECT_FALSE(raid.is_disk_active(1));

    auto out = raid.read(0, logicalSize);
    ASSERT_EQ(out.size(), logicalSize);
    EXPECT_EQ(out, logical);

    // Unfailed pair must still read byte-identical (parity members untouched).
    raid.fail_disk(2);
    raid.fail_disk(3);
    EXPECT_ANY_THROW(raid.read(0, logicalSize));
}

// AR2 follow-up: P-disk-only failure must route through the Q-syndrome
// branch (D = gfDiv(Q', g^slot)) for the stripe(s) where the failed member
// is P — previously that threw. A single failed physical disk is P in some
// stripes, a data member in others, and Q in the rest: all three paths must
// produce byte-identical reads.
TEST(VirtualRaidIo, Raid6RecoversWhenPDiskFailsAlone) {
    constexpr size_t N = 4;
    constexpr size_t BS = 4096;
    constexpr size_t STRIPES = 6;
    const size_t logicalSize = (N - 2) * STRIPES * BS;

    std::vector<std::vector<uint8_t>> images(N, std::vector<uint8_t>(STRIPES * BS, 0));
    std::vector<uint8_t> logical(logicalSize);

    for (uint64_t s = 0; s < STRIPES; ++s) {
        const auto pq = raid_layout::raid6Disks(s, static_cast<uint32_t>(N));
        std::vector<std::vector<uint8_t>> data(N - 2, std::vector<uint8_t>(BS));
        for (size_t j = 0; j < N - 2; ++j) {
            const uint32_t disk = raid_layout::raid6DataDisk(s, static_cast<uint32_t>(j), static_cast<uint32_t>(N));
            for (size_t b = 0; b < BS; ++b) {
                data[j][b] = static_cast<uint8_t>((s * 41 + j * 13 + b * 3) & 0xFF);
            }
            std::memcpy(images[disk].data() + s * BS, data[j].data(), BS);
            std::memcpy(logical.data() + (s * (N - 2) + j) * BS, data[j].data(), BS);
        }
        for (size_t b = 0; b < BS; ++b) {
            uint8_t p = 0, q = 0;
            for (size_t j = 0; j < N - 2; ++j) {
                p ^= data[j][b];
                q ^= raid6_math::gfMul(data[j][b], raid6_math::gfPow(static_cast<int>(j)));
            }
            images[pq.pDisk][s * BS + b] = p;
            images[pq.qDisk][s * BS + b] = q;
        }
    }

    VirtualRaid raid = VirtualRaid::fromImages(RaidLevel::RAID6, images, BS);

    // One physical disk failed: across the rotation it plays P, data and Q.
    raid.fail_disk(0);
    EXPECT_FALSE(raid.is_disk_active(0));
    auto out = raid.read(0, logicalSize);
    ASSERT_EQ(out.size(), logicalSize);
    EXPECT_EQ(out, logical);
}
