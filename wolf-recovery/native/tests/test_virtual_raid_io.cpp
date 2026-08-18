#include "fs/virtual_raid.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace wolf;

TEST(VirtualRaidIo, Raid0StripesAcrossMembers) {
    auto [d0, d1] = wolf::testfix::buildRaid0MemberDisks();
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
    auto [d0, d1] = wolf::testfix::buildRaid0MemberDisks();
    auto raid = VirtualRaid::fromImages(RaidLevel::RAID0, {d0, d1}, 65536);
    EXPECT_EQ(raid.capacity(), d0.size() + d1.size());
}
