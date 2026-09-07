// RAID geometry autodetection — parity-consistency scoring.
// See include/fs/raid_detect.h for the contract.
//
// ponytail: candidate set (stripe sizes {64KiB..1MiB}, data offsets
// {0, 1MiB}) is the common hardware-controller set, extendable — add
// entries to kStripeSizes / kDataOffsetSectors when a real controller
// shows up with something else.
#include "fs/raid_detect.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "byteback_io.h"
#include "fs/raid_layout.h"

namespace byteback {
namespace {

constexpr size_t kMaxSamples = 64;  // deterministic spread across the span
constexpr size_t kMinSamples = 8;   // below this only a perfect score passes
constexpr size_t kTinySamples = 3;  // absolute floor for tiny images
constexpr double kRaid1Floor = 0.95;
constexpr double kParityFloor = 0.90;
constexpr size_t kRaid1Chunk = 64 * 1024;
constexpr uint64_t kSector = 512;

// Common controller stripe sizes, largest first.
const size_t kStripeSizes[] = {
    1 * 1024 * 1024, 512 * 1024, 256 * 1024, 128 * 1024, 64 * 1024,
};
// Per-member start reservation, 1MiB-aligned (0 = no reservation).
const uint64_t kDataOffsetSectors[] = {0, 2048};

bool readBlock(DiskReader* r, uint64_t offset, uint32_t len, std::vector<uint8_t>& buf) {
    buf.resize(len);
    const ReadResult res = r->readSectors(offset, len, buf.data());
    return res.success && res.bytesRead == len;
}

// Samples below kMinSamples come from tiny images — no slack, require a
// perfect score. Above it the level's floor applies.
bool passesConfidence(double conf, size_t samples, double floorLevel) {
    if (samples < kTinySamples) return false;
    return conf >= (samples < kMinSamples ? 1.0 : floorLevel);
}

bool xorEquals(const std::vector<size_t>& idxs, const std::vector<std::vector<uint8_t>>& blocks,
               const uint8_t* expected) {
    const size_t len = blocks[0].size();
    for (size_t b = 0; b < len; ++b) {
        uint8_t x = 0;
        for (size_t i : idxs) x ^= blocks[i][b];
        if (x != expected[b]) return false;
    }
    return true;
}

bool raid5Consistent(uint64_t si, uint32_t n, const std::vector<std::vector<uint8_t>>& blocks) {
    const uint32_t p = raid_layout::raid5ParityDisk(si, n);
    std::vector<size_t> data;
    data.reserve(n - 1);
    for (uint32_t i = 0; i < n; ++i)
        if (i != p) data.push_back(i);
    return xorEquals(data, blocks, blocks[p].data());
}

bool raid6Consistent(uint64_t si, uint32_t n, const std::vector<std::vector<uint8_t>>& blocks) {
    const auto pq = raid_layout::raid6Disks(si, n);
    std::vector<size_t> data;
    data.reserve(n - 2);
    for (uint32_t i = 0; i < n; ++i)
        if (i != pq.pDisk && i != pq.qDisk) data.push_back(i);
    if (!xorEquals(data, blocks, blocks[pq.pDisk].data())) return false;
    // Q = XOR of g^slot * data, slot = data index in disk order — exactly how
    // virtual_raid.cpp builds Q, so scoring matches the read path.
    const size_t len = blocks[0].size();
    for (size_t b = 0; b < len; ++b) {
        uint8_t q = 0;
        for (uint32_t j = 0; j + 2 < n; ++j) {
            const uint32_t disk = raid_layout::raid6DataDisk(si, j, n);
            q ^= raid6_math::gfMul(blocks[disk][b], raid6_math::gfPow(static_cast<int>(j)));
        }
        if (q != blocks[pq.qDisk][b]) return false;
    }
    return true;
}

} // namespace

bool detectRaidGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    const size_t n = members.size();
    if (n < 2) return false;

    uint64_t memberSize = UINT64_MAX;
    for (const DiskReader* m : members) memberSize = std::min(memberSize, m->getDiskSize());
    if (memberSize == UINT64_MAX || memberSize < kSector) return false;

    // Tie-break rule: highest confidence wins; then the LARGER stripe size
    // (smaller stripes score spuriously on repetitive data); then offset 0
    // (no reservation is the simpler layout). Levels are evaluated mirror ->
    // RAID5 -> RAID6 and a full tie keeps the earlier (least-redundant)
    // assumption — e.g. all-zero members detect as RAID1, not RAID5.
    // RAID0 is never emitted: it has no parity, nothing here can score it.
    bool have = false;
    auto consider = [&](const RaidGeometry& c) {
        if (!have) {
            out = c;
            have = true;
            return;
        }
        if (c.confidence > out.confidence) {
            out = c;
            return;
        }
        if (c.confidence < out.confidence || c.level != out.level) return;
        if (c.blockSize > out.blockSize) {
            out = c;
            return;
        }
        if (c.blockSize < out.blockSize) return;
        if (c.dataOffsetSectors < out.dataOffsetSectors) out = c;
    };

    // --- RAID1: members byte-identical, sampled across the disk. ----------
    if (memberSize >= kSector) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<uint64_t>(kRaid1Chunk, (memberSize / kSector) * kSector));
        const size_t regions = static_cast<size_t>(memberSize / chunk);
        const size_t nSamples = std::min(kMaxSamples, std::max(regions, size_t{1}));
        std::vector<std::vector<uint8_t>> bufs(n);
        size_t consistent = 0, total = 0;
        for (size_t k = 0; k < nSamples; ++k) {
            const uint64_t off =
                nSamples > 1 ? (memberSize - chunk) * k / (nSamples - 1) : 0;
            bool ok = true;
            for (size_t m = 0; m < n && ok; ++m) ok = readBlock(members[m], off, chunk, bufs[m]);
            if (!ok) continue;
            ++total;
            bool eq = true;
            for (size_t m = 1; m < n && eq; ++m)
                eq = std::memcmp(bufs[0].data(), bufs[m].data(), chunk) == 0;
            if (eq) ++consistent;
        }
        if (total > 0) {
            const double conf = static_cast<double>(consistent) / static_cast<double>(total);
            if (passesConfidence(conf, total, kRaid1Floor))
                consider(RaidGeometry{RaidLevel::RAID1, 0, 0, conf}); // blockSize N/A for mirrors
        }
    }

    // --- RAID5 / RAID6: parity-consistency per candidate geometry. --------
    // ponytail: RAID5 P-syndrome has a known ceiling — XOR identity is
    // byte-wise, so EVERY candidate whose stripes align to whole true stripes
    // (any multiple or divisor of the real stripe size) scores 1.0; stripe
    // size is therefore a documented guess (tie-break: largest). RAID6 is
    // pinned exactly because the g^slot Q-syndrome breaks for wrong stripe
    // sizes. Upgrade path: score candidate geometries by filesystem-signature
    // consistency in the assembled view, like commercial reconstructors do.
    auto evalParity = [&](RaidLevel level, size_t stripeBytes) {
        for (uint64_t offSectors : kDataOffsetSectors) {
            const uint64_t offBytes = offSectors * kSector;
            if (offBytes + stripeBytes > memberSize) continue;
            const uint64_t nStripes = (memberSize - offBytes) / stripeBytes;
            const size_t nSamples = static_cast<size_t>(std::min<uint64_t>(kMaxSamples, nStripes));
            std::vector<std::vector<uint8_t>> blocks(n);
            size_t consistent = 0, total = 0;
            for (size_t k = 0; k < nSamples; ++k) {
                const uint64_t si = k * nStripes / nSamples;
                const uint64_t off = offBytes + si * stripeBytes;
                bool ok = true;
                for (size_t m = 0; m < n && ok; ++m)
                    ok = readBlock(members[m], off, static_cast<uint32_t>(stripeBytes), blocks[m]);
                if (!ok) continue; // skip samples with any failed member read
                ++total;
                const bool good = (level == RaidLevel::RAID5)
                                      ? raid5Consistent(si, static_cast<uint32_t>(n), blocks)
                                      : raid6Consistent(si, static_cast<uint32_t>(n), blocks);
                if (good) ++consistent;
            }
            if (total == 0) continue;
            const double conf = static_cast<double>(consistent) / static_cast<double>(total);
            if (!passesConfidence(conf, total, kParityFloor)) continue;
            consider(RaidGeometry{level, stripeBytes, offSectors, conf});
        }
    };

    if (n >= 3)
        for (size_t sb : kStripeSizes) evalParity(RaidLevel::RAID5, sb);
    if (n >= 4)
        for (size_t sb : kStripeSizes) evalParity(RaidLevel::RAID6, sb);

    return have;
}

} // namespace byteback
