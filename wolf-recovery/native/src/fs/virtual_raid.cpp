#include "fs/virtual_raid.h"
#include "fs/raid_layout.h"
#include <algorithm>
#include <cstring>

namespace wolf {

VirtualRaid::VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size)
    : level_(level), num_disks_(drive_indices.size()), disk_size_(0), block_size_(block_size) {
    if (num_disks_ < 2) {
        throw std::invalid_argument("RAID requires at least 2 disks.");
    }
    if (level == RaidLevel::RAID5 && num_disks_ < 3) {
        throw std::invalid_argument("RAID 5 requires at least 3 disks.");
    }
    if (level == RaidLevel::RAID6 && num_disks_ < 4) {
        throw std::invalid_argument("RAID 6 requires at least 4 disks.");
    }
    if (level == RaidLevel::RAID10 && (num_disks_ % 2 != 0)) {
        throw std::invalid_argument("RAID 10 requires an even number of disks.");
    }

    disk_active_.resize(num_disks_, true);
    for (int idx : drive_indices) {
        auto reader = std::make_shared<wolf::DiskReader>();
        if (!reader->openDrive(idx)) {
            throw std::runtime_error("Failed to open physical drive for RAID.");
        }
        // All array members must be readable; capacity is bounded by the smallest disk.
        uint64_t memberSize = reader->getDiskSize();
        if (memberSize == 0) {
            throw std::runtime_error("RAID member disk has zero size.");
        }
        disk_size_ = (disk_size_ == 0) ? memberSize : std::min(disk_size_, memberSize);
        disk_readers_.push_back(reader);
    }
}

void VirtualRaid::write(size_t, const std::vector<uint8_t>&) {
    throw std::runtime_error("Write unsupported on physical RAID mode (forensic read-only).");
}

uint64_t VirtualRaid::capacity() const {
    switch (level_) {
        case RaidLevel::RAID0: return disk_size_ * num_disks_;
        case RaidLevel::RAID1: return disk_size_;
        case RaidLevel::RAID5: return disk_size_ * (num_disks_ - 1);
        case RaidLevel::RAID6: return disk_size_ * (num_disks_ - 2);
        case RaidLevel::RAID10: return disk_size_ * (num_disks_ / 2);
    }
    return 0;
}

bool VirtualRaid::readMemberAligned(size_t disk_idx, uint64_t offset, size_t length, uint8_t* out) const {
    if (disk_idx >= num_disks_ || !disk_readers_[disk_idx]->isOpen()) return false;

    uint32_t sectorSize = disk_readers_[disk_idx]->getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t alignedStart = (offset / sectorSize) * sectorSize;
    uint64_t end = offset + length;
    uint64_t alignedEnd = ((end + sectorSize - 1) / sectorSize) * sectorSize;
    uint32_t alignedLen = static_cast<uint32_t>(alignedEnd - alignedStart);

    // Scratch buffer so a short read never under-runs the caller's slice.
    static thread_local std::vector<uint8_t> scratch;
    scratch.resize(alignedLen);

    auto res = disk_readers_[disk_idx]->readSectors(alignedStart, alignedLen, scratch.data());
    if (!res.success || res.bytesRead < length + (offset - alignedStart)) {
        // Forensic rule: a bad sector yields zeros, it never aborts the
        // reconstruction — an uncorrectable region must not take the whole
        // array read down with it.
        std::memset(out, 0, length);
        return false;
    }
    std::memcpy(out, scratch.data() + (offset - alignedStart), length);
    return true;
}

std::vector<uint8_t> VirtualRaid::read(size_t offset, size_t length) const {
    if (length == 0) return std::vector<uint8_t>();
    switch (level_) {
        case RaidLevel::RAID0: return read_raid0(offset, length);
        case RaidLevel::RAID1: return read_raid1(offset, length);
        case RaidLevel::RAID5: return read_raid5(offset, length);
        case RaidLevel::RAID6: return read_raid6(offset, length);
        case RaidLevel::RAID10: return read_raid10(offset, length);
    }
    return std::vector<uint8_t>();
}

void VirtualRaid::fail_disk(size_t disk_index) {
    if (disk_index >= num_disks_) throw std::out_of_range("Invalid disk index");
    disk_active_[disk_index] = false;
}

void VirtualRaid::reconstruct_disk(size_t disk_index) {
    if (disk_index >= num_disks_) throw std::out_of_range("Invalid disk index");
    if (disk_active_[disk_index]) return;
    // In-place reconstruction would require writing to physical media, which
    // this tool never does. Redundancy is applied transparently during read()
    // instead: mark the disk failed and reads will go through parity/mirrors.
    throw std::runtime_error("In-place reconstruction disabled (forensic read-only mode); redundancy is applied on read");
}

std::vector<uint8_t> VirtualRaid::read_raid0(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    while (res_idx < length) {
        size_t block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;
        size_t disk_idx = block_index % num_disks_;
        size_t block_on_disk = block_index / num_disks_;
        uint64_t disk_offset = static_cast<uint64_t>(block_on_disk) * block_size_ + offset_in_block;

        if (disk_offset + (block_size_ - offset_in_block) > disk_size_) {
            throw std::out_of_range("Read exceeds RAID capacity");
        }
        if (!disk_active_[disk_idx]) {
            throw std::runtime_error("Reading from a failed disk in RAID 0 (no redundancy)");
        }

        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);
        readMemberAligned(disk_idx, disk_offset, read_len, &result[res_idx]);

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

std::vector<uint8_t> VirtualRaid::read_raid1(size_t offset, size_t length) const {
    if (offset + length > disk_size_) {
        throw std::out_of_range("Read exceeds RAID capacity");
    }
    for (size_t i = 0; i < num_disks_; ++i) {
        if (disk_active_[i]) {
            std::vector<uint8_t> result(length);
            if (readMemberAligned(i, offset, length, result.data())) return result;
            // Read failed (bad sectors) — fall through and try the next mirror.
        }
    }
    throw std::runtime_error("No readable mirror available in RAID 1");
}

std::vector<uint8_t> VirtualRaid::read_raid5(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    while (res_idx < length) {
        size_t logical_block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;

        size_t stripe_index = logical_block_index / (num_disks_ - 1);
        size_t block_in_stripe = logical_block_index % (num_disks_ - 1);

        // Left-asymmetric placement lives in raid_layout.cpp (unit-tested).
        size_t parity_disk = raid_layout::raid5ParityDisk(stripe_index, static_cast<uint32_t>(num_disks_));
        size_t data_disk = raid_layout::raid5DataDisk(stripe_index, static_cast<uint32_t>(block_in_stripe), static_cast<uint32_t>(num_disks_));

        uint64_t disk_offset = static_cast<uint64_t>(stripe_index) * block_size_ + offset_in_block;
        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);

        if (disk_active_[data_disk]) {
            readMemberAligned(data_disk, disk_offset, read_len, &result[res_idx]);
        } else {
            // XOR-reconstruct: D_failed = P XOR (all other data blocks).
            std::vector<uint8_t> reconstructed(block_size_, 0);
            std::vector<uint8_t> temp(block_size_);
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i == data_disk) continue;
                if (!disk_active_[i]) {
                    throw std::runtime_error("RAID 5 read failed: multiple disks failed");
                }
                readMemberAligned(i, stripe_index * block_size_, block_size_, temp.data());
                for (size_t b = 0; b < block_size_; ++b) {
                    reconstructed[b] ^= temp[b];
                }
            }
            std::memcpy(&result[res_idx], &reconstructed[offset_in_block], read_len);
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

std::vector<uint8_t> VirtualRaid::read_raid6(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;

    // Stripe layout (left-asymmetric, P and Q adjacent, rotating):
    //   p_pos = (N-1) - (stripe % N)
    //   q_pos = p_pos == 0 ? N-1 : p_pos - 1
    // Remaining slots (in disk order) carry the N-2 data blocks.
    while (res_idx < length) {
        size_t logical_block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;

        size_t stripe_index = logical_block_index / (num_disks_ - 2);
        size_t block_in_stripe = logical_block_index % (num_disks_ - 2);

        raid_layout::Raid6Map pq = raid_layout::raid6Disks(stripe_index, static_cast<uint32_t>(num_disks_));
        size_t p_disk = pq.pDisk;
        size_t q_disk = pq.qDisk;

        // Map the logical data block to a physical slot: walk disk order,
        // skipping P and Q positions (raid_layout.cpp owns the placement).
        size_t data_disk = raid_layout::raid6DataDisk(stripe_index, static_cast<uint32_t>(block_in_stripe), static_cast<uint32_t>(num_disks_));

        uint64_t stripe_base = static_cast<uint64_t>(stripe_index) * block_size_;
        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);

        if (disk_active_[data_disk]) {
            readMemberAligned(data_disk, stripe_base + offset_in_block, read_len, &result[res_idx]);
        } else {
            // Count failed *data* disks in this stripe (P/Q loss alone does
            // not affect reads).
            size_t failedData = 0;
            size_t failed[2] = {0, 0};
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i == p_disk || i == q_disk) continue;
                if (!disk_active_[i]) { if (failedData < 2) failed[failedData] = i; failedData++; }
            }

            if (failedData == 1) {
                // Single failure: D = P XOR (other data blocks).
                std::vector<uint8_t> acc(block_size_, 0), temp(block_size_);
                if (!disk_active_[p_disk]) {
                    throw std::runtime_error("RAID 6: data + P failed with Q needed (unsupported single-pass path)");
                }
                readMemberAligned(p_disk, stripe_base, block_size_, acc.data());
                for (size_t i = 0; i < num_disks_; ++i) {
                    if (i == p_disk || i == q_disk || i == failed[0] || !disk_active_[i]) continue;
                    readMemberAligned(i, stripe_base, block_size_, temp.data());
                    for (size_t b = 0; b < block_size_; ++b) acc[b] ^= temp[b];
                }
                std::memcpy(&result[res_idx], &acc[offset_in_block], read_len);
            } else if (failedData == 2) {
                // Two failures: solve the 2-unknown GF system.
                //   P' = P XOR (healthy data) = X_i XOR X_j
                //   Q' = Q XOR (healthy data * g^slot) = X_i*g^i XOR X_j*g^j
                // where i/j are the failed blocks' stripe slot indices.
                auto slotOf = [&](size_t disk) -> int {
                    int slot = 0;
                    for (size_t i = 0; i < num_disks_; ++i) {
                        if (i == p_disk || i == q_disk) continue;
                        if (i == disk) return slot;
                        slot++;
                    }
                    return -1;
                };
                int si = slotOf(failed[0]);
                int sj = slotOf(failed[1]);

                std::vector<uint8_t> pAcc(block_size_, 0), qAcc(block_size_, 0), temp(block_size_);
                if (!disk_active_[p_disk] || !disk_active_[q_disk]) {
                    throw std::runtime_error("RAID 6: too many failures (P or Q also lost)");
                }
                readMemberAligned(p_disk, stripe_base, block_size_, pAcc.data());
                readMemberAligned(q_disk, stripe_base, block_size_, qAcc.data());
                for (size_t i = 0; i < num_disks_; ++i) {
                    if (i == p_disk || i == q_disk || !disk_active_[i]) continue;
                    readMemberAligned(i, stripe_base, block_size_, temp.data());
                    for (size_t b = 0; b < block_size_; ++b) {
                        pAcc[b] ^= temp[b];
                        qAcc[b] ^= raid6_math::gfMul(temp[b], raid6_math::gfPow(slotOf(i)));
                    }
                }
                // Solve: X_i = (P'*g^j XOR Q') / (g^i XOR g^j)
                uint8_t gi = raid6_math::gfPow(si);
                uint8_t gj = raid6_math::gfPow(sj);
                uint8_t denom = gi ^ gj;
                if (denom == 0) throw std::runtime_error("RAID 6: degenerate GF system");
                std::vector<uint8_t> xi(block_size_), xj(block_size_);
                for (size_t b = 0; b < block_size_; ++b) {
                    uint8_t num = raid6_math::gfMul(gj, pAcc[b]) ^ qAcc[b];
                    xi[b] = raid6_math::gfDiv(num, denom);
                    xj[b] = pAcc[b] ^ xi[b];
                }
                const std::vector<uint8_t>& wanted = (failed[0] == data_disk) ? xi : xj;
                std::memcpy(&result[res_idx], &wanted[offset_in_block], read_len);
            } else {
                throw std::runtime_error("RAID 6 read failed: >2 data disks failed");
            }
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

std::vector<uint8_t> VirtualRaid::read_raid10(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    while (res_idx < length) {
        // Stripe across mirror pairs: block b lives on pair (b % (N/2)),
        // members 2*(b % (N/2)) and 2*(b % (N/2)) + 1.
        size_t block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;
        size_t num_pairs = num_disks_ / 2;
        size_t pair = raid_layout::raid10Pair(block_index, static_cast<uint32_t>(num_disks_));
        size_t block_on_pair = block_index / num_pairs;
        uint64_t disk_offset = static_cast<uint64_t>(block_on_pair) * block_size_ + offset_in_block;

        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);
        size_t memberA = raid_layout::raid10MemberA(block_index, static_cast<uint32_t>(num_disks_));
        size_t memberB = raid_layout::raid10MemberB(block_index, static_cast<uint32_t>(num_disks_));

        bool ok = false;
        if (disk_active_[memberA]) {
            ok = readMemberAligned(memberA, disk_offset, read_len, &result[res_idx]);
        }
        if (!ok && disk_active_[memberB]) {
            ok = readMemberAligned(memberB, disk_offset, read_len, &result[res_idx]);
        }
        if (!ok) {
            throw std::runtime_error("RAID 10 read failed: both members of a mirror pair failed");
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

} // namespace wolf
