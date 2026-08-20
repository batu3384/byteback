#pragma once

#include "wolf_io.h"
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace wolf {

// NTFS 0x48 (8), FAT32 0x43 (4), FAT16 0x27 (4), exFAT 0x64 (4). Zero if unknown.
uint64_t parseVolumeSerial(const uint8_t* boot, size_t n);

// Boot serials at LBA 0 and every MBR/GPT partition start on this evidence disk.
std::unordered_set<uint64_t> collectVolumeSerials(DiskReader& reader);

} // namespace wolf
