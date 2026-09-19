#include "fs/lvm_parser.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/raid_layout.h"
#include "fs/virtual_raid.h"
#include "scan_coordinator.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void wrLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void wrLe64(uint8_t* p, uint64_t v) {
    wrLe32(p, static_cast<uint32_t>(v));
    wrLe32(p + 4, static_cast<uint32_t>(v >> 32));
}

uint32_t lvmCrc(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xF597A6CFu;
    for (size_t i = 0; i < n; ++i)
        crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

constexpr uint64_t kPeStartSec = 2048;
constexpr uint64_t kMetaOff = 4096;
constexpr uint64_t kExtentSec = 128;

std::string vgText(uint64_t peStartSec, uint64_t extentSec, uint64_t lvExtents) {
    return std::string(
               "contents = \"Text Format Volume Group\"\n"
               "version = 1\n"
               "vg0 {\n"
               "id = \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"\n"
               "extent_size = ") +
           std::to_string(extentSec) +
           "\n"
           "physical_volumes {\n"
           "pv0 {\n"
           "id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "}\n"
           "logical_volumes {\n"
           "data {\n"
           "segment_count = 1\n"
           "segment1 {\n"
           "start_extent = 0\n"
           "extent_count = " +
           std::to_string(lvExtents) +
           "\n"
           "type = \"striped\"\n"
           "stripe_count = 1\n"
           "stripes = [\n"
           "\"pv0\", 0\n"
           "]\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

void sealLabel(uint8_t* sec) {
    wrLe32(sec + 16, lvmCrc(sec + 20, 492));
}

void fillLabel(uint8_t* sec, uint64_t sector, uint64_t peBytes, uint64_t peSize,
               uint64_t metaOff, uint64_t metaSize, uint64_t devSize, char uuidCh = 'A') {
    std::memset(sec, 0, 512);
    std::memcpy(sec, "LABELONE", 8);
    wrLe64(sec + 8, sector);
    wrLe32(sec + 20, 32);
    std::memcpy(sec + 24, "LVM2 001", 8);
    std::memset(sec + 32, static_cast<unsigned char>(uuidCh), 32);
    wrLe64(sec + 64, devSize);
    wrLe64(sec + 72, peBytes);
    wrLe64(sec + 80, peSize);
    wrLe64(sec + 88, 0);
    wrLe64(sec + 96, 0);
    wrLe64(sec + 104, metaOff);
    wrLe64(sec + 112, metaSize);
    wrLe64(sec + 120, 0);
    wrLe64(sec + 128, 0);
    sealLabel(sec);
}

std::vector<uint8_t> buildLvmPvWithFat16() {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t imgSecs = kPeStartSec + lvExtents * kExtentSec;
    std::vector<uint8_t> img(imgSecs * 512, 0);
    fillLabel(img.data() + 512, 1, peBytes, img.size() - peBytes, kMetaOff, 4096, img.size());
    const std::string text = vgText(kPeStartSec, kExtentSec, lvExtents);
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM metadata text exceeds MDA";
        return {};
    }
    std::memcpy(img.data() + kMetaOff, text.data(), text.size());
    std::memcpy(img.data() + peBytes, fat.data(), fat.size());
    return img;
}

constexpr size_t kStripeBytes = 4096;

std::string vgTextStriped(uint64_t peStartSec, uint64_t extentSec, uint64_t lvExtents, uint64_t stripeSec) {
    return std::string(
               "contents = \"Text Format Volume Group\"\n"
               "version = 1\n"
               "vg0 {\n"
               "extent_size = ") +
           std::to_string(extentSec) +
           "\n"
           "physical_volumes {\n"
           "pv0 {\n"
           "id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "pv1 {\n"
           "id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "}\n"
           "logical_volumes {\n"
           "data {\n"
           "segment_count = 1\n"
           "segment1 {\n"
           "start_extent = 0\n"
           "extent_count = " +
           std::to_string(lvExtents) +
           "\n"
           "type = \"striped\"\n"
           "stripe_count = 2\n"
           "stripe_size = " +
           std::to_string(stripeSec) +
           "\n"
           "stripes = [\n"
           "\"pv0\", 0\n"
           "\"pv1\", 0\n"
           "]\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

void plantRaid0(const std::vector<uint8_t>& fat, size_t stripe, std::vector<uint8_t>& a, uint64_t aOff,
                std::vector<uint8_t>& b, uint64_t bOff) {
    size_t si = 0;
    for (size_t off = 0; off < fat.size(); off += stripe, ++si) {
        const size_t n = std::min(stripe, fat.size() - off);
        std::vector<uint8_t>& dest = (si % 2 == 0) ? a : b;
        const uint64_t base = (si % 2 == 0) ? aOff : bOff;
        const uint64_t dst = base + (si / 2) * stripe;
        if (dst + n > dest.size()) return;
        std::memcpy(dest.data() + dst, fat.data() + off, n);
    }
}

bool fillLvmStripedTwoPvs(std::vector<uint8_t>& pv0, std::vector<uint8_t>& pv1) {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t pvSecs = kPeStartSec + lvExtents * kExtentSec;
    pv0.assign(pvSecs * 512, 0);
    pv1.assign(pvSecs * 512, 0);
    fillLabel(pv0.data() + 512, 1, peBytes, pv0.size() - peBytes, kMetaOff, 4096, pv0.size(), 'A');
    fillLabel(pv1.data() + 512, 1, peBytes, pv1.size() - peBytes, kMetaOff, 4096, pv1.size(), 'B');
    const std::string text = vgTextStriped(kPeStartSec, kExtentSec, lvExtents, kStripeBytes / 512);
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM striped metadata exceeds MDA";
        pv0.clear();
        pv1.clear();
        return false;
    }
    std::memcpy(pv0.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv1.data() + kMetaOff, text.data(), text.size());
    plantRaid0(fat, kStripeBytes, pv0, peBytes, pv1, peBytes);
    return true;
}

std::vector<uint8_t> packTwoGptLinuxLvmPvs(const std::vector<uint8_t>& pv0, const std::vector<uint8_t>& pv1) {
    if (pv0.empty() || pv1.empty() || pv0.size() % 512 != 0 || pv1.size() % 512 != 0) return {};
    const uint64_t pv0Secs = pv0.size() / 512;
    const uint64_t pv1Secs = pv1.size() / 512;
    constexpr uint64_t part0 = 40;
    const uint64_t part1 = part0 + pv0Secs;
    std::vector<uint8_t> disk((part1 + pv1Secs) * 512, 0);
    uint8_t* h = disk.data() + 512;
    std::memcpy(h, "EFI PART", 8);
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
    auto writeEnt = [&](uint8_t* e, uint64_t start, uint64_t end) {
        uint32_t guid1 = 0xE6D6D379;
        std::memcpy(e, &guid1, 4);
        e[4] = 1;
        std::memcpy(e + 32, &start, 8);
        std::memcpy(e + 40, &end, 8);
    };
    writeEnt(disk.data() + 2 * 512, part0, part1 - 1);
    writeEnt(disk.data() + 2 * 512 + 128, part1, part1 + pv1Secs - 1);
    std::memcpy(disk.data() + part0 * 512, pv0.data(), pv0.size());
    std::memcpy(disk.data() + part1 * 512, pv1.data(), pv1.size());
    return disk;
}

std::vector<uint8_t> buildLvmStripedTwoPvFat16() {
    std::vector<uint8_t> pv0, pv1;
    if (!fillLvmStripedTwoPvs(pv0, pv1)) return {};
    return packTwoGptLinuxLvmPvs(pv0, pv1);
}

std::string vgTextMirror(uint64_t peStartSec, uint64_t extentSec, uint64_t lvExtents) {
    return std::string(
               "contents = \"Text Format Volume Group\"\n"
               "version = 1\n"
               "vg0 {\n"
               "extent_size = ") +
           std::to_string(extentSec) +
           "\n"
           "physical_volumes {\n"
           "pv0 {\n"
           "id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "pv1 {\n"
           "id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "}\n"
           "logical_volumes {\n"
           "data {\n"
           "segment_count = 1\n"
           "segment1 {\n"
           "start_extent = 0\n"
           "extent_count = " +
           std::to_string(lvExtents) +
           "\n"
           "type = \"mirror\"\n"
           "mirror_count = 2\n"
           "mirrors = [\n"
           "\"pv0\", 0\n"
           "\"pv1\", 0\n"
           "]\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

std::vector<uint8_t> buildLvmMirrorTwoPvFat16() {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t pvSecs = kPeStartSec + lvExtents * kExtentSec;
    std::vector<uint8_t> pv0(pvSecs * 512, 0);
    std::vector<uint8_t> pv1(pvSecs * 512, 0);
    fillLabel(pv0.data() + 512, 1, peBytes, pv0.size() - peBytes, kMetaOff, 4096, pv0.size(), 'A');
    fillLabel(pv1.data() + 512, 1, peBytes, pv1.size() - peBytes, kMetaOff, 4096, pv1.size(), 'B');
    const std::string text = vgTextMirror(kPeStartSec, kExtentSec, lvExtents);
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM mirror metadata exceeds MDA";
        return {};
    }
    std::memcpy(pv0.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv1.data() + kMetaOff, text.data(), text.size());
    if (peBytes + fat.size() > pv0.size()) {
        ADD_FAILURE() << "LVM mirror PE too small for FAT16";
        return {};
    }
    std::memcpy(pv0.data() + peBytes, fat.data(), fat.size());
    std::memcpy(pv1.data() + peBytes, fat.data(), fat.size());

    constexpr uint64_t part0 = 40;
    const uint64_t part1 = part0 + pvSecs;
    std::vector<uint8_t> disk((part1 + pvSecs) * 512, 0);
    uint8_t* h = disk.data() + 512;
    std::memcpy(h, "EFI PART", 8);
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
    auto writeEnt = [&](uint8_t* e, uint64_t start, uint64_t end) {
        uint32_t guid1 = 0xE6D6D379;
        std::memcpy(e, &guid1, 4);
        e[4] = 1;
        std::memcpy(e + 32, &start, 8);
        std::memcpy(e + 40, &end, 8);
    };
    writeEnt(disk.data() + 2 * 512, part0, part1 - 1);
    writeEnt(disk.data() + 2 * 512 + 128, part1, part1 + pvSecs - 1);
    std::memcpy(disk.data() + part0 * 512, pv0.data(), pv0.size());
    std::memcpy(disk.data() + part1 * 512, pv1.data(), pv1.size());
    return disk;
}

std::string vgTextRaid1(uint64_t peStartSec, uint64_t extentSec, uint64_t lvExtents) {
    return std::string(
               "contents = \"Text Format Volume Group\"\n"
               "version = 1\n"
               "vg0 {\n"
               "extent_size = ") +
           std::to_string(extentSec) +
           "\n"
           "physical_volumes {\n"
           "pv0 {\n"
           "id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "pv1 {\n"
           "id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"\n"
           "pe_start = " +
           std::to_string(peStartSec) +
           "\n"
           "}\n"
           "}\n"
           "logical_volumes {\n"
           "data {\n"
           "segment_count = 1\n"
           "segment1 {\n"
           "start_extent = 0\n"
           "extent_count = " +
           std::to_string(lvExtents) +
           "\n"
           "type = \"raid\"\n"
           "raid_type = \"raid1\"\n"
           "raid_disks = 2\n"
           "stripes = [\n"
           "\"pv0\", 0\n"
           "\"pv1\", 0\n"
           "]\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

std::vector<uint8_t> buildLvmRaidTypeRaid1TwoPvFat16() {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t pvSecs = kPeStartSec + lvExtents * kExtentSec;
    std::vector<uint8_t> pv0(pvSecs * 512, 0);
    std::vector<uint8_t> pv1(pvSecs * 512, 0);
    fillLabel(pv0.data() + 512, 1, peBytes, pv0.size() - peBytes, kMetaOff, 4096, pv0.size(), 'A');
    fillLabel(pv1.data() + 512, 1, peBytes, pv1.size() - peBytes, kMetaOff, 4096, pv1.size(), 'B');
    const std::string text = vgTextRaid1(kPeStartSec, kExtentSec, lvExtents);
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM raid1 metadata exceeds MDA";
        return {};
    }
    std::memcpy(pv0.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv1.data() + kMetaOff, text.data(), text.size());
    if (peBytes + fat.size() > pv0.size()) {
        ADD_FAILURE() << "LVM raid1 PE too small for FAT16";
        return {};
    }
    std::memcpy(pv0.data() + peBytes, fat.data(), fat.size());
    std::memcpy(pv1.data() + peBytes, fat.data(), fat.size());
    return packTwoGptLinuxLvmPvs(pv0, pv1);
}

void plantRaid5Payload(std::vector<uint8_t>& a, std::vector<uint8_t>& b, std::vector<uint8_t>& c,
                       uint64_t off, size_t sb, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t>* imgs[3] = {&a, &b, &c};
    const uint32_t n = 3;
    const auto algo = raid_layout::Raid5Algorithm::LeftSymmetric;
    for (size_t i = 0; i < payload.size();) {
        const size_t logicalBlock = i / sb;
        const size_t inBlk = i % sb;
        const size_t si = logicalBlock / (n - 1);
        const size_t blk = logicalBlock % (n - 1);
        const uint32_t disk = raid_layout::raid5DataDisk(si, static_cast<uint32_t>(blk), n, algo);
        const size_t ncopy = std::min(payload.size() - i, sb - inBlk);
        const uint64_t dst = off + si * sb + inBlk;
        if (dst + ncopy > imgs[disk]->size()) return;
        std::memcpy(imgs[disk]->data() + dst, payload.data() + i, ncopy);
        i += ncopy;
    }
    const uint64_t nStripes = (a.size() > off) ? (a.size() - off) / sb : 0;
    for (uint64_t s = 0; s < nStripes; ++s) {
        const uint32_t p = raid_layout::raid5ParityDisk(s, n, algo);
        std::vector<uint8_t> acc(sb, 0);
        for (uint32_t d = 0; d < n; ++d) {
            if (d == p) continue;
            const uint64_t src = off + s * sb;
            if (src + sb > imgs[d]->size()) return;
            for (size_t k = 0; k < sb; ++k) acc[k] ^= (*imgs[d])[src + k];
        }
        const uint64_t dst = off + s * sb;
        if (dst + sb > imgs[p]->size()) return;
        std::memcpy(imgs[p]->data() + dst, acc.data(), sb);
    }
}

bool fillLvmRaid5ThreePvs(std::vector<uint8_t>& pv0, std::vector<uint8_t>& pv1, std::vector<uint8_t>& pv2) {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t pvSecs = kPeStartSec + lvExtents * kExtentSec;
    pv0.assign(pvSecs * 512, 0);
    pv1.assign(pvSecs * 512, 0);
    pv2.assign(pvSecs * 512, 0);
    fillLabel(pv0.data() + 512, 1, peBytes, pv0.size() - peBytes, kMetaOff, 4096, pv0.size(), 'A');
    fillLabel(pv1.data() + 512, 1, peBytes, pv1.size() - peBytes, kMetaOff, 4096, pv1.size(), 'B');
    fillLabel(pv2.data() + 512, 1, peBytes, pv2.size() - peBytes, kMetaOff, 4096, pv2.size(), 'C');
    const std::string text = std::string(
                                 "contents = \"Text Format Volume Group\"\n"
                                 "version = 1\n"
                                 "vg0 {\n"
                                 "extent_size = ") +
                             std::to_string(kExtentSec) +
                             "\n"
                             "physical_volumes {\n"
                             "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = " +
                             std::to_string(kPeStartSec) +
                             " }\n"
                             "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = " +
                             std::to_string(kPeStartSec) +
                             " }\n"
                             "pv2 { id = \"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\" pe_start = " +
                             std::to_string(kPeStartSec) +
                             " }\n"
                             "}\n"
                             "logical_volumes {\n"
                             "data {\n"
                             "segment_count = 1\n"
                             "segment1 {\n"
                             "start_extent = 0\n"
                             "extent_count = " +
                             std::to_string(lvExtents) +
                             "\n"
                             "type = \"raid\"\n"
                             "raid_type = \"raid5_ls\"\n"
                             "raid_disks = 3\n"
                             "stripe_size = " +
                             std::to_string(kStripeBytes / 512) +
                             "\n"
                             "stripes = [\n"
                             "\"pv0\", 0\n"
                             "\"pv1\", 0\n"
                             "\"pv2\", 0\n"
                             "]\n"
                             "}\n"
                             "}\n"
                             "}\n"
                             "}\n";
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM raid5 metadata exceeds MDA";
        return false;
    }
    std::memcpy(pv0.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv1.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv2.data() + kMetaOff, text.data(), text.size());
    plantRaid5Payload(pv0, pv1, pv2, peBytes, kStripeBytes, fat);
    return true;
}

void plantRaid6Payload(std::vector<uint8_t>& a, std::vector<uint8_t>& b, std::vector<uint8_t>& c,
                       std::vector<uint8_t>& d, uint64_t off, size_t sb, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t>* imgs[4] = {&a, &b, &c, &d};
    const uint32_t n = 4;
    for (size_t i = 0; i < payload.size();) {
        const size_t logicalBlock = i / sb;
        const size_t inBlk = i % sb;
        const size_t si = logicalBlock / (n - 2);
        const size_t blk = logicalBlock % (n - 2);
        const uint32_t disk = raid_layout::raid6DataDisk(si, static_cast<uint32_t>(blk), n);
        const size_t ncopy = std::min(payload.size() - i, sb - inBlk);
        const uint64_t dst = off + si * sb + inBlk;
        if (dst + ncopy > imgs[disk]->size()) return;
        std::memcpy(imgs[disk]->data() + dst, payload.data() + i, ncopy);
        i += ncopy;
    }
    const uint64_t nStripes = (a.size() > off) ? (a.size() - off) / sb : 0;
    for (uint64_t s = 0; s < nStripes; ++s) {
        const auto pq = raid_layout::raid6Disks(s, n);
        std::vector<uint8_t> pAcc(sb, 0);
        std::vector<uint8_t> qAcc(sb, 0);
        for (uint32_t j = 0; j + 2 < n; ++j) {
            const uint32_t disk = raid_layout::raid6DataDisk(s, j, n);
            const uint64_t src = off + s * sb;
            if (src + sb > imgs[disk]->size()) return;
            for (size_t k = 0; k < sb; ++k) {
                pAcc[k] ^= (*imgs[disk])[src + k];
                qAcc[k] ^= raid6_math::gfMul((*imgs[disk])[src + k], raid6_math::gfPow(static_cast<int>(j)));
            }
        }
        const uint64_t dstP = off + s * sb;
        if (dstP + sb > imgs[pq.pDisk]->size() || dstP + sb > imgs[pq.qDisk]->size()) return;
        std::memcpy(imgs[pq.pDisk]->data() + dstP, pAcc.data(), sb);
        std::memcpy(imgs[pq.qDisk]->data() + dstP, qAcc.data(), sb);
    }
}

void plantRaid10Payload(std::vector<uint8_t>& a, std::vector<uint8_t>& b, std::vector<uint8_t>& c,
                        std::vector<uint8_t>& d, uint64_t off, size_t sb, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t>* imgs[4] = {&a, &b, &c, &d};
    const uint32_t n = 4;
    const uint32_t pairs = n / 2;
    for (size_t i = 0; i < payload.size();) {
        const size_t logicalBlock = i / sb;
        const size_t inBlk = i % sb;
        const uint32_t memberA = raid_layout::raid10MemberA(logicalBlock, n);
        const uint32_t memberB = raid_layout::raid10MemberB(logicalBlock, n);
        const size_t blockOnPair = logicalBlock / pairs;
        const size_t ncopy = std::min(payload.size() - i, sb - inBlk);
        const uint64_t dst = off + blockOnPair * sb + inBlk;
        if (dst + ncopy > imgs[memberA]->size() || dst + ncopy > imgs[memberB]->size()) return;
        std::memcpy(imgs[memberA]->data() + dst, payload.data() + i, ncopy);
        std::memcpy(imgs[memberB]->data() + dst, payload.data() + i, ncopy);
        i += ncopy;
    }
}

std::string vgTextRaidFour(const char* raidType, uint64_t lvExtents) {
    return std::string(
               "contents = \"Text Format Volume Group\"\n"
               "version = 1\n"
               "vg0 {\n"
               "extent_size = ") +
           std::to_string(kExtentSec) +
           "\n"
           "physical_volumes {\n"
           "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = " +
           std::to_string(kPeStartSec) +
           " }\n"
           "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = " +
           std::to_string(kPeStartSec) +
           " }\n"
           "pv2 { id = \"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\" pe_start = " +
           std::to_string(kPeStartSec) +
           " }\n"
           "pv3 { id = \"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD\" pe_start = " +
           std::to_string(kPeStartSec) +
           " }\n"
           "}\n"
           "logical_volumes {\n"
           "data {\n"
           "segment_count = 1\n"
           "segment1 {\n"
           "start_extent = 0\n"
           "extent_count = " +
           std::to_string(lvExtents) +
           "\n"
           "type = \"raid\"\n"
           "raid_type = \"" +
           raidType +
           "\"\n"
           "raid_disks = 4\n"
           "stripe_size = " +
           std::to_string(kStripeBytes / 512) +
           "\n"
           "stripes = [\n"
           "\"pv0\", 0\n"
           "\"pv1\", 0\n"
           "\"pv2\", 0\n"
           "\"pv3\", 0\n"
           "]\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

bool fillLvmRaidFourPvs(const char* raidType, std::vector<uint8_t>& pv0, std::vector<uint8_t>& pv1,
                        std::vector<uint8_t>& pv2, std::vector<uint8_t>& pv3) {
    auto fat = byteback::testfix::buildFat16Volume();
    const uint64_t lvExtents = (fat.size() / 512 + kExtentSec - 1) / kExtentSec;
    const uint64_t peBytes = kPeStartSec * 512;
    const uint64_t pvSecs = kPeStartSec + lvExtents * kExtentSec;
    pv0.assign(pvSecs * 512, 0);
    pv1.assign(pvSecs * 512, 0);
    pv2.assign(pvSecs * 512, 0);
    pv3.assign(pvSecs * 512, 0);
    fillLabel(pv0.data() + 512, 1, peBytes, pv0.size() - peBytes, kMetaOff, 4096, pv0.size(), 'A');
    fillLabel(pv1.data() + 512, 1, peBytes, pv1.size() - peBytes, kMetaOff, 4096, pv1.size(), 'B');
    fillLabel(pv2.data() + 512, 1, peBytes, pv2.size() - peBytes, kMetaOff, 4096, pv2.size(), 'C');
    fillLabel(pv3.data() + 512, 1, peBytes, pv3.size() - peBytes, kMetaOff, 4096, pv3.size(), 'D');
    const std::string text = vgTextRaidFour(raidType, lvExtents);
    if (text.size() >= 4096) {
        ADD_FAILURE() << "LVM " << raidType << " metadata exceeds MDA";
        return false;
    }
    std::memcpy(pv0.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv1.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv2.data() + kMetaOff, text.data(), text.size());
    std::memcpy(pv3.data() + kMetaOff, text.data(), text.size());
    if (std::strcmp(raidType, "raid6") == 0) {
        plantRaid6Payload(pv0, pv1, pv2, pv3, peBytes, kStripeBytes, fat);
    } else {
        plantRaid10Payload(pv0, pv1, pv2, pv3, peBytes, kStripeBytes, fat);
    }
    return true;
}

} // namespace

TEST(LvmParser, ParseLvm2LabelReadsPeStart) {
    std::vector<uint8_t> sec(512, 0);
    fillLabel(sec.data(), 1, kPeStartSec * 512, 8 * 1024 * 1024, kMetaOff, 4096, 16 * 1024 * 1024);
    Lvm2Pv pv;
    ASSERT_TRUE(parseLvm2PvLabel(sec.data(), sec.size(), 1, pv));
    EXPECT_EQ(pv.peStartBytes, kPeStartSec * 512);
    EXPECT_EQ(pv.metaOffsetBytes, kMetaOff);
    EXPECT_EQ(pv.metaSizeBytes, 4096u);
}

TEST(LvmParser, ParseLvm2LabelRejectsBadCrc) {
    std::vector<uint8_t> sec(512, 0);
    fillLabel(sec.data(), 1, kPeStartSec * 512, 8 * 1024 * 1024, kMetaOff, 4096, 16 * 1024 * 1024);
    sec[40] ^= 0x01;
    Lvm2Pv pv;
    EXPECT_FALSE(parseLvm2PvLabel(sec.data(), sec.size(), 1, pv));
}

TEST(LvmParser, ParseLvm2TextLinearLv) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text = vgText(kPeStartSec, kExtentSec, 32);
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_EQ(lvs[0].name, "data");
    EXPECT_EQ(lvs[0].dataStartBytes, kPeStartSec * 512);
    EXPECT_EQ(lvs[0].sizeBytes, 32 * kExtentSec * 512);
}

TEST(LvmParser, ParseLvm2TextStripedTwoColumns) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"striped\"\n"
        "stripe_count = 2\n"
        "stripe_size = 8\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_EQ(lvs[0].stripeCount, 2u);
    EXPECT_EQ(lvs[0].stripeSizeSectors, 8u);
    ASSERT_EQ(lvs[0].stripes.size(), 2u);
    EXPECT_EQ(lvs[0].stripes[0].pvName, "pv0");
    EXPECT_EQ(lvs[0].stripes[1].pvName, "pv1");
}

TEST(LvmParser, ParseLvm2TextMirrorTwoLegs) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"mirror\"\n"
        "mirror_count = 2\n"
        "mirrors = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_TRUE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 2u);
    ASSERT_EQ(lvs[0].stripes.size(), 2u);
    EXPECT_EQ(lvs[0].stripes[0].pvName, "pv0");
    EXPECT_EQ(lvs[0].stripes[1].pvName, "pv1");
}

TEST(LvmParser, ParseLvm2TextRaidTypeRaid1) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"raid\"\n"
        "raid_type = \"raid1\"\n"
        "raid_disks = 2\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_TRUE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 2u);
    ASSERT_EQ(lvs[0].stripes.size(), 2u);
    EXPECT_EQ(lvs[0].stripes[0].pvName, "pv0");
    EXPECT_EQ(lvs[0].stripes[1].pvName, "pv1");
}

TEST(LvmParser, ParseLvm2TextRaidTypeRaid0) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"raid\"\n"
        "raid_type = \"raid0\"\n"
        "raid_disks = 2\n"
        "stripe_size = 8\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_FALSE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 2u);
    EXPECT_EQ(lvs[0].stripeSizeSectors, 8u);
    ASSERT_EQ(lvs[0].stripes.size(), 2u);
}

TEST(LvmParser, ParseLvm2TextRaidTypeRaid5Ls) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "pv2 { id = \"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"raid\"\n"
        "raid_type = \"raid5_ls\"\n"
        "raid_disks = 3\n"
        "stripe_size = 8\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "\"pv2\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_TRUE(lvs[0].raid5);
    EXPECT_FALSE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 3u);
    EXPECT_EQ(lvs[0].stripeSizeSectors, 8u);
    EXPECT_EQ(lvs[0].raid5Algorithm, raid_layout::Raid5Algorithm::LeftSymmetric);
    ASSERT_EQ(lvs[0].stripes.size(), 3u);
}

TEST(LvmParser, ParseLvm2TextRaidTypeRaid6) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "pv2 { id = \"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\" pe_start = 2048 }\n"
        "pv3 { id = \"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"raid\"\n"
        "raid_type = \"raid6\"\n"
        "raid_disks = 4\n"
        "stripe_size = 8\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "\"pv2\", 0\n"
        "\"pv3\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_TRUE(lvs[0].raid6);
    EXPECT_FALSE(lvs[0].raid5);
    EXPECT_FALSE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 4u);
    EXPECT_EQ(lvs[0].stripeSizeSectors, 8u);
    ASSERT_EQ(lvs[0].stripes.size(), 4u);
}

TEST(LvmParser, ParseLvm2TextRaidTypeRaid10) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    const std::string text =
        "contents = \"Text Format Volume Group\"\n"
        "vg0 {\n"
        "extent_size = 128\n"
        "physical_volumes {\n"
        "pv0 { id = \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\" pe_start = 2048 }\n"
        "pv1 { id = \"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\" pe_start = 2048 }\n"
        "pv2 { id = \"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\" pe_start = 2048 }\n"
        "pv3 { id = \"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD\" pe_start = 2048 }\n"
        "}\n"
        "logical_volumes {\n"
        "data {\n"
        "segment_count = 1\n"
        "segment1 {\n"
        "start_extent = 0\n"
        "extent_count = 32\n"
        "type = \"raid\"\n"
        "raid_type = \"raid10\"\n"
        "raid_disks = 4\n"
        "stripe_size = 8\n"
        "stripes = [\n"
        "\"pv0\", 0\n"
        "\"pv1\", 0\n"
        "\"pv2\", 0\n"
        "\"pv3\", 0\n"
        "]\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n";
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_TRUE(lvs[0].raid10);
    EXPECT_FALSE(lvs[0].raid5);
    EXPECT_FALSE(lvs[0].mirror);
    EXPECT_EQ(lvs[0].stripeCount, 4u);
    ASSERT_EQ(lvs[0].stripes.size(), 4u);
}

TEST(LvmParser, ParseLvm2TextRejectsStripe) {
    Lvm2Pv pv;
    pv.peStartBytes = kPeStartSec * 512;
    std::string text = vgText(kPeStartSec, kExtentSec, 32);
    const auto pos = text.find("stripe_count = 1");
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, std::strlen("stripe_count = 1"), "stripe_count = 2");
    std::vector<Lvm2LinearLv> lvs;
    EXPECT_FALSE(parseLvm2LinearLvs(text.data(), text.size(), pv, lvs));
}

TEST(LvmParser, ListLinearLvFromPvImage) {
    auto img = buildLvmPvWithFat16();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(listLvm2LinearLvs(reader, 0, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_EQ(lvs[0].dataStartBytes, kPeStartSec * 512);
}

TEST(ScanCoordinator, LinearLvmLvFindsFat16TestTxt) {
    auto img = buildLvmPvWithFat16();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, GptLinuxLvmPartitionFindsFat16TestTxt) {
    auto pv = buildLvmPvWithFat16();
    constexpr uint64_t partStart = 40;
    const uint64_t pvSecs = pv.size() / 512;
    std::vector<uint8_t> disk((partStart + pvSecs) * 512, 0);
    uint8_t* h = disk.data() + 512;
    std::memcpy(h, "EFI PART", 8);
    uint64_t entryLba = 2;
    uint32_t num = 128;
    uint32_t esize = 128;
    std::memcpy(h + 72, &entryLba, 8);
    std::memcpy(h + 80, &num, 4);
    std::memcpy(h + 84, &esize, 4);
    uint8_t* e = disk.data() + 2 * 512;
    uint32_t guid1 = 0xE6D6D379;
    std::memcpy(e, &guid1, 4);
    e[4] = 1;
    uint64_t start = partStart;
    uint64_t end = partStart + pvSecs - 1;
    std::memcpy(e + 32, &start, 8);
    std::memcpy(e + 40, &end, 8);
    std::memcpy(disk.data() + partStart * 512, pv.data(), pv.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, StripedLvmLvFindsFat16TestTxt) {
    auto disk = buildLvmStripedTwoPvFat16();
    ASSERT_FALSE(disk.empty());
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, MirrorLvmLvFindsFat16TestTxt) {
    auto disk = buildLvmMirrorTwoPvFat16();
    ASSERT_FALSE(disk.empty());
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ScanCoordinator, RaidTypeRaid1LvmLvFindsFat16TestTxt) {
    auto disk = buildLvmRaidTypeRaid1TwoPvFat16();
    ASSERT_FALSE(disk.empty());
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LvmParser, AssemblesStripedLvFromTwoSeparatePvReaders) {
    std::vector<uint8_t> img0, img1;
    ASSERT_TRUE(fillLvmStripedTwoPvs(img0, img1));
    DiskReader a, b;
    a.attachMemoryVolume(std::move(img0));
    b.attachMemoryVolume(std::move(img1));
    std::vector<Lvm2LinearLv> lvs;
    ASSERT_TRUE(listLvm2LinearLvs(a, 0, lvs));
    ASSERT_EQ(lvs.size(), 1u);
    EXPECT_EQ(lvs[0].stripeCount, 2u);
    auto raid = assembleLvmStripedLv({{&a, 0}, {&b, 0}}, lvs[0]);
    ASSERT_NE(raid, nullptr);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LvmParser, AssemblesRaid6LvFromFourPvReaders) {
    std::vector<uint8_t> img0, img1, img2, img3;
    ASSERT_TRUE(fillLvmRaidFourPvs("raid6", img0, img1, img2, img3));
    DiskReader a, b, c, d;
    a.attachMemoryVolume(std::move(img0));
    b.attachMemoryVolume(std::move(img1));
    c.attachMemoryVolume(std::move(img2));
    d.attachMemoryVolume(std::move(img3));
    auto raid = assembleLvmFromOpenDisks({&a, &b, &c, &d});
    ASSERT_NE(raid, nullptr);
    EXPECT_EQ(raid->level(), RaidLevel::RAID6);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LvmParser, AssemblesRaid10LvFromFourPvReaders) {
    std::vector<uint8_t> img0, img1, img2, img3;
    ASSERT_TRUE(fillLvmRaidFourPvs("raid10", img0, img1, img2, img3));
    DiskReader a, b, c, d;
    a.attachMemoryVolume(std::move(img0));
    b.attachMemoryVolume(std::move(img1));
    c.attachMemoryVolume(std::move(img2));
    d.attachMemoryVolume(std::move(img3));
    auto raid = assembleLvmFromOpenDisks({&a, &b, &c, &d});
    ASSERT_NE(raid, nullptr);
    EXPECT_EQ(raid->level(), RaidLevel::RAID10);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LvmParser, AssemblesRaid5LsLvFromThreePvReaders) {
    std::vector<uint8_t> img0, img1, img2;
    ASSERT_TRUE(fillLvmRaid5ThreePvs(img0, img1, img2));
    DiskReader a, b, c;
    a.attachMemoryVolume(std::move(img0));
    b.attachMemoryVolume(std::move(img1));
    c.attachMemoryVolume(std::move(img2));
    auto raid = assembleLvmFromOpenDisks({&a, &b, &c});
    ASSERT_NE(raid, nullptr);
    EXPECT_EQ(raid->level(), RaidLevel::RAID5);
    DiskReader lvR;
    lvR.setRaidBackend(raid);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(lvR, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LvmParser, AssemblesStripedLvFromTwoEvidenceFiles) {
    std::vector<uint8_t> img0, img1;
    ASSERT_TRUE(fillLvmStripedTwoPvs(img0, img1));
    const auto p0 = bytebackTestTemp("bb_lvm_pv0", ".img");
    const auto p1 = bytebackTestTemp("bb_lvm_pv1", ".img");
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
    {
        std::ofstream a(p0, std::ios::binary | std::ios::trunc);
        a.write(reinterpret_cast<const char*>(img0.data()), static_cast<std::streamsize>(img0.size()));
        std::ofstream b(p1, std::ios::binary | std::ios::trunc);
        b.write(reinterpret_cast<const char*>(img1.data()), static_cast<std::streamsize>(img1.size()));
        ASSERT_TRUE(a.good());
        ASSERT_TRUE(b.good());
    }
    bool found = false;
    {
        auto raid = assembleLvmFromEvidencePaths({p0.u8string(), p1.u8string()});
        ASSERT_NE(raid, nullptr);
        DiskReader lvR;
        lvR.setRaidBackend(raid);
        std::vector<std::string> names;
        std::atomic<bool> running{true};
        runQuickScan(lvR, [&](const FileRecord& fr) {
            if (!fr.name.empty()) names.push_back(fr.name);
        }, [&](uint64_t, uint64_t) {}, &running, nullptr);
        for (const auto& n : names) {
            if (n.find("TEST") != std::string::npos) found = true;
        }
    }
    EXPECT_TRUE(found);
    std::filesystem::remove(p0);
    std::filesystem::remove(p1);
}

TEST(LvmParser, AssembleLvmFromEvidencePathsRejectsDevice) {
    EXPECT_EQ(assembleLvmFromEvidencePaths({"\\\\.\\PhysicalDrive0", "C:\\cases\\disk.img"}), nullptr);
    EXPECT_EQ(assembleLvmFromEvidencePaths({"/dev/sda", "/tmp/a.img"}), nullptr);
}
