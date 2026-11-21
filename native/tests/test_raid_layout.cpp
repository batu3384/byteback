// Native unit tests for the pure RAID stripe-layout math (fs/raid_layout).
// Placement bugs are invisible to the GF(2^8) algebra tests: a wrong map
// reads the right offset from the wrong disk. These expectation tables pin
// the left-asymmetric RAID 5 rotation, the adjacent RAID 6 P/Q rotation,
// and the RAID 10 mirror-pair mapping (CA-005).
#include "fs/raid_layout.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <set>

using namespace byteback::raid_layout;

// ---- RAID 5 (left-asymmetric, N=4) ----
TEST(Raid5Layout, ParityRotationTable) {
    // Parity starts on the last disk and steps down one disk per stripe.
    EXPECT_EQ(raid5ParityDisk(0, 4), 3u);
    EXPECT_EQ(raid5ParityDisk(1, 4), 2u);
    EXPECT_EQ(raid5ParityDisk(2, 4), 1u);
    EXPECT_EQ(raid5ParityDisk(3, 4), 0u);
    EXPECT_EQ(raid5ParityDisk(4, 4), 3u); // period = N
    EXPECT_EQ(raid5ParityDisk(9, 4), 2u); // 9 % 4 = 1
}

TEST(Raid5Layout, DataDisksSkipParity) {
    // Stripe 0 (parity=3): data slots 0..2 -> disks 0,1,2.
    EXPECT_EQ(raid5DataDisk(0, 0, 4), 0u);
    EXPECT_EQ(raid5DataDisk(0, 1, 4), 1u);
    EXPECT_EQ(raid5DataDisk(0, 2, 4), 2u);
    // Stripe 1 (parity=2): slots 0..2 -> disks 0,1,3 (3 fills parity's gap).
    EXPECT_EQ(raid5DataDisk(1, 0, 4), 0u);
    EXPECT_EQ(raid5DataDisk(1, 1, 4), 1u);
    EXPECT_EQ(raid5DataDisk(1, 2, 4), 3u);
    // Stripe 3 (parity=0): slots -> disks 1,2,3.
    EXPECT_EQ(raid5DataDisk(3, 0, 4), 1u);
    EXPECT_EQ(raid5DataDisk(3, 2, 4), 3u);
}

TEST(Raid5Layout, EveryStripeUsesEachDiskOnce) {
    // Invariant: for each stripe, parity + (N-1) data slots is a permutation
    // of 0..N-1. Checked exhaustively for N=4 over a full rotation.
    for (uint64_t s = 0; s < 4; ++s) {
        std::set<uint32_t> used{raid5ParityDisk(s, 4)};
        for (uint32_t b = 0; b < 3; ++b) {
            uint32_t d = raid5DataDisk(s, b, 4);
            EXPECT_EQ(used.count(d), 0u) << "stripe " << s << " slot " << b;
            used.insert(d);
        }
        EXPECT_EQ(used.size(), 4u);
    }
}

// mdadm default (drivers/md/raid5.c algorithm_left_symmetric):
// dd_idx = (pd_idx + 1 + data_disk) % N. Stripe 0 matches asymmetric;
// stripe 1 starts after parity (disk 3), not at disk 0.
TEST(Raid5Layout, LeftSymmetricContinuesAfterParity) {
    EXPECT_EQ(raid5DataDisk(0, 0, 4, Raid5Algorithm::LeftSymmetric), 0u);
    EXPECT_EQ(raid5DataDisk(0, 1, 4, Raid5Algorithm::LeftSymmetric), 1u);
    EXPECT_EQ(raid5DataDisk(0, 2, 4, Raid5Algorithm::LeftSymmetric), 2u);
    EXPECT_EQ(raid5DataDisk(1, 0, 4, Raid5Algorithm::LeftSymmetric), 3u);
    EXPECT_EQ(raid5DataDisk(1, 1, 4, Raid5Algorithm::LeftSymmetric), 0u);
    EXPECT_EQ(raid5DataDisk(1, 2, 4, Raid5Algorithm::LeftSymmetric), 1u);
    EXPECT_NE(raid5DataDisk(1, 0, 4, Raid5Algorithm::LeftSymmetric),
              raid5DataDisk(1, 0, 4, Raid5Algorithm::LeftAsymmetric));
}

TEST(Raid5Layout, LeftSymmetricEveryStripeUsesEachDiskOnce) {
    for (uint64_t s = 0; s < 4; ++s) {
        std::set<uint32_t> used{raid5ParityDisk(s, 4)};
        for (uint32_t b = 0; b < 3; ++b) {
            uint32_t d = raid5DataDisk(s, b, 4, Raid5Algorithm::LeftSymmetric);
            EXPECT_EQ(used.count(d), 0u) << "stripe " << s << " slot " << b;
            used.insert(d);
        }
        EXPECT_EQ(used.size(), 4u);
    }
}

TEST(Raid5Layout, RightParityStartsAtDiskZero) {
    EXPECT_EQ(raid5ParityDisk(0, 4, Raid5Algorithm::RightAsymmetric), 0u);
    EXPECT_EQ(raid5ParityDisk(1, 4, Raid5Algorithm::RightAsymmetric), 1u);
    EXPECT_EQ(raid5ParityDisk(2, 4, Raid5Algorithm::RightAsymmetric), 2u);
    EXPECT_EQ(raid5ParityDisk(3, 4, Raid5Algorithm::RightAsymmetric), 3u);
    EXPECT_EQ(raid5ParityDisk(0, 4, Raid5Algorithm::RightSymmetric), 0u);
    EXPECT_NE(raid5ParityDisk(0, 4, Raid5Algorithm::RightAsymmetric),
              raid5ParityDisk(0, 4, Raid5Algorithm::LeftAsymmetric));
}

TEST(Raid5Layout, RightAsymmetricDataSkipsParityInDiskOrder) {
    EXPECT_EQ(raid5DataDisk(0, 0, 4, Raid5Algorithm::RightAsymmetric), 1u);
    EXPECT_EQ(raid5DataDisk(0, 1, 4, Raid5Algorithm::RightAsymmetric), 2u);
    EXPECT_EQ(raid5DataDisk(0, 2, 4, Raid5Algorithm::RightAsymmetric), 3u);
}

// Linux md ALGORITHM_RIGHT_SYMMETRIC (raid5.c raid5_compute_sector):
// pd = stripe % n, data = (pd + 1 + k) % n — data follows parity upward.
TEST(Raid5Layout, RightSymmetricDataFollowsParityLikeMd) {
    EXPECT_EQ(raid5DataDisk(0, 0, 4, Raid5Algorithm::RightSymmetric), 1u);
    EXPECT_EQ(raid5DataDisk(0, 1, 4, Raid5Algorithm::RightSymmetric), 2u);
    EXPECT_EQ(raid5DataDisk(0, 2, 4, Raid5Algorithm::RightSymmetric), 3u);
    EXPECT_EQ(raid5DataDisk(1, 0, 4, Raid5Algorithm::RightSymmetric), 2u);
    EXPECT_EQ(raid5DataDisk(1, 2, 4, Raid5Algorithm::RightSymmetric), 0u);
}

// ---- RAID 6 (Linux md layouts, P then Q, N=5) ----
TEST(Raid6Layout, EveryStripeUsesEachDiskOnce) {
    for (uint64_t s = 0; s < 5; ++s) {
        auto m = raid6Disks(s, 5);
        EXPECT_NE(m.pDisk, m.qDisk);
        std::set<uint32_t> used{m.pDisk, m.qDisk};
        for (uint32_t b = 0; b < 3; ++b) {
            uint32_t d = raid6DataDisk(s, b, 5);
            EXPECT_EQ(used.count(d), 0u) << "stripe " << s << " slot " << b;
            used.insert(d);
        }
        EXPECT_EQ(used.size(), 5u);
    }
}

// ---- RAID 10 (mirror pairs) ----
TEST(Raid10Layout, PairMapping) {
    // N=4 -> mirror pairs (0,1) and (2,3); each logical block stripes to one
    // pair (b % N/2), mirroring within it (virtual_raid read_raid10 agrees).
    EXPECT_EQ(raid10Pair(0, 4), 0u);
    EXPECT_EQ(raid10Pair(1, 4), 1u);
    EXPECT_EQ(raid10Pair(2, 4), 0u);
    EXPECT_EQ(raid10Pair(3, 4), 1u);
    EXPECT_EQ(raid10Pair(4, 4), 0u); // wraps

    EXPECT_EQ(raid10MemberA(2, 4), 0u);
    EXPECT_EQ(raid10MemberB(2, 4), 1u);
    EXPECT_EQ(raid10MemberA(3, 4), 2u);
    EXPECT_EQ(raid10MemberB(3, 4), 3u);
}

TEST(Raid1eLayout, NearThreeDiskTable) {
    auto a = raid1eCopy(0, 0, 3);
    EXPECT_EQ(a.disk, 0u);
    EXPECT_EQ(a.row, 0u);
    auto b = raid1eCopy(0, 1, 3);
    EXPECT_EQ(b.disk, 1u);
    EXPECT_EQ(b.row, 0u);
    auto c = raid1eCopy(1, 0, 3);
    EXPECT_EQ(c.disk, 2u);
    EXPECT_EQ(c.row, 0u);
    auto d = raid1eCopy(1, 1, 3);
    EXPECT_EQ(d.disk, 0u);
    EXPECT_EQ(d.row, 1u);
    auto e = raid1eCopy(2, 0, 3);
    EXPECT_EQ(e.disk, 1u);
    EXPECT_EQ(e.row, 1u);
    auto f = raid1eCopy(2, 1, 3);
    EXPECT_EQ(f.disk, 2u);
    EXPECT_EQ(f.row, 1u);
}

// Linux md RAID 6 layouts (raid5.c raid5_compute_sector), N=5.
TEST(Raid6Layout, MdLeftSymmetricMatchesKernel) {
    // stripe 0: P=4, Q=0, data 1,2,3; stripe 1: P=3, Q=4, data 0,1,2
    auto m0 = raid6Disks(0, 5, Raid5Algorithm::LeftSymmetric);
    EXPECT_EQ(m0.pDisk, 4u); EXPECT_EQ(m0.qDisk, 0u);
    EXPECT_EQ(raid6DataDisk(0, 0, 5, Raid5Algorithm::LeftSymmetric), 1u);
    EXPECT_EQ(raid6DataDisk(0, 2, 5, Raid5Algorithm::LeftSymmetric), 3u);
    auto m2 = raid6Disks(2, 5, Raid5Algorithm::LeftSymmetric); // P=2, Q=3, data 4,0,1
    EXPECT_EQ(m2.qDisk, 3u);
    EXPECT_EQ(raid6DataDisk(2, 0, 5, Raid5Algorithm::LeftSymmetric), 4u);
    EXPECT_EQ(raid6QSlot(2, 4, 5, Raid5Algorithm::LeftSymmetric), 0u);
}

TEST(Raid6Layout, MdLeftAsymmetricMatchesKernel) {
    // stripe 0: Q D D D P; stripe 2: D D P Q D with Q slots counted after Q
    auto m0 = raid6Disks(0, 5);
    EXPECT_EQ(m0.pDisk, 4u); EXPECT_EQ(m0.qDisk, 0u);
    EXPECT_EQ(raid6DataDisk(0, 0, 5), 1u);
    auto m2 = raid6Disks(2, 5);
    EXPECT_EQ(m2.pDisk, 2u); EXPECT_EQ(m2.qDisk, 3u);
    EXPECT_EQ(raid6DataDisk(2, 0, 5), 0u);
    EXPECT_EQ(raid6DataDisk(2, 2, 5), 4u);
    EXPECT_EQ(raid6QSlot(2, 4, 5), 0u); // first slot is the disk right after Q
    EXPECT_EQ(raid6QSlot(2, 0, 5), 1u);
}
