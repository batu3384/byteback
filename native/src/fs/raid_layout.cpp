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
    // Both symmetric layouts start the data right after parity (md raid5.c).
    if (algo == Raid5Algorithm::LeftSymmetric || algo == Raid5Algorithm::RightSymmetric) {
        return (parity + 1u + blockInStripe) % numDisks;
    }
    uint32_t disk = blockInStripe;
    if (disk >= parity) ++disk;
    return disk % numDisks;
}

namespace {
bool isSymmetric(Raid5Algorithm a) {
    return a == Raid5Algorithm::LeftSymmetric || a == Raid5Algorithm::RightSymmetric;
}
} // namespace

Raid6Map raid6Disks(uint64_t stripeIndex, uint32_t numDisks, Raid5Algorithm algo) {
    Raid6Map m{};
    if (numDisks == 0) return m;
    const uint32_t r = static_cast<uint32_t>(stripeIndex % numDisks);
    const bool right = algo == Raid5Algorithm::RightAsymmetric || algo == Raid5Algorithm::RightSymmetric;
    m.pDisk = right ? r : (numDisks - 1) - r;
    m.qDisk = (m.pDisk + 1) % numDisks;
    return m;
}

uint32_t raid6DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks,
                       Raid5Algorithm algo) {
    if (numDisks < 3) return 0;
    const Raid6Map m = raid6Disks(stripeIndex, numDisks, algo);
    if (isSymmetric(algo)) return (m.pDisk + 2u + blockInStripe) % numDisks;
    if (m.pDisk == numDisks - 1) return blockInStripe + 1;          // Q D D D P
    return blockInStripe >= m.pDisk ? blockInStripe + 2 : blockInStripe; // D D P Q D
}

uint32_t raid6QSlot(uint64_t stripeIndex, uint32_t disk, uint32_t numDisks, Raid5Algorithm algo) {
    if (numDisks < 3) return 0;
    const Raid6Map m = raid6Disks(stripeIndex, numDisks, algo);
    const uint32_t d0 = m.qDisk == numDisks - 1 ? 0 : m.qDisk + 1;
    uint32_t slot = 0;
    for (uint32_t k = 0, i = d0; k < numDisks; ++k, i = (i + 1) % numDisks) {
        if (i == m.pDisk || i == m.qDisk) continue;
        if (i == disk) return slot;
        ++slot;
    }
    return 0; // P/Q disk: no data slot
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
