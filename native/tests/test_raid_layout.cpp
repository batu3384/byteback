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

// ---- RAID 6 (adjacent P/Q rotating together, N=5) ----
TEST(Raid6Layout, ParityRotationTable) {
    auto m0 = raid6Disks(0, 5); // P=4, Q=3
    EXPECT_EQ(m0.pDisk, 4u); EXPECT_EQ(m0.qDisk, 3u);
    auto m1 = raid6Disks(1, 5); // P=3, Q=2
    EXPECT_EQ(m1.pDisk, 3u); EXPECT_EQ(m1.qDisk, 2u);
    auto m2 = raid6Disks(2, 5);
    EXPECT_EQ(m2.pDisk, 2u); EXPECT_EQ(m2.qDisk, 1u);
    auto m3 = raid6Disks(3, 5);
    EXPECT_EQ(m3.pDisk, 1u); EXPECT_EQ(m3.qDisk, 0u);
    auto m4 = raid6Disks(4, 5); // P wraps to 0, Q wraps to the last disk
    EXPECT_EQ(m4.pDisk, 0u); EXPECT_EQ(m4.qDisk, 4u);
    auto m5 = raid6Disks(5, 5); // period = N
    EXPECT_EQ(m5.pDisk, 4u); EXPECT_EQ(m5.qDisk, 3u);
}

TEST(Raid6Layout, DataDisksSkipBothParities) {
    // Stripe 0 (P=4, Q=3): slots 0..2 -> disks 0,1,2.
    EXPECT_EQ(raid6DataDisk(0, 0, 5), 0u);
    EXPECT_EQ(raid6DataDisk(0, 1, 5), 1u);
    EXPECT_EQ(raid6DataDisk(0, 2, 5), 2u);
    // Stripe 1 (P=3, Q=2): slots -> 0,1,4.
    EXPECT_EQ(raid6DataDisk(1, 0, 5), 0u);
    EXPECT_EQ(raid6DataDisk(1, 1, 5), 1u);
    EXPECT_EQ(raid6DataDisk(1, 2, 5), 4u);
    // Stripe 4 (P=0, Q=4): slots -> 1,2,3.
    EXPECT_EQ(raid6DataDisk(4, 0, 5), 1u);
    EXPECT_EQ(raid6DataDisk(4, 2, 5), 3u);
}

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
