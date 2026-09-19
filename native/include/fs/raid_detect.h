#pragma once

// RAID geometry autodetection over member disk images. For each candidate
// (level, stripe size, parity start/rotation) the detector samples stripes
// and scores parity consistency:
//   RAID5 — XOR of data blocks == P block
//   RAID6 — XOR == P and GF(2^8) Reed-Solomon syndrome == Q
//   RAID10 — adjacent mirror pairs; stripe pinned by FS magics only
//   RAID1 — member contents identical
// Best-scoring candidate wins; RAID5 stripe-size ties are broken by
// filesystem-signature score on the assembled view (MBR/NTFS/FAT/ext).
// Below the confidence floor the result is "unknown" (honest fail, never
// a guess — a wrong geometry silently interleaves unrelated sectors, which
// is worse than no array).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "fs/virtual_raid.h" // RaidLevel
#include "fs/raid_layout.h"

namespace byteback {

class DiskReader;

struct RaidGeometry {
    RaidLevel level = RaidLevel::RAID0;
    size_t blockSize = 0;          // stripe/chunk size in bytes
    uint64_t dataOffsetSectors = 0; // per-member start offset (512B sectors)
    double confidence = 0.0;        // 0..1 parity-consistency score
    bool fsConfirmed = false;       // assembled view has MBR/NTFS/FAT/ext magic
    raid_layout::Raid5Algorithm raid5Algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric;
    // Slot permutation of the input member list used for this geometry.
    // Identity when user order already scores. Empty means identity.
    std::vector<size_t> memberOrder;
};

// members must be opened and equal-sized (caller validates sizes).
// Returns false when no candidate reaches the confidence floor.
bool detectRaidGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out);

/** Open local evidence files and score geometry. Rejects devices/http/dev. */
bool detectRaidFromEvidencePaths(const std::vector<std::string>& paths, RaidGeometry& out);

// mdadm super1 (magic 0xa92b4efc). 1.2 at 4KiB, 1.1 at LBA 0, 1.0 8KiB from end.
// kernel offsetof: data_offset=128, utime=192, sizeof=256.
struct MdadmSuper1 {
    int32_t level = 0;
    uint32_t kernelLayout = 0;
    uint32_t chunkSectors = 0;
    uint32_t raidDisks = 0;
    uint64_t dataOffsetSectors = 0;
    uint32_t devNumber = 0;
    uint16_t role = 0xffff;
    uint8_t setUuid[16]{};
};

bool parseMdadmSuper1(const uint8_t* buf, size_t n, MdadmSuper1& out);
bool readMdadmSuper1(DiskReader& reader, MdadmSuper1& out);
raid_layout::Raid5Algorithm raid5AlgoFromMdadmLayout(uint32_t kernelLayout);

// Intel IMSM MPB — mdadm super-intel.c. Anchor at dsize-2*512.
// Signature "Intel Raid ISM Cfg Sig. "; checksum = sum of le32 words minus field.
struct ImsmSuper {
    uint32_t familyNum = 0;
    uint8_t numDisks = 0;
    uint8_t numRaidDevs = 0;
    int32_t level = -1;
    uint32_t chunkSectors = 0;
    uint64_t dataOffsetSectors = 0;
    uint32_t numMembers = 0;
    std::vector<uint32_t> diskOrd;
};

bool parseImsmSuper(const uint8_t* buf, size_t n, ImsmSuper& out);
bool readImsmSuper(DiskReader& reader, ImsmSuper& out);

// SNIA DDF — mdadm super-ddf.c. Magic BE 0xDE11DE11 at last 512 (anchor).
// CRC-32 IEEE with crc field held at 0xFFFFFFFF (zlib crc32(0,...)).
struct DdfHeader {
    uint64_t primaryLba = 0;
    uint32_t configSectionOffset = 0;
    uint32_t configSectionLength = 0;
    uint8_t type = 0xFF;
    uint8_t guid[24]{};
};

struct DdfSuper {
    int32_t level = -1;
    uint8_t rlq = 0;
    uint32_t chunkSectors = 0;
    uint32_t numMembers = 0;
    uint8_t guid[24]{};
};

bool parseDdfHeader(const uint8_t* buf, size_t n, DdfHeader& out);
bool readDdfSuper(DiskReader& reader, DdfSuper& out);

} // namespace byteback
