#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <memory>
#include "wolf_io.h"

namespace wolf {

enum class RaidLevel {
    RAID0,
    RAID1,
    RAID5,
    RAID6,
    RAID10
};

// Software reconstruction of a degraded or broken RAID array over physical
// disks. All reads go through the member DiskReaders; nothing is written
// back to the physical media (forensic read-only by design).
//
// Layouts:
//   RAID0  — left-synchronous stripe, block i lives on disk (i % N)
//   RAID1  — mirror; reads from the first healthy member
//   RAID5  — left-asymmetric, rotating parity; single-disk failure
//            reconstructed via XOR of the surviving members
//   RAID6  — double parity (P = XOR, Q = GF(2^8) Reed-Solomon); tolerates
//            two simultaneous failures
//   RAID10 — stripe of mirrors (pairs: 0+1, 2+3, ...); tolerates one
//            failure per mirrored pair
class VirtualRaid {
public:
    VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size);

    // Assembly from already-opened member readers (unit tests, pre-loaded images).
    VirtualRaid(RaidLevel level, std::vector<std::shared_ptr<DiskReader>> members, size_t block_size);

    // Convenience: one in-memory image per member disk.
    static VirtualRaid fromImages(RaidLevel level, std::vector<std::vector<uint8_t>> images,
                                  size_t block_size);

    // Reads are read-only; write() always throws (forensic mode).
    void write(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read(size_t offset, size_t length) const;

    // Logical capacity of the assembled array in bytes.
    uint64_t capacity() const;

    void fail_disk(size_t disk_index);
    void reconstruct_disk(size_t disk_index);

    size_t num_disks() const { return num_disks_; }
    RaidLevel level() const { return level_; }

private:
    void initFromMembers(std::vector<std::shared_ptr<DiskReader>> members);

    RaidLevel level_;
    size_t num_disks_;
    uint64_t disk_size_;
    size_t block_size_;
    std::vector<std::shared_ptr<wolf::DiskReader>> disk_readers_;
    mutable std::vector<bool> disk_active_;

    // Sector-aligned read of [offset, offset+length) from one member disk.
    // DiskReader::readSectors rejects unaligned offsets/sizes, and RAID
    // arithmetic produces byte-granular block offsets — so this helper rounds
    // down/up to sector boundaries and slices the requested range out of the
    // aligned result. Fills the output with zeros on read failure (bad
    // sectors must not abort a forensic reconstruction).
    bool readMemberAligned(size_t disk_idx, uint64_t offset, size_t length, uint8_t* out) const;

    std::vector<uint8_t> read_raid0(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid1(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid5(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid6(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid10(size_t offset, size_t length) const;
};

// GF(2^8) arithmetic used by RAID 6 Q-syndrome (Reed-Solomon). Polynomial
// 0x11D (x^8 + x^4 + x^3 + x^2 + 1) — the same field ATA/SCSI RAID-6
// implementations use. Tables are built once, lazily.
namespace raid6_math {
// Multiply two GF(2^8) elements.
uint8_t gfMul(uint8_t a, uint8_t b);
// Raise the generator (2) to the given power.
uint8_t gfPow(int exponent);
// Divide in GF(2^8) (b must be nonzero).
uint8_t gfDiv(uint8_t a, uint8_t b);
} // namespace raid6_math

} // namespace wolf
