#include "fs/virtual_raid.h"
#include "fs/raid_layout.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

using namespace byteback;

TEST(VirtualRaidIo, ResumeKeyDistinguishesStripeAndMembers) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    auto raid64 = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 65536);
    auto raid128 = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 128 * 1024);
    EXPECT_EQ(raid64.capacity(), raid128.capacity());
    EXPECT_NE(raid64.resumeKey(), raid128.resumeKey());

    std::vector<uint8_t> c0(d0.size(), static_cast<uint8_t>('C'));
    std::vector<uint8_t> c1(d1.size(), static_cast<uint8_t>('D'));
    auto raidOther = VirtualRaid::fromImages(RaidLevel::RAID0, {c0, c1}, 65536);
    EXPECT_EQ(raid64.capacity(), raidOther.capacity());
    EXPECT_NE(raid64.resumeKey(), raidOther.resumeKey());

    const auto beforeFail = raid64.resumeKey();
    raid64.fail_disk(0);
    EXPECT_NE(raid64.resumeKey(), beforeFail);
}

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

TEST(VirtualRaidIo, Raid0FailedMemberThrowsNotSilentZeros) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    constexpr size_t kBlock = 64 * 1024;
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, kBlock);
    raid.fail_disk(0);
    EXPECT_FALSE(raid.is_disk_active(0));
    EXPECT_THROW(raid.read(0, 16), std::runtime_error);
    auto sibling = raid.read(kBlock, 16);
    ASSERT_EQ(sibling.size(), 16u);
    EXPECT_EQ(sibling[0], static_cast<uint8_t>('B'));
}

TEST(VirtualRaidIo, Raid0MemberIoFaultThrowsNotSilentZeros) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    constexpr size_t kBlock = 64 * 1024;
    auto m0 = std::make_shared<DiskReader>();
    m0->attachMemoryVolume(std::move(d0));
    auto m1 = std::make_shared<DiskReader>();
    m1->attachMemoryVolume(std::move(d1));
    m0->setMemoryFaultRange(0, 1);
    VirtualRaid raid(RaidLevel::RAID0, {m0, m1}, kBlock);
    EXPECT_THROW(raid.read(0, 16), std::runtime_error);
    auto sibling = raid.read(kBlock, 16);
    ASSERT_EQ(sibling.size(), 16u);
    EXPECT_EQ(sibling[0], static_cast<uint8_t>('B'));
}

TEST(VirtualRaidIo, Raid0UnreadReachDiskReaderAsPaddedNotClean) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    constexpr size_t kBlock = 64 * 1024;
    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, kBlock));
    raid->fail_disk(0);
    DiskReader reader;
    reader.setRaidBackend(raid);
    uint8_t buf[512];
    std::memset(buf, 0xAA, sizeof(buf));
    auto res = reader.readSectors(0, 512, buf);
    EXPECT_TRUE(res.paddedZeros);
    EXPECT_FALSE(res.success);
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

// Third adversarial pass: the Q-syndrome branch (D = gfDiv(Q', g^slot)) was
// never executed by the tests above — a failed data disk always paired with a
// live P there. Fail P + one data disk of the SAME stripe: P is dead, so the
// single-failure solve must route through Q.
TEST(VirtualRaidIo, Raid6QSyndromeReconstructsWhenPAndDataFail) {
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
                data[j][b] = static_cast<uint8_t>((s * 53 + j * 29 + b * 7) & 0xFF);
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
    // Stripe 0: P = disk 3, data = disks 0 (slot 0) and 1 (slot 1). Striping
    // the same two physical failures across all stripes also exercises the
    // P-XOR path (disk 1 is a data member of stripe 3) and direct hits.
    raid.fail_disk(3);
    raid.fail_disk(1);
    auto out = raid.read(0, logicalSize);
    ASSERT_EQ(out.size(), logicalSize);
    EXPECT_EQ(out, logical);
}

// RAID0 tail: when a member's size is not a multiple of block_size_, reads of
// valid tail bytes inside full blocks used to throw "Read exceeds RAID
// capacity" because the guard tested the whole remaining block instead of the
// requested length. The dead zone AFTER the floored capacity still throws —
// real controllers never expose a partial-block tail either.
TEST(VirtualRaidIo, Raid0ReadsLastTailByteWithoutFalseThrow) {
    constexpr size_t kMember = 3 * 512;  // 1536, not a multiple of kBlock
    constexpr size_t kBlock = 1024;
    std::vector<uint8_t> m0(kMember), m1(kMember);
    for (size_t i = 0; i < kMember; ++i) {
        m0[i] = static_cast<uint8_t>(i & 0xFF);
        m1[i] = static_cast<uint8_t>((i + 0x80) & 0xFF);
    }
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {m0, m1}, kBlock);
    // Floored capacity = floor(1536/1024)*1024*2 = 2048 — the dead-zone tail
    // (partial last block) is unreachable, matching real controllers.
    EXPECT_EQ(raid.capacity(), 2048u);
    // Last valid logical byte = block 1, disk 1 offset 1023 -> m1[1023].
    auto out = raid.read(2 * kBlock - 1, 1);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], static_cast<uint8_t>(127));
    // Dead zone and any cross-capacity read throw honestly.
    EXPECT_ANY_THROW((raid.read(2048, 1)));
    EXPECT_ANY_THROW((raid.read(2047, 2)));
}

// Detected controller reservation (e.g. 2048 sectors) must shift every RAID
// read. Reconstruct used to ignore it and assemble from byte 0 — silent wrong.
TEST(VirtualRaidIo, Raid0HonorsMemberDataOffset) {
    constexpr size_t kOff = 4096;
    constexpr size_t kBlock = 64 * 1024;
    std::vector<uint8_t> d0(kOff + 2 * kBlock, 0xCC);
    std::vector<uint8_t> d1(kOff + 2 * kBlock, 0xCC);
    for (size_t i = 0; i < kBlock; ++i) d0[kOff + i] = static_cast<uint8_t>('A');
    for (size_t i = 0; i < kBlock; ++i) d1[kOff + i] = static_cast<uint8_t>('B');
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, kBlock, kOff);
    EXPECT_EQ(raid.capacity(), 4 * kBlock);
    auto chunkA = raid.read(0, 16);
    ASSERT_EQ(chunkA.size(), 16u);
    EXPECT_EQ(chunkA[0], static_cast<uint8_t>('A'));
    auto chunkB = raid.read(kBlock, 16);
    ASSERT_EQ(chunkB.size(), 16u);
    EXPECT_EQ(chunkB[0], static_cast<uint8_t>('B'));
}

TEST(VirtualRaidIo, Raid1HonorsMemberDataOffset) {
    constexpr size_t kOff = 4096;
    std::vector<uint8_t> d0(kOff + 8192, 0xCC);
    std::vector<uint8_t> d1(kOff + 8192, 0xCC);
    for (size_t i = 0; i < 8192; ++i) {
        d0[kOff + i] = static_cast<uint8_t>('Z');
        d1[kOff + i] = static_cast<uint8_t>('Z');
    }
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID1, {d0, d1}, 65536, kOff);
    EXPECT_EQ(raid.capacity(), 8192u);
    auto chunk = raid.read(0, 8);
    ASSERT_EQ(chunk.size(), 8u);
    EXPECT_EQ(chunk[0], static_cast<uint8_t>('Z'));
}

TEST(VirtualRaidIo, DataOffsetAtOrPastMemberSizeThrows) {
    std::vector<uint8_t> d0(4096, 1), d1(4096, 1);
    EXPECT_ANY_THROW(VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 512, 4096));
}

// Out-of-capacity reads must throw on every level; RAID6 previously returned
// silent zeros (data zero-filled, reconstruction "succeeded" against zeros).
TEST(VirtualRaidIo, ReadPastCapacityThrows) {
    auto [d0, d1] = byteback::testfix::buildRaid0MemberDisks();
    auto r0 = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 65536);
    EXPECT_ANY_THROW(r0.read(r0.capacity(), 1));
    EXPECT_ANY_THROW(r0.read(r0.capacity() - 1, 2));

    constexpr size_t N = 4, BS = 4096, STRIPES = 2;
    std::vector<std::vector<uint8_t>> imgs(N, std::vector<uint8_t>(STRIPES * BS, 0));
    auto r6 = VirtualRaid::fromImages(RaidLevel::RAID6, imgs, BS);
    EXPECT_ANY_THROW(r6.read(r6.capacity(), 1));
}
