#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <memory>
#include <string>
#include "byteback_io.h"
#include "fs/raid_layout.h"

namespace byteback {

enum class RaidLevel {
    RAID0,
    RAID1,
    RAID5,
    RAID6,
    RAID10,
    JBOD,   // mdadm LINEAR / concat; Disk Drill JBOD
    RAID1E  // 2-copy near; slot=D*2+copy; NAPI 6
};

// Reconstruct IPC trust boundary: power-of-two stripe, 512 B .. 1 MiB.
// VirtualRaid itself accepts any non-zero stripe so unit tests can use 4 KiB.
inline bool isRaidStripeSize(size_t n) {
    constexpr size_t kMin = 512;
    constexpr size_t kMax = 1024u * 1024u;
    return n >= kMin && n <= kMax && (n & (n - 1)) == 0;
}

// Software reconstruction of a degraded or broken RAID array over physical
// disks. All reads go through the member DiskReaders; nothing is written
// back to the physical media (forensic read-only by design).
//
// Layouts:
//   RAID0  — left-synchronous stripe, block i lives on disk (i % N).
//            No redundancy: an unreadable member throws (not silent zeros).
//   RAID1  — mirror; reads from the first healthy member
//   RAID5  — left-asymmetric (default) or left-symmetric (mdadm);
//            rotating parity; single-disk failure reconstructed via XOR
//   RAID6  — double parity (P = XOR, Q = GF(2^8) Reed-Solomon); tolerates
//            two simultaneous failures
//   RAID10 — stripe of mirrors (pairs: 0+1, 2+3, ...); tolerates one
//            failure per mirrored pair
//   JBOD   — concatenate member payloads in order (mdadm LINEAR). No stripe.
//   RAID1E — 2-copy near (odd N typical): copies at consecutive slots.
class VirtualRaid {
public:
    VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size,
                uint64_t data_offset_bytes = 0,
                raid_layout::Raid5Algorithm raid5_algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric);

    // Assembly from already-opened member readers (unit tests, pre-loaded images).
    VirtualRaid(RaidLevel level, std::vector<std::shared_ptr<DiskReader>> members, size_t block_size,
                uint64_t data_offset_bytes = 0,
                raid_layout::Raid5Algorithm raid5_algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric,
                std::vector<uint64_t> member_data_offsets = {},
                std::vector<uint64_t> member_data_lengths = {});

    // Convenience: one in-memory image per member disk.
    static VirtualRaid fromImages(RaidLevel level, std::vector<std::vector<uint8_t>> images,
                                  size_t block_size, uint64_t data_offset_bytes = 0,
                                  raid_layout::Raid5Algorithm raid5_algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric);

    // Local evidence files (RAW/E01/VHD family). Rejects devices, http(s), /dev/.
    static VirtualRaid fromEvidencePaths(RaidLevel level, const std::vector<std::string>& paths,
                                         size_t block_size, uint64_t data_offset_bytes = 0,
                                         raid_layout::Raid5Algorithm raid5_algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric);

    // Reads are read-only; write() always throws (forensic mode).
    void write(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read(size_t offset, size_t length) const;

    // Logical capacity of the assembled array in bytes.
    uint64_t capacity() const;

    void fail_disk(size_t disk_index);
    void reconstruct_disk(size_t disk_index);
    bool is_disk_active(size_t disk_index) const;

    size_t num_disks() const { return num_disks_; }
    RaidLevel level() const { return level_; }
    size_t blockSize() const { return block_size_; }
    uint64_t dataOffsetBytes() const { return data_offset_bytes_; }
    // Imaging resume identity: level, stripe, offset, capacity, failed-disk
    // mask, member drive index + first 16 bytes. Same-size arrays no longer
    // collide on "raid:<capacity>" alone.
    std::string resumeKey() const;

private:
    void initFromMembers(std::vector<std::shared_ptr<DiskReader>> members);

    RaidLevel level_;
    size_t num_disks_;
    uint64_t disk_size_;
    size_t block_size_;
    uint64_t data_offset_bytes_ = 0;
    std::vector<uint64_t> member_data_offsets_;
    std::vector<uint64_t> member_data_lengths_;
    raid_layout::Raid5Algorithm raid5_algorithm_ = raid_layout::Raid5Algorithm::LeftAsymmetric;
    std::vector<std::shared_ptr<byteback::DiskReader>> disk_readers_;
    mutable std::vector<bool> disk_active_;

    // Sector-aligned read of [offset, offset+length) from one member disk.
    // DiskReader::readSectors rejects unaligned offsets/sizes, and RAID
    // arithmetic produces byte-granular block offsets — so this helper rounds
    // down/up to sector boundaries and slices the requested range out of the
    // aligned result. Zeros `out` on read failure so XOR reconstruction is
    // not poisoned by stale bytes. Callers without redundancy (RAID0) must
    // treat false as unread and throw — zeros are not file content.
    // `offset` is payload-relative; data_offset_bytes_ is added here so every
    // RAID level inherits the per-member reservation.
    bool readMemberAligned(size_t disk_idx, uint64_t offset, size_t length, uint8_t* out) const;
    uint64_t memberPayloadBytes() const;
    uint64_t memberBase(size_t disk_idx) const;

    std::vector<uint8_t> read_raid0(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid1(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid5(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid6(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid10(size_t offset, size_t length) const;
    std::vector<uint8_t> read_jbod(size_t offset, size_t length) const;
    std::vector<uint8_t> read_raid1e(size_t offset, size_t length) const;
    uint64_t memberExtentBytes(size_t disk_idx) const;
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

} // namespace byteback
