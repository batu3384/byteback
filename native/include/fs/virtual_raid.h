#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>

enum class RaidLevel {
    RAID0,
    RAID1,
    RAID5
};

class VirtualRaid {
public:
    VirtualRaid(RaidLevel level, size_t num_disks, size_t disk_size, size_t block_size);

    void write(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read(size_t offset, size_t length) const;

    void fail_disk(size_t disk_index);
    void reconstruct_disk(size_t disk_index);

    const std::vector<uint8_t>& get_disk_content(size_t disk_index) const;

private:
    RaidLevel level_;
    size_t num_disks_;
    size_t disk_size_;
    size_t block_size_;
    std::vector<std::vector<uint8_t>> disks_;
    std::vector<bool> disk_active_;

    void write_raid0(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid0(size_t offset, size_t length) const;

    void write_raid1(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid1(size_t offset, size_t length) const;

    void write_raid5(size_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_raid5(size_t offset, size_t length) const;

    void update_parity_raid5(size_t stripe_index);
};

