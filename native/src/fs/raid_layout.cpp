// Pure RAID stripe-layout math — see fs/raid_layout.h.
#include "fs/raid_layout.h"

namespace byteback {
namespace raid_layout {

uint32_t raid5ParityDisk(uint64_t stripeIndex, uint32_t numDisks) {
    if (numDisks == 0) return 0;
    return (numDisks - 1) - static_cast<uint32_t>(stripeIndex % numDisks);
}

uint32_t raid5DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks) {
    uint32_t parity = raid5ParityDisk(stripeIndex, numDisks);
    uint32_t disk = blockInStripe;
    if (disk >= parity) ++disk;
    return disk % numDisks;
}

Raid6Map raid6Disks(uint64_t stripeIndex, uint32_t numDisks) {
    Raid6Map m{};
    if (numDisks == 0) return m;
    m.pDisk = (numDisks - 1) - static_cast<uint32_t>(stripeIndex % numDisks);
    m.qDisk = (m.pDisk == 0) ? (numDisks - 1) : (m.pDisk - 1);
    return m;
}

uint32_t raid6DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks) {
    Raid6Map m = raid6Disks(stripeIndex, numDisks);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < numDisks; ++i) {
        if (i == m.pDisk || i == m.qDisk) continue;
        if (seen == blockInStripe) return i;
        ++seen;
    }
    return 0; // blockInStripe out of range — caller's stripe math is wrong
}

uint32_t raid10Pair(uint64_t blockIndex, uint32_t numDisks) {
    uint32_t numPairs = numDisks / 2;
    if (numPairs == 0) return 0;
    return static_cast<uint32_t>(blockIndex % numPairs);
}

uint32_t raid10MemberA(uint64_t blockIndex, uint32_t numDisks) {
    return raid10Pair(blockIndex, numDisks) * 2;
}

uint32_t raid10MemberB(uint64_t blockIndex, uint32_t numDisks) {
    return raid10Pair(blockIndex, numDisks) * 2 + 1;
}

} // namespace raid_layout
} // namespace byteback
