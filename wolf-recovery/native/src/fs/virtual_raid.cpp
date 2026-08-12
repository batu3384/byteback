#include "fs/virtual_raid.h"
#include <algorithm>
#include <cstring>

VirtualRaid::VirtualRaid(RaidLevel level, size_t num_disks, size_t disk_size, size_t block_size)
    : level_(level), num_disks_(num_disks), disk_size_(disk_size), block_size_(block_size) {
    if (num_disks < 2) {
        throw std::invalid_argument("RAID requires at least 2 disks.");
    }
    if (level == RaidLevel::RAID5 && num_disks < 3) {
        throw std::invalid_argument("RAID 5 requires at least 3 disks.");
    }
    if (disk_size % block_size != 0) {
        throw std::invalid_argument("Disk size must be a multiple of block size.");
    }

    disks_.resize(num_disks, std::vector<uint8_t>(disk_size, 0));
    disk_active_.resize(num_disks, true);
}

void VirtualRaid::write(size_t offset, const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    switch (level_) {
        case RaidLevel::RAID0: write_raid0(offset, data); break;
        case RaidLevel::RAID1: write_raid1(offset, data); break;
        case RaidLevel::RAID5: write_raid5(offset, data); break;
    }
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
    std::fill(disks_[disk_index].begin(), disks_[disk_index].end(), 0);
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
        disks_[disk_index] = disks_[source_disk];
        disk_active_[disk_index] = true;
    } else if (level_ == RaidLevel::RAID5) {
        size_t failed_count = 0;
        for (bool active : disk_active_) {
            if (!active) failed_count++;
        }
        if (failed_count > 1) {
            throw std::runtime_error("Cannot reconstruct RAID 5 with > 1 failed disk");
        }

        std::fill(disks_[disk_index].begin(), disks_[disk_index].end(), 0);
        for (size_t i = 0; i < num_disks_; ++i) {
            if (i != disk_index && disk_active_[i]) {
                for (size_t j = 0; j < disk_size_; ++j) {
                    disks_[disk_index][j] ^= disks_[i][j];
                }
            }
        }
        disk_active_[disk_index] = true;
    }
}

const std::vector<uint8_t>& VirtualRaid::get_disk_content(size_t disk_index) const {
    if (disk_index >= num_disks_) throw std::out_of_range("Invalid disk index");
    return disks_[disk_index];
}

void VirtualRaid::write_raid0(size_t offset, const std::vector<uint8_t>& data) {
    size_t data_idx = 0;
    while (data_idx < data.size()) {
        size_t block_index = offset / block_size_;
        size_t offset_in_block = offset % block_size_;
        size_t disk_idx = block_index % num_disks_;
        size_t block_on_disk = block_index / num_disks_;
        size_t disk_offset = block_on_disk * block_size_ + offset_in_block;

        if (disk_offset >= disk_size_) {
            throw std::out_of_range("Write exceeds RAID capacity");
        }
        if (!disk_active_[disk_idx]) {
            throw std::runtime_error("Writing to a failed disk in RAID 0");
        }

        size_t write_len = std::min(data.size() - data_idx, block_size_ - offset_in_block);
        std::memcpy(&disks_[disk_idx][disk_offset], &data[data_idx], write_len);

        data_idx += write_len;
        offset += write_len;
    }
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
        std::memcpy(&result[res_idx], &disks_[disk_idx][disk_offset], read_len);

        res_idx += read_len;
        offset += read_len;
    }
    return result;
}

void VirtualRaid::write_raid1(size_t offset, const std::vector<uint8_t>& data) {
    if (offset + data.size() > disk_size_) {
        throw std::out_of_range("Write exceeds RAID capacity");
    }
    for (size_t i = 0; i < num_disks_; ++i) {
        if (disk_active_[i]) {
            std::memcpy(&disks_[i][offset], data.data(), data.size());
        }
    }
}

std::vector<uint8_t> VirtualRaid::read_raid1(size_t offset, size_t length) const {
    if (offset + length > disk_size_) {
        throw std::out_of_range("Read exceeds RAID capacity");
    }
    for (size_t i = 0; i < num_disks_; ++i) {
        if (disk_active_[i]) {
            std::vector<uint8_t> result(length);
            std::memcpy(result.data(), &disks_[i][offset], length);
            return result;
        }
    }
    throw std::runtime_error("No active disks to read from in RAID 1");
}

void VirtualRaid::write_raid5(size_t offset, const std::vector<uint8_t>& data) {
    size_t data_idx = 0;
    while (data_idx < data.size()) {
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
            throw std::out_of_range("Write exceeds RAID capacity");
        }
        
        size_t write_len = std::min(data.size() - data_idx, block_size_ - offset_in_block);
        
        if (disk_active_[data_disk]) {
            std::memcpy(&disks_[data_disk][disk_offset], &data[data_idx], write_len);
        }
        
        update_parity_raid5(stripe_index);

        data_idx += write_len;
        offset += write_len;
    }
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
            std::memcpy(&result[res_idx], &disks_[data_disk][disk_offset], read_len);
        } else {
            std::vector<uint8_t> reconstructed_block(block_size_, 0);
            for (size_t i = 0; i < num_disks_; ++i) {
                if (i != data_disk) {
                    if (!disk_active_[i]) {
                        throw std::runtime_error("RAID 5 read failed: multiple disks failed");
                    }
                    size_t block_start = stripe_index * block_size_;
                    for (size_t b = 0; b < block_size_; ++b) {
                        reconstructed_block[b] ^= disks_[i][block_start + b];
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

void VirtualRaid::update_parity_raid5(size_t stripe_index) {
    size_t parity_disk = (num_disks_ - 1) - (stripe_index % num_disks_);
    size_t block_start = stripe_index * block_size_;
    
    if (!disk_active_[parity_disk]) {
        return; 
    }

    std::fill(&disks_[parity_disk][block_start], &disks_[parity_disk][block_start] + block_size_, 0);
    
    for (size_t i = 0; i < num_disks_; ++i) {
        if (i != parity_disk) {
            if (disk_active_[i]) {
                for (size_t b = 0; b < block_size_; ++b) {
                    disks_[parity_disk][block_start + b] ^= disks_[i][block_start + b];
                }
            } else {
                throw std::runtime_error("Writing to degraded RAID 5 is not supported in this simple simulation");
            }
        }
    }
}
