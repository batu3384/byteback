// RAID geometry autodetection — parity-consistency scoring.
// See include/fs/raid_detect.h for the contract.
//
// ponytail: candidate set (stripe sizes {64KiB..1MiB}, data offsets
// {0, 1MiB}) is the common hardware-controller set, extendable — add
// entries to kStripeSizes / kDataOffsetSectors when a real controller
// shows up with something else.
#include "fs/raid_detect.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "byteback_io.h"
#include "fs/raid_layout.h"
#include "io/volume_mapper_win.h"

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

bool raid5Consistent(uint64_t si, uint32_t n, const std::vector<std::vector<uint8_t>>& blocks,
                     raid_layout::Raid5Algorithm algo) {
    const uint32_t p = raid_layout::raid5ParityDisk(si, n, algo);
    std::vector<size_t> data;
    data.reserve(n - 1);
    for (uint32_t i = 0; i < n; ++i)
        if (i != p) data.push_back(i);
    return xorEquals(data, blocks, blocks[p].data());
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t readBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | readBe32(p + 4);
}

uint32_t crc32Ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool ddfCrcOk(const uint8_t* buf) {
    uint8_t tmp[512];
    std::memcpy(tmp, buf, 512);
    tmp[4] = tmp[5] = tmp[6] = tmp[7] = 0xFF;
    return crc32Ieee(tmp, 512) == readBe32(buf + 4);
}

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint64_t readLe64(const uint8_t* p) {
    return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

constexpr uint32_t kMdadmMagic = 0xA92B4EFCu;
constexpr size_t kSb1Size = 256;

uint32_t mdadmSb1Csum(const uint8_t* sb, size_t n, uint32_t maxDev) {
    const size_t bytes = kSb1Size + static_cast<size_t>(maxDev) * 2;
    if (n < bytes) return 0;
    uint64_t sum = 0;
    int size = static_cast<int>(bytes);
    const uint8_t* p = sb;
    for (; size >= 4; size -= 4, p += 4) {
        if (p == sb + 216) continue; // field zeroed in kernel calc
        sum += readLe32(p);
    }
    if (size == 2) sum += readLe16(p);
    return static_cast<uint32_t>(sum) + static_cast<uint32_t>(sum >> 32);
}

int superblockHits(const std::vector<uint8_t>& sec) {
    if (sec.size() < 512) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    int s = 2;
    const uint16_t bps = static_cast<uint16_t>(sec[11] | (static_cast<uint16_t>(sec[12]) << 8));
    if (bps != 512) return s;
    if (std::memcmp(sec.data() + 3, "NTFS", 4) == 0) s += 4;
    if (std::memcmp(sec.data() + 0x36, "FAT", 3) == 0) s += 4;
    if (std::memcmp(sec.data() + 0x52, "FAT", 3) == 0) s += 4;
    return s;
}

// Assemble a few sectors with the candidate geometry and score boot/superblock
// magics, including MBR partition start LBAs. Breaks RAID5 stripe-size ties
// that XOR identity cannot (aligned multiples stay parity-perfect).
int fsSignatureScore(const std::vector<DiskReader*>& members, const RaidGeometry& g) {
    if (g.blockSize == 0) return 0;
    if (g.level != RaidLevel::RAID5 && g.level != RaidLevel::RAID6 && g.level != RaidLevel::RAID10 &&
        g.level != RaidLevel::RAID1E)
        return 0;
    std::vector<std::shared_ptr<DiskReader>> wrap;
    wrap.reserve(members.size());
    for (DiskReader* m : members)
        wrap.emplace_back(m, [](DiskReader*) {});
    try {
        VirtualRaid raid(g.level, wrap, g.blockSize, g.dataOffsetSectors * kSector, g.raid5Algorithm);
        auto boot = raid.read(0, 512);
        int s = superblockHits(boot);
        if (raid.capacity() >= 1024 + 0x3A) {
            auto ext = raid.read(1024, 512);
            if (ext.size() == 512 && ext[0x38] == 0x53 && ext[0x39] == 0xEF) s += 6;
        }
        if (boot.size() == 512 && boot[510] == 0x55 && boot[511] == 0xAA) {
            // Volume boot (FAT/NTFS superfloppy) is not an MBR; walking
            // bytes 446.. as partitions inflates wrong RAID5 rotations.
            const bool volumeBoot =
                std::memcmp(boot.data() + 3, "NTFS", 4) == 0 ||
                std::memcmp(boot.data() + 0x36, "FAT", 3) == 0 ||
                std::memcmp(boot.data() + 0x52, "FAT", 3) == 0;
            if (!volumeBoot) {
                for (int p = 0; p < 4; ++p) {
                    const uint8_t* pe = boot.data() + 446 + p * 16;
                    if (pe[4] == 0) continue;
                    const uint32_t start = readLe32(pe + 8);
                    if (start == 0) continue;
                    const uint64_t off = static_cast<uint64_t>(start) * kSector;
                    if (off + 512 > raid.capacity()) continue;
                    s += superblockHits(raid.read(off, 512));
                }
            }
        }
        return s;
    } catch (...) {
        return 0;
    }
}

bool scoreRaid1eCopies(const std::vector<DiskReader*>& members, uint64_t memberSize, size_t sb,
                       uint64_t offBytes, double& conf) {
    conf = 0;
    const uint32_t n = static_cast<uint32_t>(members.size());
    if (n < 3 || (n % 2) == 0) return false;
    if (sb == 0 || offBytes + sb > memberSize) return false;
    const uint64_t payload = memberSize - offBytes;
    const uint64_t rows = payload / sb;
    const uint64_t nData = (rows * n) / 2;
    if (nData == 0) return false;
    const size_t nSamples = static_cast<size_t>(std::min<uint64_t>(kMaxSamples, nData));
    std::vector<uint8_t> a, b;
    size_t hits = 0, total = 0;
    for (size_t k = 0; k < nSamples; ++k) {
        const uint64_t di = (nSamples > 1) ? (nData - 1) * k / (nSamples - 1) : 0;
        const auto l0 = raid_layout::raid1eCopy(di, 0, n);
        const auto l1 = raid_layout::raid1eCopy(di, 1, n);
        if (l0.disk >= n || l1.disk >= n) continue;
        const uint64_t o0 = offBytes + l0.row * sb;
        const uint64_t o1 = offBytes + l1.row * sb;
        if (o0 + sb > memberSize || o1 + sb > memberSize) continue;
        if (!readBlock(members[l0.disk], o0, static_cast<uint32_t>(sb), a)) continue;
        if (!readBlock(members[l1.disk], o1, static_cast<uint32_t>(sb), b)) continue;
        ++total;
        if (std::memcmp(a.data(), b.data(), sb) == 0) ++hits;
    }
    if (total == 0) return false;
    conf = static_cast<double>(hits) / static_cast<double>(total);
    return passesConfidence(conf, total, kRaid1Floor);
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

bool scoreAdjacentMirrorPairs(const std::vector<DiskReader*>& members, uint64_t memberSize,
                              double& confOut) {
    const size_t n = members.size();
    if (n < 4 || (n % 2) != 0 || memberSize < kSector) return false;
    const uint32_t chunk =
        static_cast<uint32_t>(std::min<uint64_t>(kRaid1Chunk, (memberSize / kSector) * kSector));
    const size_t regions = static_cast<size_t>(memberSize / chunk);
    const size_t nSamples = std::min(kMaxSamples, std::max(regions, size_t{1}));
    double minConf = 1.0;
    std::vector<uint8_t> a, b;
    for (size_t p = 0; p < n; p += 2) {
        size_t consistent = 0, total = 0;
        for (size_t k = 0; k < nSamples; ++k) {
            const uint64_t off = (nSamples > 1)
                ? static_cast<uint64_t>(chunk) * (regions - 1) * k / (nSamples - 1)
                : 0;
            if (!readBlock(members[p], off, chunk, a) || !readBlock(members[p + 1], off, chunk, b))
                continue;
            ++total;
            if (a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0) ++consistent;
        }
        if (total == 0) return false;
        const double conf = static_cast<double>(consistent) / static_cast<double>(total);
        if (!passesConfidence(conf, total, kRaid1Floor)) return false;
        minConf = std::min(minConf, conf);
    }
    confOut = minConf;
    return true;
}

} // namespace

bool detectRaidGeometryFixedOrder(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    const size_t n = members.size();
    if (n < 2) return false;

    uint64_t memberSize = UINT64_MAX;
    for (const DiskReader* m : members) memberSize = std::min(memberSize, m->getDiskSize());
    if (memberSize == UINT64_MAX || memberSize < kSector) return false;

    // Tie-break rule: highest confidence wins; then the LARGER stripe size
    // (smaller stripes score spuriously on repetitive data); then offset 0
    // (no reservation is the simpler layout). Levels are evaluated mirror ->
    // RAID5 -> RAID6. RAID10 (adjacent mirrors) is scored after RAID1 and
    // suppresses vacuous RAID5 XOR on A,A,B,B. A full tie keeps the earlier
    // (least-redundant) assumption — e.g. all-zero members detect as RAID1,
    // not RAID5. RAID0 is never emitted: it has no parity, nothing here can score it.
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
            // Chunk starts only — readSectors requires sector alignment. Linear
            // byte interpolation of (memberSize-chunk) missed the FAT16 4120-sector
            // mirror (only 2/32 samples landed on a 512B boundary).
            const uint64_t off = (nSamples > 1)
                ? static_cast<uint64_t>(chunk) * (regions - 1) * k / (nSamples - 1)
                : 0;
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

    // --- RAID10: adjacent mirror pairs. XOR of A,A,B,B is vacuously RAID5
    // consistent — skip parity when pairs match. Stripe only when FS magics
    // land (no parity to score size). RAID1 already won if every member matches.
    double raid10PairConf = 0;
    const bool raid10Pairs = !have && scoreAdjacentMirrorPairs(members, memberSize, raid10PairConf);
    if (raid10Pairs) {
        std::vector<RaidGeometry> raid10Pass;
        for (size_t sb : kStripeSizes) {
            for (uint64_t offSectors : kDataOffsetSectors) {
                const uint64_t offBytes = offSectors * kSector;
                if (offBytes + sb > memberSize) continue;
                RaidGeometry cand{RaidLevel::RAID10, sb, offSectors, raid10PairConf};
                if (fsSignatureScore(members, cand) > 0) raid10Pass.push_back(cand);
            }
        }
        if (!raid10Pass.empty()) {
            RaidGeometry best = raid10Pass[0];
            int bestFs = fsSignatureScore(members, best);
            for (const auto& c : raid10Pass) {
                const int fs = fsSignatureScore(members, c);
                if (fs > bestFs) {
                    best = c;
                    bestFs = fs;
                } else if (fs == bestFs) {
                    // ponytail: no XOR size vote; boot-only ties keep the smaller stripe.
                    if (c.blockSize < best.blockSize) best = c;
                    else if (c.blockSize == best.blockSize && c.dataOffsetSectors < best.dataOffsetSectors)
                        best = c;
                }
            }
            best.fsConfirmed = bestFs > 0;
            consider(best);
        }
    }

    // --- RAID1E: odd N, 2-copy near. Even N is RAID10 pair mapping.
    bool raid1eFs = false;
    if (!have && n >= 3 && (n % 2) == 1) {
        std::vector<RaidGeometry> raid1ePass;
        for (size_t sb : kStripeSizes) {
            for (uint64_t offSectors : kDataOffsetSectors) {
                const uint64_t offBytes = offSectors * kSector;
                double conf = 0;
                if (!scoreRaid1eCopies(members, memberSize, sb, offBytes, conf)) continue;
                RaidGeometry cand{RaidLevel::RAID1E, sb, offSectors, conf};
                if (fsSignatureScore(members, cand) > 0) raid1ePass.push_back(cand);
            }
        }
        if (!raid1ePass.empty()) {
            RaidGeometry best = raid1ePass[0];
            int bestFs = fsSignatureScore(members, best);
            for (const auto& c : raid1ePass) {
                const int fs = fsSignatureScore(members, c);
                if (fs > bestFs) {
                    best = c;
                    bestFs = fs;
                } else if (fs == bestFs) {
                    if (c.blockSize < best.blockSize) best = c;
                    else if (c.blockSize == best.blockSize && c.dataOffsetSectors < best.dataOffsetSectors)
                        best = c;
                }
            }
            best.fsConfirmed = bestFs > 0;
            consider(best);
            raid1eFs = best.fsConfirmed;
        }
    }

    // --- RAID5 / RAID6: parity-consistency per candidate geometry. --------
    // ponytail: RAID5 P-syndrome still scores some aligned wrong sizes; when
    // the assembled view has no FS magic, tie-break stays largest stripe.
    // When boot/MBR/NTFS/FAT/ext magics land, fsSignatureScore picks the
    // geometry that reconstructs them. RAID6 Q-syndrome already pins size.
    std::vector<RaidGeometry> raid5Pass;
    auto evalParity = [&](RaidLevel level, size_t stripeBytes) {
        for (uint64_t offSectors : kDataOffsetSectors) {
            const uint64_t offBytes = offSectors * kSector;
            if (offBytes + stripeBytes > memberSize) continue;
            const uint64_t nStripes = (memberSize - offBytes) / stripeBytes;
            const size_t nSamples = static_cast<size_t>(std::min<uint64_t>(kMaxSamples, nStripes));
            std::vector<std::vector<uint8_t>> blocks(n);
            size_t leftHits = 0, rightHits = 0, raid6Hits = 0, total = 0;
            for (size_t k = 0; k < nSamples; ++k) {
                const uint64_t si = k * nStripes / nSamples;
                const uint64_t off = offBytes + si * stripeBytes;
                bool ok = true;
                for (size_t m = 0; m < n && ok; ++m)
                    ok = readBlock(members[m], off, static_cast<uint32_t>(stripeBytes), blocks[m]);
                if (!ok) continue; // skip samples with any failed member read
                ++total;
                if (level == RaidLevel::RAID6) {
                    if (raid6Consistent(si, static_cast<uint32_t>(n), blocks)) ++raid6Hits;
                } else {
                    if (raid5Consistent(si, static_cast<uint32_t>(n), blocks,
                                        raid_layout::Raid5Algorithm::LeftAsymmetric))
                        ++leftHits;
                    if (raid5Consistent(si, static_cast<uint32_t>(n), blocks,
                                        raid_layout::Raid5Algorithm::RightAsymmetric))
                        ++rightHits;
                }
            }
            if (total == 0) continue;
            auto emit = [&](size_t hits, raid_layout::Raid5Algorithm algo) {
                const double conf = static_cast<double>(hits) / static_cast<double>(total);
                if (!passesConfidence(conf, total, kParityFloor)) return;
                RaidGeometry cand{level, stripeBytes, offSectors, conf};
                cand.raid5Algorithm = algo;
                if (level == RaidLevel::RAID5) raid5Pass.push_back(cand);
                consider(cand);
            };
            if (level == RaidLevel::RAID6) emit(raid6Hits, raid_layout::Raid5Algorithm::LeftAsymmetric);
            else {
                emit(leftHits, raid_layout::Raid5Algorithm::LeftAsymmetric);
                emit(rightHits, raid_layout::Raid5Algorithm::RightAsymmetric);
            }
        }
    };

    if (!raid10Pairs && !raid1eFs) {
        if (n >= 3)
            for (size_t sb : kStripeSizes) evalParity(RaidLevel::RAID5, sb);
        if (n >= 4)
            for (size_t sb : kStripeSizes) evalParity(RaidLevel::RAID6, sb);

        if (have && out.level == RaidLevel::RAID5 && !raid5Pass.empty()) {
            auto raid5Fs = [&](RaidGeometry& c) {
                static const raid_layout::Raid5Algorithm kAlgos[] = {
                    raid_layout::Raid5Algorithm::LeftAsymmetric,
                    raid_layout::Raid5Algorithm::LeftSymmetric,
                    raid_layout::Raid5Algorithm::RightAsymmetric,
                    raid_layout::Raid5Algorithm::RightSymmetric,
                };
                int best = fsSignatureScore(members, c);
                auto bestA = c.raid5Algorithm;
                for (auto a : kAlgos) {
                    if (a == bestA) continue;
                    RaidGeometry t = c;
                    t.raid5Algorithm = a;
                    const int fs = fsSignatureScore(members, t);
                    if (fs > best) {
                        best = fs;
                        bestA = a;
                    }
                }
                c.raid5Algorithm = bestA;
                return best;
            };
            int bestFs = raid5Fs(out);
            RaidGeometry best = out;
            for (auto c : raid5Pass) {
                const int fs = raid5Fs(c);
                if (fs > bestFs) {
                    best = c;
                    bestFs = fs;
                } else if (fs == bestFs && bestFs > 0) {
                    if (c.blockSize > best.blockSize) best = c;
                    else if (c.blockSize == best.blockSize && c.dataOffsetSectors < best.dataOffsetSectors)
                        best = c;
                }
            }
            out = best;
            out.fsConfirmed = bestFs > 0;
        }
    }

    return have;
}

bool tryMdadmGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out);
bool tryImsmGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out);
bool tryDdfGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out);

bool detectRaidGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    const size_t n = members.size();
    if (n < 2) return false;
    if (tryMdadmGeometry(members, out)) return true;
    if (tryImsmGeometry(members, out)) return true;
    if (tryDdfGeometry(members, out)) return true;

    uint64_t memberSize = UINT64_MAX;
    for (const DiskReader* m : members) {
        if (m) memberSize = std::min(memberSize, m->getDiskSize());
    }
    double pairConf = 0;
    const bool adjacentPairs = scoreAdjacentMirrorPairs(members, memberSize, pairConf);

    auto identity = [](size_t m) {
        std::vector<size_t> o(m);
        for (size_t i = 0; i < m; ++i) o[i] = i;
        return o;
    };

    RaidGeometry best;
    int bestFs = -1;
    bool any = false;
    auto considerPerm = [&](const std::vector<DiskReader*>& perm, std::vector<size_t> order) {
        RaidGeometry g;
        if (!detectRaidGeometryFixedOrder(perm, g)) return;
        // A,A,B,B XOR is RAID5 in other slot orders; keep RAID10-or-fail.
        if (adjacentPairs && g.level != RaidLevel::RAID10) return;
        const int fs = fsSignatureScore(perm, g);
        g.memberOrder = std::move(order);
        if (!any || fs > bestFs) {
            best = std::move(g);
            bestFs = fs;
            any = true;
        }
    };

    considerPerm(members, identity(n));
    // RAID1 reconstruct ignores slot order.
    // ponytail: n rotations + reverse, not n! — member cap 16.
    if (!(any && best.level == RaidLevel::RAID1)) {
        for (size_t r = 1; r < n; ++r) {
            std::vector<DiskReader*> perm(n);
            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i) {
                order[i] = (i + r) % n;
                perm[i] = members[order[i]];
            }
            considerPerm(perm, std::move(order));
        }
        std::vector<DiskReader*> rev(n);
        std::vector<size_t> revOrder(n);
        for (size_t i = 0; i < n; ++i) {
            revOrder[i] = n - 1 - i;
            rev[i] = members[revOrder[i]];
        }
        considerPerm(rev, std::move(revOrder));
    }
    if (!any) return false;
    out = std::move(best);
    return true;
}

bool detectRaidFromEvidencePaths(const std::vector<std::string>& paths, RaidGeometry& out) {
    out = RaidGeometry{};
    if (paths.size() < 2 || paths.size() > 16) return false;
    std::vector<std::shared_ptr<DiskReader>> holders;
    std::vector<DiskReader*> raw;
    holders.reserve(paths.size());
    raw.reserve(paths.size());
    for (const auto& p : paths) {
        if (!isEvidenceImagePath(p)) return false;
        auto r = std::make_shared<DiskReader>();
        if (!r->attachEvidenceImage(p)) return false;
        raw.push_back(r.get());
        holders.push_back(std::move(r));
    }
    return detectRaidGeometry(raw, out);
}

bool parseMdadmSuper1(const uint8_t* buf, size_t n, MdadmSuper1& out) {
    out = MdadmSuper1{};
    if (!buf || n < kSb1Size) return false;
    if (readLe32(buf) != kMdadmMagic) return false;
    if (readLe32(buf + 4) != 1) return false;
    const uint32_t maxDev = readLe32(buf + 220);
    if (maxDev == 0 || maxDev > 256) return false;
    const size_t need = kSb1Size + static_cast<size_t>(maxDev) * 2;
    if (n < need) return false;
    if (readLe32(buf + 216) != mdadmSb1Csum(buf, n, maxDev)) return false;

    const uint32_t raidDisks = readLe32(buf + 92);
    const uint32_t chunkSectors = readLe32(buf + 88);
    if (raidDisks < 2 || raidDisks > 16) return false;
    const int32_t level = static_cast<int32_t>(readLe32(buf + 72));
    const bool linear = level == -1;
    if (linear) {
        if (chunkSectors != 0 &&
            (chunkSectors > 2048 || (chunkSectors & (chunkSectors - 1)) != 0))
            return false;
    } else if (chunkSectors == 0 || chunkSectors > 2048 ||
               (chunkSectors & (chunkSectors - 1)) != 0) {
        return false;
    }

    out.level = level;
    out.kernelLayout = readLe32(buf + 76);
    out.chunkSectors = chunkSectors;
    out.raidDisks = raidDisks;
    out.dataOffsetSectors = readLe64(buf + 128);
    out.devNumber = readLe32(buf + 160);
    std::memcpy(out.setUuid, buf + 16, 16);
    if (out.devNumber < maxDev) out.role = readLe16(buf + kSb1Size + out.devNumber * 2);
    return true;
}

bool readMdadmSuper1(DiskReader& reader, MdadmSuper1& out) {
    out = MdadmSuper1{};
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    auto tryAt = [&](uint64_t byteOff) -> bool {
        if (byteOff + 4096 > disk) return false;
        if ((byteOff % ss) != 0) return false;
        std::vector<uint8_t> buf(4096);
        const ReadResult res = reader.readSectors(byteOff, 4096, buf.data());
        if (!res.success || res.bytesRead < kSb1Size) return false;
        return parseMdadmSuper1(buf.data(), static_cast<size_t>(res.bytesRead), out);
    };
    if (tryAt(4096)) return true; // super-1.2
    if (tryAt(0)) return true;    // super-1.1
    // super-1.0: 8 KiB from end, 4 KiB aligned (mdadm super1.c case 0).
    const uint64_t dsize = disk / 512;
    if (dsize > 16) {
        const uint64_t offSec = (dsize - 16) & ~uint64_t{7};
        if (tryAt(offSec * 512)) return true;
    }
    return false;
}

raid_layout::Raid5Algorithm raid5AlgoFromMdadmLayout(uint32_t kernelLayout) {
    // drivers/md/raid5.c ALGORITHM_* — not our Raid5Algorithm numbering.
    switch (kernelLayout) {
    case 0: return raid_layout::Raid5Algorithm::LeftAsymmetric;
    case 1: return raid_layout::Raid5Algorithm::RightAsymmetric;
    case 2: return raid_layout::Raid5Algorithm::LeftSymmetric;
    case 3: return raid_layout::Raid5Algorithm::RightSymmetric;
    default: return raid_layout::Raid5Algorithm::LeftAsymmetric;
    }
}

bool tryMdadmGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    if (members.size() < 2 || members.size() > 16) return false;
    std::vector<MdadmSuper1> sbs;
    sbs.reserve(members.size());
    for (DiskReader* m : members) {
        if (!m) return false;
        MdadmSuper1 sb;
        if (!readMdadmSuper1(*m, sb)) return false;
        sbs.push_back(sb);
    }
    const MdadmSuper1& a = sbs[0];
    if (a.raidDisks != members.size()) return false;
    for (const auto& s : sbs) {
        if (s.level != a.level || s.kernelLayout != a.kernelLayout ||
            s.chunkSectors != a.chunkSectors || s.raidDisks != a.raidDisks ||
            s.dataOffsetSectors != a.dataOffsetSectors ||
            std::memcmp(s.setUuid, a.setUuid, 16) != 0)
            return false;
    }
    RaidLevel level;
    switch (a.level) {
    case -1: level = RaidLevel::JBOD; break;
    case 0: level = RaidLevel::RAID0; break;
    case 1: level = RaidLevel::RAID1; break;
    case 5: level = RaidLevel::RAID5; break;
    case 6: level = RaidLevel::RAID6; break;
    case 10: level = RaidLevel::RAID10; break;
    default: return false;
    }
    if (level == RaidLevel::RAID5 && a.kernelLayout > 3) return false;
    if (level == RaidLevel::RAID5 && members.size() < 3) return false;
    if (level == RaidLevel::RAID6 && members.size() < 4) return false;
    if (level == RaidLevel::RAID10 && (members.size() < 4 || (members.size() % 2) != 0))
        return false;

    std::vector<size_t> order(members.size(), static_cast<size_t>(-1));
    std::vector<uint8_t> used(members.size(), 0);
    for (size_t i = 0; i < sbs.size(); ++i) {
        const uint16_t role = sbs[i].role;
        if (role >= members.size() || used[role]) return false;
        used[role] = 1;
        order[role] = i;
    }
    for (size_t slot : order)
        if (slot == static_cast<size_t>(-1)) return false;

    const uint64_t offBytes = a.dataOffsetSectors * kSector;
    for (DiskReader* m : members) {
        if (!m || m->getDiskSize() <= offBytes) return false;
    }
    const size_t stripe = static_cast<size_t>(a.chunkSectors) * static_cast<size_t>(kSector);
    if (level != RaidLevel::RAID1 && level != RaidLevel::JBOD) {
        const uint64_t memberSize = members[0]->getDiskSize();
        if (stripe == 0 || offBytes + stripe > memberSize) return false;
    }

    out.level = level;
    out.blockSize = (level == RaidLevel::RAID1 || level == RaidLevel::JBOD) ? 0 : stripe;
    out.dataOffsetSectors = a.dataOffsetSectors;
    out.confidence = 1.0;
    if (level == RaidLevel::RAID5) out.raid5Algorithm = raid5AlgoFromMdadmLayout(a.kernelLayout);
    out.memberOrder = std::move(order);
    return true;
}

bool parseImsmSuper(const uint8_t* buf, size_t n, ImsmSuper& out) {
    out = ImsmSuper{};
    if (!buf || n < 264) return false;
    if (std::memcmp(buf, "Intel Raid ISM Cfg Sig. ", 24) != 0) return false;
    const uint32_t mpbSize = readLe32(buf + 36);
    if (mpbSize < 264 || mpbSize > n || (mpbSize % 4) != 0 || mpbSize > 4096) return false;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < mpbSize; i += 4) {
        if (i == 32) continue;
        sum += readLe32(buf + i);
    }
    if (sum != readLe32(buf + 32)) return false;

    const uint8_t numDisks = buf[56];
    const uint8_t numDevs = buf[57];
    if (numDisks < 2 || numDisks > 16 || numDevs == 0) return false;
    const size_t disksEnd = 216 + static_cast<size_t>(numDisks) * 48;
    const size_t mapOff = disksEnd + 112;
    if (mpbSize < mapOff + 52) return false;
    if (buf[disksEnd + 88] != 0) return false; // migr_state: skip in-flight reshape
    const uint8_t nmem = buf[mapOff + 16];
    if (nmem < 2 || nmem > numDisks) return false;
    const size_t mapBytes = 52 + 4 * (static_cast<size_t>(nmem) - 1);
    if (mpbSize < mapOff + mapBytes) return false;

    const uint8_t level = buf[mapOff + 15];
    if (level != 0 && level != 1 && level != 5 && level != 10) return false;
    const uint16_t strip = readLe16(buf + mapOff + 12);
    if (strip == 0 || strip > 2048 || (strip & (strip - 1)) != 0) return false;

    out.familyNum = readLe32(buf + 40);
    out.numDisks = numDisks;
    out.numRaidDevs = numDevs;
    out.level = level;
    if (level == 1 && nmem != 2) out.level = 10; // mdadm get_imsm_raid_level
    out.chunkSectors = strip;
    out.dataOffsetSectors = readLe32(buf + mapOff) |
                            (static_cast<uint64_t>(readLe32(buf + mapOff + 20)) << 32);
    out.numMembers = nmem;
    out.diskOrd.resize(nmem);
    for (uint8_t s = 0; s < nmem; ++s)
        out.diskOrd[s] = readLe32(buf + mapOff + 48 + static_cast<size_t>(s) * 4) & 0x00FFFFFFu;
    return true;
}

bool readImsmSuper(DiskReader& reader, ImsmSuper& out) {
    out = ImsmSuper{};
    const uint64_t disk = reader.getDiskSize();
    if (disk < 2048) return false;
    std::vector<uint8_t> buf(4096, 0);
    const ReadResult res = reader.readSectors(disk - 1024, 512, buf.data());
    if (!res.success || res.bytesRead < 512) return false;
    if (std::memcmp(buf.data(), "Intel Raid ISM Cfg Sig. ", 24) != 0) return false;
    const uint32_t mpbSize = readLe32(buf.data() + 36);
    if (mpbSize > 4096 || mpbSize < 264) return false;
    if (mpbSize > 512) {
        const uint32_t totalSec = (mpbSize + 511) / 512;
        const uint32_t extra = totalSec - 1;
        const uint64_t extOff = disk - 512ull * (2 + extra);
        const uint32_t extraBytes = extra * 512;
        if (extOff + extraBytes > disk || extraBytes + 512 > buf.size()) return false;
        const ReadResult r2 = reader.readSectors(extOff, extraBytes, buf.data() + 512);
        if (!r2.success || r2.bytesRead < extraBytes) return false;
    }
    return parseImsmSuper(buf.data(), buf.size(), out);
}

bool tryImsmGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    if (members.size() < 2 || members.size() > 16) return false;
    std::vector<ImsmSuper> sbs;
    sbs.reserve(members.size());
    for (DiskReader* m : members) {
        if (!m) return false;
        ImsmSuper sb;
        if (!readImsmSuper(*m, sb)) return false;
        sbs.push_back(std::move(sb));
    }
    const ImsmSuper& a = sbs[0];
    if (a.numMembers != members.size()) return false;
    for (const auto& s : sbs) {
        if (s.familyNum != a.familyNum || s.level != a.level ||
            s.chunkSectors != a.chunkSectors || s.numMembers != a.numMembers ||
            s.dataOffsetSectors != a.dataOffsetSectors)
            return false;
    }
    RaidLevel level;
    switch (a.level) {
    case 0: level = RaidLevel::RAID0; break;
    case 1: level = RaidLevel::RAID1; break;
    case 5: level = RaidLevel::RAID5; break;
    case 10: level = RaidLevel::RAID10; break;
    default: return false;
    }
    if (level == RaidLevel::RAID5 && members.size() < 3) return false;
    if (level == RaidLevel::RAID10 && (members.size() < 4 || (members.size() % 2) != 0))
        return false;

    const uint64_t memberSize = members[0]->getDiskSize();
    const uint64_t offBytes = a.dataOffsetSectors * kSector;
    const size_t stripe = static_cast<size_t>(a.chunkSectors) * static_cast<size_t>(kSector);
    if (level != RaidLevel::RAID1) {
        if (stripe == 0 || offBytes + stripe > memberSize) return false;
    }

    out.level = level;
    out.blockSize = (level == RaidLevel::RAID1) ? 0 : stripe;
    out.dataOffsetSectors = a.dataOffsetSectors;
    out.confidence = 1.0;
    if (level == RaidLevel::RAID5)
        out.raid5Algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric;
    out.memberOrder.resize(members.size());
    for (size_t i = 0; i < members.size(); ++i) out.memberOrder[i] = i;
    return true;
}

bool parseDdfHeader(const uint8_t* buf, size_t n, DdfHeader& out) {
    out = DdfHeader{};
    if (!buf || n < 512) return false;
    if (readBe32(buf) != 0xDE11DE11u) return false;
    if (!ddfCrcOk(buf)) return false;
    out.primaryLba = readBe64(buf + 96);
    out.configSectionOffset = readBe32(buf + 188);
    out.configSectionLength = readBe32(buf + 192);
    out.type = buf[112];
    std::memcpy(out.guid, buf + 8, 24);
    return true;
}

bool parseDdfVd(const uint8_t* buf, size_t n, DdfSuper& out) {
    out = DdfSuper{};
    if (!buf || n < 512) return false;
    if (readBe32(buf) != 0xEEEEEEEEu) return false;
    if (!ddfCrcOk(buf)) return false;
    const uint16_t nmem = readBe16(buf + 64);
    const uint8_t chunkShift = buf[66];
    const uint8_t prl = buf[67];
    if (nmem < 2 || nmem > 16 || chunkShift > 12) return false;
    switch (prl) {
    case 0: out.level = 0; break;
    case 1: out.level = 1; break;
    case 5: out.level = 5; break;
    case 6: out.level = 6; break;
    case 0x11: out.level = 10; break;
    default: return false;
    }
    out.rlq = buf[68];
    out.chunkSectors = 1u << chunkShift;
    out.numMembers = nmem;
    std::memcpy(out.guid, buf + 8, 24);
    return true;
}

bool readDdfSuper(DiskReader& reader, DdfSuper& out) {
    out = DdfSuper{};
    const uint64_t disk = reader.getDiskSize();
    if (disk < 4 * kSector || (disk % kSector) != 0) return false;
    std::vector<uint8_t> buf(512, 0);
    const ReadResult anchorRes = reader.readSectors(disk - kSector, 512, buf.data());
    if (!anchorRes.success || anchorRes.bytesRead < 512) return false;
    DdfHeader anchor;
    if (!parseDdfHeader(buf.data(), 512, anchor)) return false;
    const uint64_t lastLba = disk / kSector - 1;
    DdfHeader primary = anchor;
    if (anchor.type != 1) {
        if (anchor.primaryLba > lastLba || anchor.primaryLba == lastLba) return false;
        const ReadResult pRes = reader.readSectors(anchor.primaryLba * kSector, 512, buf.data());
        if (!pRes.success || pRes.bytesRead < 512) return false;
        if (!parseDdfHeader(buf.data(), 512, primary)) return false;
    }
    if (primary.type != 1 && primary.type != 2) return false;
    if (primary.configSectionLength == 0 || primary.configSectionLength > 1024) return false;
    const uint64_t cfgLba = primary.primaryLba + primary.configSectionOffset;
    if (cfgLba > lastLba) return false;
    const ReadResult cRes = reader.readSectors(cfgLba * kSector, 512, buf.data());
    if (!cRes.success || cRes.bytesRead < 512) return false;
    return parseDdfVd(buf.data(), 512, out);
}

bool tryDdfGeometry(const std::vector<DiskReader*>& members, RaidGeometry& out) {
    out = RaidGeometry{};
    if (members.size() < 2 || members.size() > 16) return false;
    std::vector<DdfSuper> sbs;
    sbs.reserve(members.size());
    for (DiskReader* m : members) {
        if (!m) return false;
        DdfSuper sb;
        if (!readDdfSuper(*m, sb)) return false;
        sbs.push_back(sb);
    }
    const DdfSuper& a = sbs[0];
    if (a.numMembers != members.size()) return false;
    for (const auto& s : sbs) {
        if (s.level != a.level || s.rlq != a.rlq || s.chunkSectors != a.chunkSectors ||
            s.numMembers != a.numMembers || std::memcmp(s.guid, a.guid, 24) != 0)
            return false;
    }
    RaidLevel level;
    switch (a.level) {
    case 0: level = RaidLevel::RAID0; break;
    case 1: level = RaidLevel::RAID1; break;
    case 5: level = RaidLevel::RAID5; break;
    case 6: level = RaidLevel::RAID6; break;
    case 10: level = RaidLevel::RAID10; break;
    default: return false;
    }
    if (level == RaidLevel::RAID5 && members.size() < 3) return false;
    if (level == RaidLevel::RAID6 && members.size() < 4) return false;
    if (level == RaidLevel::RAID10 && (members.size() < 4 || (members.size() % 2) != 0))
        return false;

    raid_layout::Raid5Algorithm r5 = raid_layout::Raid5Algorithm::LeftAsymmetric;
    if (level == RaidLevel::RAID5) {
        uint32_t kernelLayout = 0xFFFFFFFFu;
        switch (a.rlq) {
        case 0: kernelLayout = 1; break; // DDF_RAID5_0_RESTART → RIGHT_ASYM
        case 2: kernelLayout = 0; break; // DDF_RAID5_N_RESTART → LEFT_ASYM
        case 3: kernelLayout = 2; break; // DDF_RAID5_N_CONTINUE → LEFT_SYM
        default: return false;
        }
        r5 = raid5AlgoFromMdadmLayout(kernelLayout);
    }

    const uint64_t memberSize = members[0]->getDiskSize();
    const size_t stripe = static_cast<size_t>(a.chunkSectors) * static_cast<size_t>(kSector);
    if (level != RaidLevel::RAID1) {
        if (stripe == 0 || stripe > memberSize) return false;
    }

    out.level = level;
    out.blockSize = (level == RaidLevel::RAID1) ? 0 : stripe;
    out.dataOffsetSectors = 0;
    out.confidence = 1.0;
    if (level == RaidLevel::RAID5) out.raid5Algorithm = r5;
    out.memberOrder.resize(members.size());
    for (size_t i = 0; i < members.size(); ++i) out.memberOrder[i] = i;
    return true;
}

} // namespace byteback
