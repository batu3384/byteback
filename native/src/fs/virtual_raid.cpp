#include "fs/virtual_raid.h"
#include "fs/raid_layout.h"
#include "io/volume_mapper_win.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace byteback {

namespace {

void appendHex(std::string& out, const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out += kHex[p[i] >> 4];
        out += kHex[p[i] & 0xf];
    }
}

} // namespace

std::string VirtualRaid::resumeKey() const {
    std::string k = "raid:lv=";
    k += std::to_string(static_cast<int>(level_));
    k += ":n=";
    k += std::to_string(num_disks_);
    k += ":st=";
    k += std::to_string(block_size_);
    k += ":off=";
    k += std::to_string(data_offset_bytes_);
    k += ":a=";
    k += std::to_string(static_cast<int>(raid5_algorithm_));
    k += ":sz=";
    k += std::to_string(capacity());
    k += ":f=";
    for (size_t i = 0; i < disk_active_.size(); ++i) {
        k += disk_active_[i] ? '1' : '0';
    }
    for (const auto& r : disk_readers_) {
        const int idx = r ? r->getDriveIndex() : -2;
        k += ":m";
        k += std::to_string(idx);
        uint8_t head[16] = {};
        if (r && r->isOpen()) {
            (void)r->readBytes(0, 16, head);
        }
        k += "h";
        appendHex(k, head, sizeof(head));
    }
    return k;
}

VirtualRaid::VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size,
                         uint64_t data_offset_bytes, raid_layout::Raid5Algorithm raid5_algorithm)
    : level_(level), num_disks_(drive_indices.size()), disk_size_(0), block_size_(block_size),
      data_offset_bytes_(data_offset_bytes), raid5_algorithm_(raid5_algorithm) {
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
    if (level == RaidLevel::RAID1E && num_disks_ < 3) {
        throw std::invalid_argument("RAID 1E requires at least 3 disks.");
    }

    std::vector<std::shared_ptr<DiskReader>> members;
    members.reserve(drive_indices.size());
    for (int idx : drive_indices) {
        auto reader = std::make_shared<DiskReader>();
        if (!reader->openDrive(idx)) {
            throw std::runtime_error("Failed to open physical drive for RAID.");
        }
        members.push_back(std::move(reader));
    }
    initFromMembers(std::move(members));
}

VirtualRaid::VirtualRaid(RaidLevel level, std::vector<std::shared_ptr<DiskReader>> members, size_t block_size,
                         uint64_t data_offset_bytes, raid_layout::Raid5Algorithm raid5_algorithm,
                         std::vector<uint64_t> member_data_offsets,
                         std::vector<uint64_t> member_data_lengths)
    : level_(level), num_disks_(members.size()), disk_size_(0), block_size_(block_size),
      data_offset_bytes_(data_offset_bytes), member_data_offsets_(std::move(member_data_offsets)),
      member_data_lengths_(std::move(member_data_lengths)),
      raid5_algorithm_(raid5_algorithm) {
    if (num_disks_ < 2) throw std::invalid_argument("RAID requires at least 2 disks.");
    initFromMembers(std::move(members));
}

VirtualRaid VirtualRaid::fromImages(RaidLevel level, std::vector<std::vector<uint8_t>> images, size_t block_size,
                                    uint64_t data_offset_bytes, raid_layout::Raid5Algorithm raid5_algorithm) {
    std::vector<std::shared_ptr<DiskReader>> members;
    members.reserve(images.size());
    for (auto& img : images) {
        auto reader = std::make_shared<DiskReader>();
        reader->attachMemoryVolume(std::move(img));
        members.push_back(std::move(reader));
    }
    return VirtualRaid(level, std::move(members), block_size, data_offset_bytes, raid5_algorithm);
}

VirtualRaid VirtualRaid::fromEvidencePaths(RaidLevel level, const std::vector<std::string>& paths,
                                           size_t block_size, uint64_t data_offset_bytes,
                                           raid_layout::Raid5Algorithm raid5_algorithm) {
    const size_t n = paths.size();
    if (n < 2 || n > 16) throw std::invalid_argument("RAID requires 2..16 member images.");
    if (level == RaidLevel::RAID5 && n < 3) throw std::invalid_argument("RAID 5 requires at least 3 disks.");
    if (level == RaidLevel::RAID6 && n < 4) throw std::invalid_argument("RAID 6 requires at least 4 disks.");
    if (level == RaidLevel::RAID10 && (n % 2 != 0)) {
        throw std::invalid_argument("RAID 10 requires an even number of disks.");
    }
    if (level == RaidLevel::RAID1E && n < 3) {
        throw std::invalid_argument("RAID 1E requires at least 3 disks.");
    }
    std::vector<std::shared_ptr<DiskReader>> members;
    members.reserve(n);
    for (const auto& p : paths) {
        if (!isEvidenceImagePath(p)) throw std::invalid_argument("invalid RAID member image path");
        auto reader = std::make_shared<DiskReader>();
        std::string err;
        if (!reader->attachEvidenceImage(p, &err)) {
            throw std::runtime_error(err.empty() ? "Failed to open RAID member image" : err);
        }
        members.push_back(std::move(reader));
    }
    return VirtualRaid(level, std::move(members), block_size, data_offset_bytes, raid5_algorithm);
}

void VirtualRaid::initFromMembers(std::vector<std::shared_ptr<DiskReader>> members) {
    num_disks_ = members.size();
    disk_active_.assign(num_disks_, true);
    disk_readers_ = std::move(members);
    disk_size_ = 0;
    for (const auto& reader : disk_readers_) {
        if (!reader || !reader->isOpen()) {
            throw std::runtime_error("RAID member reader is not open.");
        }
        uint64_t memberSize = reader->getDiskSize();
        if (memberSize == 0) throw std::runtime_error("RAID member disk has zero size.");
        disk_size_ = (disk_size_ == 0) ? memberSize : std::min(disk_size_, memberSize);
    }
    if (!member_data_offsets_.empty() && member_data_offsets_.size() != num_disks_) {
        throw std::invalid_argument("RAID member data offsets must match disk count.");
    }
    if (!member_data_lengths_.empty() && member_data_lengths_.size() != num_disks_) {
        throw std::invalid_argument("RAID member data lengths must match disk count.");
    }
    if (member_data_offsets_.size() == num_disks_) {
        for (size_t i = 0; i < num_disks_; ++i) {
            const uint64_t sz = disk_readers_[i]->getDiskSize();
            if (member_data_offsets_[i] >= sz) {
                throw std::invalid_argument("RAID data offset exceeds member size.");
            }
            if (i < member_data_lengths_.size() && member_data_lengths_[i] > 0 &&
                member_data_offsets_[i] + member_data_lengths_[i] > sz) {
                throw std::invalid_argument("RAID member extent exceeds member size.");
            }
        }
    } else if (data_offset_bytes_ >= disk_size_) {
        throw std::invalid_argument("RAID data offset exceeds member size.");
    }
    if (block_size_ == 0 && level_ != RaidLevel::RAID1 && level_ != RaidLevel::JBOD) {
        throw std::invalid_argument("RAID stripe size required");
    }
}

void VirtualRaid::write(size_t, const std::vector<uint8_t>&) {
    throw std::runtime_error("Write unsupported on physical RAID mode (forensic read-only).");
}

uint64_t VirtualRaid::memberBase(size_t disk_idx) const {
    if (member_data_offsets_.size() == num_disks_ && disk_idx < num_disks_)
        return member_data_offsets_[disk_idx];
    return data_offset_bytes_;
}

uint64_t VirtualRaid::memberPayloadBytes() const {
    if (member_data_offsets_.size() == num_disks_) {
        uint64_t payload = (std::numeric_limits<uint64_t>::max)();
        for (size_t i = 0; i < num_disks_; ++i) {
            const uint64_t sz = disk_readers_[i]->getDiskSize();
            const uint64_t off = member_data_offsets_[i];
            const uint64_t p = sz > off ? sz - off : 0;
            payload = std::min(payload, p);
        }
        return payload == (std::numeric_limits<uint64_t>::max)() ? 0 : payload;
    }
    return disk_size_ > data_offset_bytes_ ? disk_size_ - data_offset_bytes_ : 0;
}

uint64_t VirtualRaid::memberExtentBytes(size_t disk_idx) const {
    if (disk_idx >= num_disks_ || !disk_readers_[disk_idx]) return 0;
    const uint64_t sz = disk_readers_[disk_idx]->getDiskSize();
    const uint64_t off = memberBase(disk_idx);
    uint64_t n = sz > off ? sz - off : 0;
    if (disk_idx < member_data_lengths_.size() && member_data_lengths_[disk_idx] > 0)
        n = std::min(n, member_data_lengths_[disk_idx]);
    return n;
}

uint64_t VirtualRaid::capacity() const {
    if (level_ == RaidLevel::JBOD) {
        uint64_t cap = 0;
        for (size_t i = 0; i < num_disks_; ++i) cap += memberExtentBytes(i);
        return cap;
    }
    // Block-floored usable extent per member: a striped array never exposes
    // the tail of a partial last block — real controllers report floored
    // capacity and the dead zone is unreachable (reads throw honestly).
    // RAID1 is a mirror of the payload, not stripe-floored.
    const uint64_t payload = memberPayloadBytes();
    if (level_ == RaidLevel::RAID1) return payload;
    if (block_size_ == 0) return 0;
    const uint64_t usable = (payload / block_size_) * block_size_;
    switch (level_) {
        case RaidLevel::RAID0: return usable * num_disks_;
        case RaidLevel::RAID1: return payload;
        case RaidLevel::RAID5: return usable * (num_disks_ - 1);
        case RaidLevel::RAID6: return usable * (num_disks_ - 2);
        case RaidLevel::RAID10: return usable * (num_disks_ / 2);
        case RaidLevel::JBOD: return 0;
        case RaidLevel::RAID1E: return (usable * num_disks_) / 2;
    }
    return 0;
}

bool VirtualRaid::readMemberAligned(size_t disk_idx, uint64_t offset, size_t length, uint8_t* out) const {
    if (disk_idx >= num_disks_ || !disk_readers_[disk_idx]->isOpen()) return false;
    if (data_offset_bytes_ > (std::numeric_limits<uint64_t>::max)() - offset) {
        std::memset(out, 0, length);
        return false;
    }
    const uint64_t base = memberBase(disk_idx);
    if (base > (std::numeric_limits<uint64_t>::max)() - offset) {
        std::memset(out, 0, length);
        return false;
    }
    offset += base;

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
    // Uniform capacity guard: RAID5/6/10 used to return silent zeros for
    // out-of-capacity reads (members zero-fill on I/O failure and parity
    // reconstruction then throws — or worse, RAID6 "succeeds" with a
    // zero-filled buffer). Reject up front, overflow-safe.
    const uint64_t cap = capacity();
    if (offset > cap || length > cap - offset) {
        throw std::out_of_range("Read exceeds RAID capacity");
    }
    switch (level_) {
        case RaidLevel::RAID0: return read_raid0(offset, length);
        case RaidLevel::RAID1: return read_raid1(offset, length);
        case RaidLevel::RAID5: return read_raid5(offset, length);
        case RaidLevel::RAID6: return read_raid6(offset, length);
        case RaidLevel::RAID10: return read_raid10(offset, length);
        case RaidLevel::JBOD: return read_jbod(offset, length);
        case RaidLevel::RAID1E: return read_raid1e(offset, length);
    }
    return std::vector<uint8_t>();
}

void VirtualRaid::fail_disk(size_t disk_index) {
    if (disk_index >= num_disks_) throw std::out_of_range("Invalid disk index");
    disk_active_[disk_index] = false;
}

bool VirtualRaid::is_disk_active(size_t disk_index) const {
    return disk_index < disk_active_.size() && disk_active_[disk_index];
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

        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);
        // Bounds-check the bytes actually requested, not the rest of the
        // block: when a member's size is not a multiple of block_size_, the
        // old full-block check rejected reads of valid tail bytes (last
        // member sector is not the last stripe byte).
        if (disk_offset + read_len > memberPayloadBytes()) {
            throw std::out_of_range("Read exceeds RAID capacity");
        }

        if (!disk_active_[disk_idx] ||
            !readMemberAligned(disk_idx, disk_offset, read_len, &result[res_idx])) {
            throw std::runtime_error("RAID 0 read failed: member unreadable");
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

std::vector<uint8_t> VirtualRaid::read_raid1(size_t offset, size_t length) const {
    const uint64_t payload = memberPayloadBytes();
    if (offset > payload || length > payload - offset) {
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

        // Placement lives in raid_layout.cpp (left-asym default, left-sym mdadm).
        size_t parity_disk = raid_layout::raid5ParityDisk(stripe_index, static_cast<uint32_t>(num_disks_), raid5_algorithm_);
        size_t data_disk = raid_layout::raid5DataDisk(stripe_index, static_cast<uint32_t>(block_in_stripe),
                                                     static_cast<uint32_t>(num_disks_), raid5_algorithm_);

        uint64_t disk_offset = static_cast<uint64_t>(stripe_index) * block_size_ + offset_in_block;
        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);

        const bool dataOk = disk_active_[data_disk] &&
            readMemberAligned(data_disk, disk_offset, read_len, &result[res_idx]);
        if (!dataOk) {
            // XOR-reconstruct: D_failed = P XOR (all other data blocks).
            std::vector<uint8_t> reconstructed(block_size_, 0);
            std::vector<uint8_t> temp(block_size_);
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i == data_disk) continue;
                if (!disk_active_[i] ||
                    !readMemberAligned(i, stripe_index * block_size_, block_size_, temp.data())) {
                    throw std::runtime_error("RAID 5 read failed: multiple disks unreadable");
                }
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

        if (disk_active_[data_disk] &&
            readMemberAligned(data_disk, stripe_base + offset_in_block, read_len, &result[res_idx])) {
            // Direct data hit.
        } else {
            // Count failed *data* disks in this stripe (P/Q loss alone does
            // not affect reads). Treat I/O failure on data_disk as a fail.
            size_t failedData = 0;
            size_t failed[2] = {0, 0};
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i == p_disk || i == q_disk) continue;
                if (!disk_active_[i] || i == data_disk) {
                    if (failedData < 2) failed[failedData] = i;
                    failedData++;
                }
            }

            auto slotOf = [&](size_t disk) -> int {
                int slot = 0;
                for (size_t i = 0; i < num_disks_; ++i) {
                    if (i == p_disk || i == q_disk) continue;
                    if (i == disk) return slot;
                    slot++;
                }
                return -1;
            };

            if (failedData == 1) {
                // Single failure: D = P XOR (other data blocks) — or, when the
                // failed disk is P itself, D = Q' / g^slot from the Q syndrome.
                // Previously the P-lost case threw even though Q alone fully
                // reconstructs one missing block.
                //
                // Integrity guard: zero-fill-on-failure is fine for direct
                // data hits, but a member read that fails DURING reconstruction
                // silently poisons the rebuilt block (0 XOR garbage). Treat a
                // failed member read as another unreadable disk — same policy
                // as the RAID 5 path below.
                std::vector<uint8_t> acc(block_size_, 0), temp(block_size_);
                bool pOk = disk_active_[p_disk] &&
                    readMemberAligned(p_disk, stripe_base, block_size_, acc.data());
                if (pOk) {
                    for (size_t i = 0; i < num_disks_; ++i) {
                        if (i == p_disk || i == q_disk || i == failed[0] || !disk_active_[i]) continue;
                        if (!readMemberAligned(i, stripe_base, block_size_, temp.data())) {
                            throw std::runtime_error("RAID 6 read failed: member unreadable during reconstruction");
                        }
                        for (size_t b = 0; b < block_size_; ++b) acc[b] ^= temp[b];
                    }
                } else if (disk_active_[q_disk] &&
                           readMemberAligned(q_disk, stripe_base, block_size_, acc.data())) {
                    for (size_t i = 0; i < num_disks_; ++i) {
                        if (i == p_disk || i == q_disk || i == failed[0] || !disk_active_[i]) continue;
                        if (!readMemberAligned(i, stripe_base, block_size_, temp.data())) {
                            throw std::runtime_error("RAID 6 read failed: member unreadable during reconstruction");
                        }
                        for (size_t b = 0; b < block_size_; ++b) {
                            acc[b] ^= raid6_math::gfMul(temp[b], raid6_math::gfPow(slotOf(i)));
                        }
                    }
                    const uint8_t gf = raid6_math::gfPow(slotOf(failed[0]));
                    for (size_t b = 0; b < block_size_; ++b) acc[b] = raid6_math::gfDiv(acc[b], gf);
                } else {
                    throw std::runtime_error("RAID 6: data block and both parities failed");
                }
                std::memcpy(&result[res_idx], &acc[offset_in_block], read_len);
            } else if (failedData == 2) {
                // Two failures: solve the 2-unknown GF system.
                //   P' = P XOR (healthy data) = X_i XOR X_j
                //   Q' = Q XOR (healthy data * g^slot) = X_i*g^i XOR X_j*g^j
                // where i/j are the failed blocks' stripe slot indices.
                int si = slotOf(failed[0]);
                int sj = slotOf(failed[1]);

                std::vector<uint8_t> pAcc(block_size_, 0), qAcc(block_size_, 0), temp(block_size_);
                if (!disk_active_[p_disk] || !disk_active_[q_disk]) {
                    throw std::runtime_error("RAID 6: too many failures (P or Q also lost)");
                }
                if (!readMemberAligned(p_disk, stripe_base, block_size_, pAcc.data()) ||
                    !readMemberAligned(q_disk, stripe_base, block_size_, qAcc.data())) {
                    throw std::runtime_error("RAID 6 read failed: parity unreadable during reconstruction");
                }
                for (size_t i = 0; i < num_disks_; ++i) {
                    if (i == p_disk || i == q_disk || !disk_active_[i]) continue;
                    if (!readMemberAligned(i, stripe_base, block_size_, temp.data())) {
                        throw std::runtime_error("RAID 6 read failed: member unreadable during reconstruction");
                    }
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

std::vector<uint8_t> VirtualRaid::read_raid1e(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    const uint32_t n = static_cast<uint32_t>(num_disks_);
    while (res_idx < length) {
        const uint64_t dataIndex = offset / block_size_;
        const size_t offset_in_block = offset % block_size_;
        const size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);
        bool ok = false;
        for (uint32_t c = 0; c < 2 && !ok; ++c) {
            const auto loc = raid_layout::raid1eCopy(dataIndex, c, n);
            if (loc.disk >= num_disks_ || !disk_active_[loc.disk]) continue;
            const uint64_t disk_offset = loc.row * block_size_ + offset_in_block;
            ok = readMemberAligned(loc.disk, disk_offset, read_len, &result[res_idx]);
        }
        if (!ok) {
            throw std::runtime_error("RAID 1E read failed: both copies unreadable");
        }
        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

std::vector<uint8_t> VirtualRaid::read_jbod(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t done = 0;
    uint64_t pos = offset;
    for (size_t i = 0; i < num_disks_ && done < length; ++i) {
        const uint64_t plen = memberExtentBytes(i);
        if (pos >= plen) {
            pos -= plen;
            continue;
        }
        const size_t n = static_cast<size_t>(std::min<uint64_t>(length - done, plen - pos));
        if (!disk_active_[i] || !readMemberAligned(i, pos, n, &result[done])) {
            throw std::runtime_error("JBOD read failed: member unreadable");
        }
        done += n;
        pos = 0;
    }
    if (done < length) throw std::out_of_range("Read exceeds RAID capacity");
    return result;
}

} // namespace byteback
