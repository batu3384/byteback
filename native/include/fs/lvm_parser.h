#pragma once

// LVM2 PV label + LV map. Linear (`stripe_count=1`), striped (2..8
// `type=striped`), `type=mirror`, and modern `type=raid`
// (`raid1`/`raid0`/`raid5`/`raid5_ls|la|ra|rs`/`raid6*`/`raid10*`) via VirtualRaid.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "fs/raid_layout.h"

namespace byteback {

class DiskReader;
class VirtualRaid;

struct Lvm2Pv {
    uint64_t labelSector = 1;
    uint64_t peStartBytes = 0;
    uint64_t metaOffsetBytes = 0;
    uint64_t metaSizeBytes = 0;
    uint8_t uuid[32]{};
};

struct Lvm2StripeCol {
    std::string pvName;
    uint64_t startPe = 0;
    uint8_t pvUuid[32]{};
};

struct Lvm2LinearLv {
    std::string name;
    bool mirror = false;
    bool raid5 = false;
    bool raid6 = false;
    bool raid10 = false;
    raid_layout::Raid5Algorithm raid5Algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric;
    uint32_t stripeCount = 1;
    uint64_t stripeSizeSectors = 0;
    uint64_t extentSizeSectors = 0;
    uint64_t dataStartBytes = 0;
    uint64_t sizeBytes = 0;
    std::vector<Lvm2StripeCol> stripes;
};

bool parseLvm2PvLabel(const uint8_t* sector, size_t n, uint64_t expectedSector, Lvm2Pv& out);
bool readLvm2Pv(DiskReader& reader, uint64_t pvOffsetBytes, Lvm2Pv& out);
bool parseLvm2LinearLvs(const char* text, size_t n, const Lvm2Pv& pv, std::vector<Lvm2LinearLv>& out);
bool listLvm2LinearLvs(DiskReader& reader, uint64_t pvOffsetBytes, std::vector<Lvm2LinearLv>& out);

struct Lvm2PvExtent {
    DiskReader* disk = nullptr;
    uint64_t offsetBytes = 0;
};

std::shared_ptr<VirtualRaid> assembleLvmStripedLv(DiskReader& disk, const std::vector<uint64_t>& pvOffsets,
                                                 const Lvm2LinearLv& lv);
std::shared_ptr<VirtualRaid> assembleLvmStripedLv(const std::vector<Lvm2PvExtent>& pvs, const Lvm2LinearLv& lv);
std::shared_ptr<VirtualRaid> assembleLvmFromOpenDisks(const std::vector<DiskReader*>& disks);
std::shared_ptr<VirtualRaid> assembleLvmFromEvidencePaths(const std::vector<std::string>& paths);

} // namespace byteback
