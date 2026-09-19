// Pure RAID stripe-layout math — see fs/raid_layout.h.
#include "fs/raid_layout.h"

namespace byteback {
namespace raid_layout {

uint32_t raid5ParityDisk(uint64_t stripeIndex, uint32_t numDisks, Raid5Algorithm algo) {
    if (numDisks == 0) return 0;
    const uint32_t i = static_cast<uint32_t>(stripeIndex % numDisks);
    if (algo == Raid5Algorithm::RightAsymmetric || algo == Raid5Algorithm::RightSymmetric) return i;
    return (numDisks - 1) - i;
}

uint32_t raid5DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks,
                       Raid5Algorithm algo) {
    uint32_t parity = raid5ParityDisk(stripeIndex, numDisks, algo);
    if (numDisks == 0) return 0;
    if (algo == Raid5Algorithm::LeftSymmetric) return (parity + 1u + blockInStripe) % numDisks;
    if (algo == Raid5Algorithm::RightSymmetric) {
        return (parity + numDisks - 1u - blockInStripe) % numDisks;
    }
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

Raid1eLoc raid1eCopy(uint64_t dataIndex, uint32_t copy, uint32_t numDisks) {
    Raid1eLoc loc;
    if (numDisks < 3 || copy > 1) return loc;
    const uint64_t slot = dataIndex * 2 + copy;
    loc.disk = static_cast<uint32_t>(slot % numDisks);
    loc.row = slot / numDisks;
    return loc;
}

} // namespace raid_layout
} // namespace byteback
