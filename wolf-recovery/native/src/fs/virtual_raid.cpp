#include "fs/virtual_raid.h"
#include <algorithm>
#include <cstring>

VirtualRaid::VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size)
    : level_(level), num_disks_(drive_indices.size()), disk_size_(0), block_size_(block_size) {
    if (num_disks_ < 2) {
        throw std::invalid_argument("RAID requires at least 2 disks.");
    }
    if (level == RaidLevel::RAID5 && num_disks_ < 3) {
        throw std::invalid_argument("RAID 5 requires at least 3 disks.");
    }

    disk_active_.resize(num_disks_, true);
    for (int idx : drive_indices) {
        auto reader = std::make_shared<wolf::DiskReader>();
        if (!reader->openDrive(idx)) {
            throw std::runtime_error("Failed to open physical drive for RAID.");
        }
        // Assume all disks in array are same size, get size from first one
        if (disk_size_ == 0) {
            disk_size_ = reader->getDiskSize();
        }
        disk_readers_.push_back(reader);
    }
}

void VirtualRaid::write(size_t offset, const std::vector<uint8_t>& data) {
    throw std::runtime_error("Write unsupported on physical RAID mode.");
}

std::vector<uint8_t> VirtualRaid::read(size_t offset, size_t length) const {
    if (length == 0) return std::vector<uint8_t>();
    switch (level_) {
        case RaidLevel::RAID0: return read_raid0(offset, length);
        case RaidLevel::RAID1: return read_raid1(offset, length);
        case RaidLevel::RAID5: return read_raid5(offset, length);
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

    if (level_ == RaidLevel::RAID0) {
        throw std::runtime_error("Cannot reconstruct RAID 0");
    } else if (level_ == RaidLevel::RAID1) {
        size_t source_disk = -1;
        for (size_t i = 0; i < num_disks_; ++i) {
            if (disk_active_[i]) {
                source_disk = i;
                break;
            }
        }
        if (source_disk == static_cast<size_t>(-1)) {
            throw std::runtime_error("No active disks to reconstruct from");
        }
        // For RAID 1, we don't reconstruct physical disks in software.
        throw std::runtime_error("Cannot reconstruct physical RAID 1 disk in-place");
    } else if (level_ == RaidLevel::RAID5) {
        size_t failed_count = 0;
        for (bool active : disk_active_) {
            if (!active) failed_count++;
        }
        if (failed_count > 1) {
            throw std::runtime_error("Cannot reconstruct RAID 5 with > 1 failed disk");
        }

        // For RAID 5, we don't reconstruct physical disks in software in-place.
        throw std::runtime_error("Cannot reconstruct physical RAID 5 disk in-place");
    }
}


void VirtualRaid::write_raid0(size_t offset, const std::vector<uint8_t>& data) {
    throw std::runtime_error("Write unsupported");
}

std::vector<uint8_t> VirtualRaid::read_raid0(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    while (res_idx < length) {
        size_t block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;
        size_t disk_idx = block_index % num_disks_;
        size_t block_on_disk = block_index / num_disks_;
        size_t disk_offset = block_on_disk * block_size_ + offset_in_block;

        if (disk_offset >= disk_size_) {
            throw std::out_of_range("Read exceeds RAID capacity");
        }
        if (!disk_active_[disk_idx]) {
            throw std::runtime_error("Reading from a failed disk in RAID 0");
        }

        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);
        
        auto res = disk_readers_[disk_idx]->readSectors(disk_offset, read_len, &result[res_idx]);
        if (!res.success) {
            throw std::runtime_error("Failed to read from disk_idx " + std::to_string(disk_idx));
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

void VirtualRaid::write_raid1(size_t offset, const std::vector<uint8_t>& data) {
    throw std::runtime_error("Write unsupported");
}

std::vector<uint8_t> VirtualRaid::read_raid1(size_t offset, size_t length) const {
    if (offset + length > disk_size_) {
        throw std::out_of_range("Read exceeds RAID capacity");
    }
    for (size_t i = 0; i < num_disks_; ++i) {
        if (disk_active_[i]) {
            std::vector<uint8_t> result(length);
            auto res = disk_readers_[i]->readSectors(offset, length, result.data());
            if (res.success) return result;
        }
    }
    throw std::runtime_error("No active disks to read from in RAID 1");
}

void VirtualRaid::write_raid5(size_t offset, const std::vector<uint8_t>& data) {
    throw std::runtime_error("Write unsupported");
}

std::vector<uint8_t> VirtualRaid::read_raid5(size_t offset, size_t length) const {
    std::vector<uint8_t> result(length);
    size_t res_idx = 0;
    while (res_idx < length) {
        size_t logical_block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;
        
        size_t stripe_index = logical_block_index / (num_disks_ - 1);
        size_t block_in_stripe = logical_block_index % (num_disks_ - 1);
        
        size_t parity_disk = (num_disks_ - 1) - (stripe_index % num_disks_);
        size_t data_disk = block_in_stripe;
        if (data_disk >= parity_disk) {
            data_disk++;
        }

        size_t disk_offset = stripe_index * block_size_ + offset_in_block;
        if (disk_offset >= disk_size_) {
            throw std::out_of_range("Read exceeds RAID capacity");
        }

        size_t read_len = std::min(length - res_idx, block_size_ - offset_in_block);

        if (disk_active_[data_disk]) {
            auto res = disk_readers_[data_disk]->readSectors(disk_offset, read_len, &result[res_idx]);
            if (!res.success) throw std::runtime_error("RAID 5 direct read failed");
        } else {
            std::vector<uint8_t> reconstructed_block(block_size_, 0);
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i != data_disk) {
                    if (!disk_active_[i]) {
                        throw std::runtime_error("RAID 5 read failed: multiple disks failed");
                    }
                    size_t block_start = stripe_index * block_size_;
                    
                    std::vector<uint8_t> temp_block(block_size_);
                    auto res = disk_readers_[i]->readSectors(block_start, block_size_, temp_block.data());
                    if (!res.success) throw std::runtime_error("RAID 5 block reconstruct read failed");
                    
                    for (size_t b = 0; b < block_size_; ++b) {
                        reconstructed_block[b] ^= temp_block[b];
                    }
                }
            }
            std::memcpy(&result[res_idx], &reconstructed_block[offset_in_block], read_len);
        }

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}
