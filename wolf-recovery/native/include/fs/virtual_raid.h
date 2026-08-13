#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <memory>
#include "wolf_io.h"

enum class RaidLevel {
    RAID0,
    RAID1,
    RAID5
};

class VirtualRaid {
public:
    VirtualRaid(RaidLevel level, const std::vector<int>& drive_indices, size_t block_size);

    void write(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read(size_t offset, size_t length) const;

    void fail_disk(size_t disk_index);
    void reconstruct_disk(size_t disk_index);

private:
    RaidLevel level_;
    size_t num_disks_;
    size_t disk_size_;
    size_t block_size_;
    std::vector<std::shared_ptr<wolf::DiskReader>> disk_readers_;
    std::vector<bool> disk_active_;

    void write_raid0(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid0(size_t offset, size_t length) const;

    void write_raid1(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid1(size_t offset, size_t length) const;

    void write_raid5(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid5(size_t offset, size_t length) const;
};

