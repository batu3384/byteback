#pragma once

// Pure RAID stripe-layout math, extracted from virtual_raid.cpp so the
// on-disk placement (which disk holds data/parity for a given stripe) is
// unit-testable (CA-005). The read paths in virtual_raid.cpp delegate to
// these; if a layout function is wrong, recovery reads the RIGHT offset on
// the WRONG disk — GF(2^8) correctness cannot mask that.

#include <cstdint>

namespace byteback {
namespace raid_layout {

// RAID 5 data-block order. Cite: drivers/md/raid5.c
// algorithm_left_asymmetric / left_symmetric / right_asymmetric / right_symmetric.
// Linux mdadm default is LeftSymmetric.
enum class Raid5Algorithm : uint8_t {
    LeftAsymmetric = 0,
    LeftSymmetric = 1,
    RightAsymmetric = 2,
    RightSymmetric = 3,
};

// RAID 5 — LEFT: parity starts on the LAST disk and steps down.
// RIGHT: parity starts on disk 0 and steps up. Period = numDisks.
uint32_t raid5ParityDisk(uint64_t stripeIndex, uint32_t numDisks,
                         Raid5Algorithm algo = Raid5Algorithm::LeftAsymmetric);

// RAID 5 — physical data disk for the logical block within the stripe.
// Left-asymmetric: remaining disks fill in increasing index order.
// Left-symmetric: remaining disks start at parity+1 and wrap (mdadm default).
uint32_t raid5DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks,
                       Raid5Algorithm algo = Raid5Algorithm::LeftAsymmetric);

// RAID 6 — Linux md layouts (drivers/md/raid5.c raid5_compute_sector).
// Q sits right after P. Asymmetric: data fills the other disks in index
// order. Symmetric: data starts right after Q and wraps. Left: P starts on
// the last disk and steps down; right: P starts on disk 0 and steps up.
struct Raid6Map {
    uint32_t pDisk;
    uint32_t qDisk;
};
Raid6Map raid6Disks(uint64_t stripeIndex, uint32_t numDisks,
                    Raid5Algorithm algo = Raid5Algorithm::LeftAsymmetric);

// RAID 6 — physical data disk for the logical block within the stripe.
uint32_t raid6DataDisk(uint64_t stripeIndex, uint32_t blockInStripe, uint32_t numDisks,
                       Raid5Algorithm algo = Raid5Algorithm::LeftAsymmetric);

// RAID 6 — Q-syndrome slot of a data disk: Q = XOR of g^slot * D. md counts
// slots from the disk after Q (disk 0 when Q is the last disk), wrapping and
// skipping P and Q (raid6_d0 / raid6_idx_to_slot).
uint32_t raid6QSlot(uint64_t stripeIndex, uint32_t disk, uint32_t numDisks,
                    Raid5Algorithm algo = Raid5Algorithm::LeftAsymmetric);

// RAID 10 — stripe of mirror pairs: block b lives on pair (b % (N/2));
// the pair's members are disks 2*pair and 2*pair+1 (either serves a read).
uint32_t raid10Pair(uint64_t blockIndex, uint32_t numDisks);
uint32_t raid10MemberA(uint64_t blockIndex, uint32_t numDisks);
uint32_t raid10MemberB(uint64_t blockIndex, uint32_t numDisks);

// RAID1E near (2 copies): slot = dataIndex*2 + copy; disk = slot % N; row = slot / N.
// IBM ServeRAID / md RAID10 layout=n2 on odd N. N >= 3.
struct Raid1eLoc {
    uint32_t disk = 0;
    uint64_t row = 0;
};
Raid1eLoc raid1eCopy(uint64_t dataIndex, uint32_t copy, uint32_t numDisks);

} // namespace raid_layout
} // namespace byteback
