#pragma once

// Pure RAID stripe-layout math, extracted from virtual_raid.cpp so the
// on-disk placement (which disk holds data/parity for a given stripe) is
// unit-testable (CA-005). The read paths in virtual_raid.cpp delegate to
// these; if a layout function is wrong, recovery reads the RIGHT offset on
// the WRONG disk — GF(2^8) correctness cannot mask that.

#include <cstdint>

namespace byteback {
namespace raid_layout {

// RAID 5 — left-asymmetric: parity starts on the LAST disk and rotates one
// disk down per stripe (period = numDisks).
uint32_t raid5ParityDisk(uint64_t stripeIndex, uint32_t numDisks);

// RAID 5 — physical data disk for the logical block within the stripe,
// skipping the parity disk (left-asymmetric: data disks fill in disk order).
uint32_t raid5DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks);

// RAID 6 — P and Q are adjacent and rotate together: P = RAID-5-style parity,
// Q sits immediately below P (wrapping to the last disk when P is disk 0).
struct Raid6Map {
    uint32_t pDisk;
    uint32_t qDisk;
};
Raid6Map raid6Disks(uint64_t stripeIndex, uint32_t numDisks);

// RAID 6 — physical data disk for the logical block within the stripe,
// skipping BOTH parity positions (disk order).
uint32_t raid6DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks);

// RAID 10 — stripe of mirror pairs: block b lives on pair (b % (N/2));
// the pair's members are disks 2*pair and 2*pair+1 (either serves a read).
uint32_t raid10Pair(uint64_t blockIndex, uint32_t numDisks);
uint32_t raid10MemberA(uint64_t blockIndex, uint32_t numDisks);
uint32_t raid10MemberB(uint64_t blockIndex, uint32_t numDisks);

} // namespace raid_layout
} // namespace byteback
