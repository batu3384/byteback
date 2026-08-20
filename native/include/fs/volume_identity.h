#pragma once

#include "byteback_io.h"
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace byteback {

struct VolumeIdentity {
    uint64_t serial = 0;
    uint64_t sizeBytes = 0;
    uint64_t offsetBytes = 0;
};

// NTFS 0x48 (8), FAT32 0x43 (4), FAT16 0x27 (4), exFAT 0x64 (4). Zero if unknown.
uint64_t parseVolumeSerial(const uint8_t* boot, size_t n);

// Boot serials at LBA 0 and every MBR/GPT partition start on this evidence disk.
std::unordered_set<uint64_t> collectVolumeSerials(DiskReader& reader);

std::vector<VolumeIdentity> collectVolumeIdentities(DiskReader& reader);

// Serial must match. If both sizes known and differ, reject (same serial, other volume).
bool volumeIdentityMatches(const VolumeIdentity& evidence, uint64_t snapSerial, uint64_t snapSizeBytes);

} // namespace byteback
