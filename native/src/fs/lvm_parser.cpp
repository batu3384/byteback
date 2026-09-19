#include "fs/lvm_parser.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "byteback_io.h"
#include "fs/partition_scanner.h"
#include "fs/virtual_raid.h"
#include "io/volume_mapper_win.h"

namespace byteback {
namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
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

bool parseQuotedAfter(const std::string& s, const char* key, std::string& out) {
    const size_t k = s.find(key);
    if (k == std::string::npos) return false;
    size_t p = k + std::strlen(key);
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || s[p] != '"') return false;
    const size_t q = s.find('"', p + 1);
    if (q == std::string::npos || q - p > 33) return false;
    out.assign(s, p + 1, q - p - 1);
    return !out.empty();
}

bool parseU64After(const std::string& s, const char* key, uint64_t& out) {
    const size_t k = s.find(key);
    if (k == std::string::npos) return false;
    size_t p = k + std::strlen(key);
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
    uint64_t v = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
        v = v * 10 + static_cast<uint64_t>(s[p] - '0');
        ++p;
    }
    out = v;
    return true;
}

std::string parseIdentBefore(const std::string& s, size_t brace) {
    size_t i = brace;
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) --i;
    const size_t e = i;
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_' ||
                     s[i - 1] == '.' || s[i - 1] == '+' || s[i - 1] == '-'))
        --i;
    if (e <= i || e - i >= 128) return {};
    return s.substr(i, e - i);
}

bool parsePvIds(const std::string& s, std::vector<Lvm2StripeCol>& pvs) {
    pvs.clear();
    const size_t phys = s.find("physical_volumes");
    if (phys == std::string::npos) return false;
    const size_t logi = s.find("logical_volumes");
    const size_t end = logi == std::string::npos ? s.size() : logi;
    size_t p = phys;
    while (p < end) {
        const size_t idp = s.find("id = \"", p);
        if (idp == std::string::npos || idp >= end) break;
        if (idp + 6 + 32 > s.size() || s[idp + 6 + 32] != '"') {
            p = idp + 6;
            continue;
        }
        size_t brace = s.rfind('{', idp);
        if (brace == std::string::npos || brace < phys) {
            p = idp + 6;
            continue;
        }
        Lvm2StripeCol col;
        col.pvName = parseIdentBefore(s, brace);
        if (col.pvName.empty()) {
            p = idp + 6;
            continue;
        }
        std::memcpy(col.pvUuid, s.data() + idp + 6, 32);
        pvs.push_back(std::move(col));
        p = idp + 38;
    }
    return !pvs.empty();
}

bool parseStripeCols(const std::string& s, const char* listKey, std::vector<Lvm2StripeCol>& cols) {
    cols.clear();
    const size_t a = s.find(listKey);
    if (a == std::string::npos) return false;
    const size_t b = s.find(']', a);
    if (b == std::string::npos) return false;
    size_t p = a;
    while (p < b) {
        const size_t q = s.find('"', p);
        if (q == std::string::npos || q >= b) break;
        const size_t r = s.find('"', q + 1);
        if (r == std::string::npos || r >= b) return false;
        Lvm2StripeCol col;
        col.pvName.assign(s, q + 1, r - q - 1);
        size_t n = r + 1;
        while (n < b && (s[n] == ',' || std::isspace(static_cast<unsigned char>(s[n])))) ++n;
        if (n >= b || !std::isdigit(static_cast<unsigned char>(s[n]))) return false;
        uint64_t pe = 0;
        while (n < b && std::isdigit(static_cast<unsigned char>(s[n]))) {
            pe = pe * 10 + static_cast<uint64_t>(s[n] - '0');
            ++n;
        }
        col.startPe = pe;
        cols.push_back(std::move(col));
        p = n;
    }
    return !cols.empty();
}

const Lvm2StripeCol* findPvByName(const std::vector<Lvm2StripeCol>& pvs, const std::string& name) {
    for (const auto& p : pvs)
        if (p.pvName == name) return &p;
    return nullptr;
}

} // namespace

bool parseLvm2PvLabel(const uint8_t* sector, size_t n, uint64_t expectedSector, Lvm2Pv& out) {
    out = Lvm2Pv{};
    if (!sector || n < 512) return false;
    if (std::memcmp(sector, "LABELONE", 8) != 0) return false;
    if (std::memcmp(sector + 24, "LVM2 001", 8) != 0) return false;
    if (readLe64(sector + 8) != expectedSector) return false;
    const uint32_t hdrOff = readLe32(sector + 20);
    if (hdrOff < 32 || hdrOff > 128 || hdrOff + 56 > 512) return false;
    if (lvmCrc(sector + 20, 492) != readLe32(sector + 16)) return false;

    const uint8_t* pv = sector + hdrOff;
    size_t off = 40; // uuid[32] + device_size[8]
    uint64_t peOff = 0, peSize = 0;
    bool sawData = false;
    while (hdrOff + off + 16 <= 512) {
        const uint64_t a = readLe64(pv + off);
        const uint64_t b = readLe64(pv + off + 8);
        off += 16;
        if (a == 0 && b == 0) break;
        if (!sawData) {
            peOff = a;
            peSize = b;
            sawData = true;
        }
    }
    uint64_t metaOff = 0, metaSize = 0;
    bool sawMeta = false;
    while (hdrOff + off + 16 <= 512) {
        const uint64_t a = readLe64(pv + off);
        const uint64_t b = readLe64(pv + off + 8);
        off += 16;
        if (a == 0 && b == 0) break;
        if (!sawMeta) {
            metaOff = a;
            metaSize = b;
            sawMeta = true;
        }
    }
    if (!sawData || !sawMeta || peOff == 0 || metaOff == 0 || metaSize == 0 || metaSize > 1024 * 1024)
        return false;
    (void)peSize;
    std::memcpy(out.uuid, sector + hdrOff, 32);
    out.labelSector = expectedSector;
    out.peStartBytes = peOff;
    out.metaOffsetBytes = metaOff;
    out.metaSizeBytes = metaSize;
    return true;
}

bool readLvm2Pv(DiskReader& reader, uint64_t pvOffsetBytes, Lvm2Pv& out) {
    out = Lvm2Pv{};
    uint8_t buf[2048];
    const ReadResult res = reader.readSectors(pvOffsetBytes, 2048, buf);
    if (!res.success || res.bytesRead < 2048) return false;
    for (uint64_t i = 0; i < 4; ++i) {
        if (parseLvm2PvLabel(buf + i * 512, 512, i, out)) return true;
    }
    return false;
}

bool parseLvm2LinearLvs(const char* text, size_t n, const Lvm2Pv& pv, std::vector<Lvm2LinearLv>& out) {
    out.clear();
    if (!text || n == 0 || pv.peStartBytes == 0) return false;
    const std::string s(text, n);
    if (s.find("contents = \"Text Format Volume Group\"") == std::string::npos) return false;
    uint64_t extentSec = 0, stripe = 0, startExt = 0, extCount = 0;
    if (!parseU64After(s, "extent_size =", extentSec) || extentSec == 0 || extentSec > 65536)
        return false;
    const bool isRaidSeg = s.find("type = \"raid\"") != std::string::npos;
    const bool isMirror = s.find("type = \"mirror\"") != std::string::npos;
    const bool isStriped = s.find("type = \"striped\"") != std::string::npos;
    std::string raidType;
    if (isRaidSeg) {
        if (!parseQuotedAfter(s, "raid_type =", raidType)) return false;
        if (raidType == "raid1") {
            if (!parseU64After(s, "raid_disks =", stripe) || stripe < 2 || stripe > 8) return false;
        } else if (raidType == "raid0") {
            if (!parseU64After(s, "raid_disks =", stripe) || stripe < 2 || stripe > 8) return false;
        } else if (raidType == "raid5" || raidType == "raid5_ls" || raidType == "raid5_la" ||
                   raidType == "raid5_ra" || raidType == "raid5_rs") {
            if (!parseU64After(s, "raid_disks =", stripe) || stripe < 3 || stripe > 8) return false;
        } else if (raidType == "raid6" || raidType == "raid6_zr" || raidType == "raid6_nr" ||
                   raidType == "raid6_nc") {
            if (!parseU64After(s, "raid_disks =", stripe) || stripe < 4 || stripe > 8) return false;
        } else if (raidType == "raid10" || raidType == "raid10_near") {
            if (!parseU64After(s, "raid_disks =", stripe) || stripe < 4 || stripe > 8 ||
                (stripe % 2) != 0)
                return false;
        } else {
            return false;
        }
    } else if (isMirror) {
        if (!parseU64After(s, "mirror_count =", stripe) || stripe < 2 || stripe > 8) return false;
    } else if (isStriped) {
        if (!parseU64After(s, "stripe_count =", stripe) || stripe == 0 || stripe > 8) return false;
    } else {
        return false;
    }
    const bool asMirror = isMirror || raidType == "raid1";
    const bool asRaid5 = raidType == "raid5" || raidType == "raid5_ls" || raidType == "raid5_la" ||
                         raidType == "raid5_ra" || raidType == "raid5_rs";
    const bool asRaid6 = raidType == "raid6" || raidType == "raid6_zr" || raidType == "raid6_nr" ||
                         raidType == "raid6_nc";
    const bool asRaid10 = raidType == "raid10" || raidType == "raid10_near";
    if (!parseU64After(s, "start_extent =", startExt)) return false;
    if (!parseU64After(s, "extent_count =", extCount) || extCount == 0) return false;

    std::string name = "lv";
    const size_t lvKey = s.find("logical_volumes");
    if (lvKey != std::string::npos) {
        size_t brace = s.find('{', lvKey);
        if (brace != std::string::npos) {
            name = parseIdentBefore(s, s.find('{', brace + 1) == std::string::npos
                                           ? brace
                                           : /* inner lv name */ brace);
            size_t inner = brace + 1;
            while (inner < s.size() && std::isspace(static_cast<unsigned char>(s[inner]))) ++inner;
            size_t e = inner;
            while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_' ||
                                    s[e] == '.' || s[e] == '+' || s[e] == '-'))
                ++e;
            if (e > inner && e - inner < 128) name.assign(s, inner, e - inner);
        }
    }

    Lvm2LinearLv lv;
    lv.name = std::move(name);
    lv.stripeCount = static_cast<uint32_t>(stripe);
    lv.mirror = asMirror;
    lv.raid5 = asRaid5;
    lv.raid6 = asRaid6;
    lv.raid10 = asRaid10;
    if (raidType == "raid5_la")
        lv.raid5Algorithm = raid_layout::Raid5Algorithm::LeftAsymmetric;
    else if (raidType == "raid5_ra")
        lv.raid5Algorithm = raid_layout::Raid5Algorithm::RightAsymmetric;
    else if (raidType == "raid5_rs")
        lv.raid5Algorithm = raid_layout::Raid5Algorithm::RightSymmetric;
    else if (asRaid5)
        lv.raid5Algorithm = raid_layout::Raid5Algorithm::LeftSymmetric;
    lv.extentSizeSectors = extentSec;
    lv.sizeBytes = extCount * extentSec * 512;
    if (lv.sizeBytes < 512) return false;

    if (!asMirror && stripe == 1) {
        lv.dataStartBytes = pv.peStartBytes + startExt * extentSec * 512;
        if (lv.dataStartBytes < 512) return false;
        out.push_back(std::move(lv));
        return true;
    }

    uint64_t stripeSec = 0;
    if (!asMirror) {
        if (!parseU64After(s, "stripe_size =", stripeSec) || stripeSec == 0 || stripeSec > 2048)
            return false;
    }
    std::vector<Lvm2StripeCol> pvs;
    std::vector<Lvm2StripeCol> cols;
    const char* listKey = (isMirror && !isRaidSeg) ? "mirrors = [" : "stripes = [";
    if (!parsePvIds(s, pvs) || !parseStripeCols(s, listKey, cols) || cols.size() != stripe) return false;
    for (auto& col : cols) {
        const Lvm2StripeCol* meta = findPvByName(pvs, col.pvName);
        if (!meta) return false;
        std::memcpy(col.pvUuid, meta->pvUuid, 32);
    }
    lv.stripeSizeSectors = stripeSec;
    lv.stripes = std::move(cols);
    out.push_back(std::move(lv));
    return true;
}

bool listLvm2LinearLvs(DiskReader& reader, uint64_t pvOffsetBytes, std::vector<Lvm2LinearLv>& out) {
    out.clear();
    Lvm2Pv pv;
    if (!readLvm2Pv(reader, pvOffsetBytes, pv)) return false;
    if (pv.metaSizeBytes < 32 || pv.metaSizeBytes > 1024 * 1024) return false;
    std::vector<uint8_t> meta(static_cast<size_t>(pv.metaSizeBytes), 0);
    const ReadResult res =
        reader.readBytes(pvOffsetBytes + pv.metaOffsetBytes, static_cast<uint32_t>(pv.metaSizeBytes),
                         meta.data());
    if (!res.success || res.bytesRead < 32) return false;
    const char* key = "contents = \"Text Format Volume Group\"";
    const size_t keyLen = std::strlen(key);
    const size_t n = static_cast<size_t>(res.bytesRead);
    size_t hit = static_cast<size_t>(-1);
    if (n >= keyLen) {
        for (size_t i = 0; i + keyLen <= n; ++i) {
            if (std::memcmp(meta.data() + i, key, keyLen) == 0) {
                hit = i;
                break;
            }
        }
    }
    if (hit == static_cast<size_t>(-1)) return false;
    return parseLvm2LinearLvs(reinterpret_cast<const char*>(meta.data() + hit), n - hit, pv, out);
}

std::shared_ptr<VirtualRaid> assembleLvmStripedLv(const std::vector<Lvm2PvExtent>& pvs, const Lvm2LinearLv& lv) {
    if (lv.stripeCount < 2 || lv.stripeCount > 8) return nullptr;
    if (lv.stripes.size() != lv.stripeCount || lv.extentSizeSectors == 0) return nullptr;
    if (lv.raid5 && lv.stripeCount < 3) return nullptr;
    if (lv.raid6 && lv.stripeCount < 4) return nullptr;
    if (lv.raid10 && (lv.stripeCount < 4 || (lv.stripeCount % 2) != 0)) return nullptr;
    if (!lv.mirror && lv.stripeSizeSectors == 0) return nullptr;
    const size_t stripeBytes = lv.mirror ? 0 : static_cast<size_t>(lv.stripeSizeSectors) * 512u;
    if (!lv.mirror && stripeBytes == 0) return nullptr;
    std::vector<std::shared_ptr<DiskReader>> members;
    std::vector<uint64_t> offs;
    members.reserve(lv.stripes.size());
    offs.reserve(lv.stripes.size());
    for (const auto& col : lv.stripes) {
        bool hit = false;
        for (const auto& src : pvs) {
            if (!src.disk) continue;
            Lvm2Pv p;
            if (!readLvm2Pv(*src.disk, src.offsetBytes, p)) continue;
            if (std::memcmp(p.uuid, col.pvUuid, 32) != 0) continue;
            auto cloned = src.disk->clone();
            if (!cloned) return nullptr;
            members.push_back(std::shared_ptr<DiskReader>(std::move(cloned)));
            offs.push_back(src.offsetBytes + p.peStartBytes + col.startPe * lv.extentSizeSectors * 512);
            hit = true;
            break;
        }
        if (!hit) return nullptr;
    }
    try {
        RaidLevel level = RaidLevel::RAID0;
        if (lv.raid6)
            level = RaidLevel::RAID6;
        else if (lv.raid10)
            level = RaidLevel::RAID10;
        else if (lv.raid5)
            level = RaidLevel::RAID5;
        else if (lv.mirror)
            level = RaidLevel::RAID1;
        return std::make_shared<VirtualRaid>(level, std::move(members), stripeBytes, 0,
                                             lv.raid5Algorithm, std::move(offs));
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<VirtualRaid> assembleLvmStripedLv(DiskReader& disk, const std::vector<uint64_t>& pvOffsets,
                                                 const Lvm2LinearLv& lv) {
    std::vector<Lvm2PvExtent> srcs;
    srcs.reserve(pvOffsets.size());
    for (uint64_t off : pvOffsets) srcs.push_back({&disk, off});
    return assembleLvmStripedLv(srcs, lv);
}

void collectLvm2PvExtents(DiskReader& disk, std::vector<Lvm2PvExtent>& out) {
    Lvm2Pv p;
    if (readLvm2Pv(disk, 0, p)) {
        out.push_back({&disk, 0});
        return;
    }
    PartitionScanner scan(&disk);
    const auto gpt = scan.parseGPT();
    uint32_t sec = disk.getSectorSize();
    if (sec == 0) sec = 512;
    for (const auto& part : gpt) {
        if (part.type != "Linux LVM") continue;
        const uint64_t off = part.startSector * static_cast<uint64_t>(sec);
        if (readLvm2Pv(disk, off, p)) out.push_back({&disk, off});
    }
}

std::shared_ptr<VirtualRaid> assembleLvmFromOpenDisks(const std::vector<DiskReader*>& disks) {
    if (disks.size() < 2 || disks.size() > 16) return nullptr;
    std::vector<Lvm2PvExtent> extents;
    for (DiskReader* d : disks) {
        if (!d) return nullptr;
        collectLvm2PvExtents(*d, extents);
    }
    if (extents.size() < 2) return nullptr;
    std::vector<Lvm2LinearLv> lvs;
    bool got = false;
    for (const auto& ext : extents) {
        if (!ext.disk) continue;
        if (listLvm2LinearLvs(*ext.disk, ext.offsetBytes, lvs)) {
            got = true;
            break;
        }
    }
    if (!got) return nullptr;
    for (const auto& lv : lvs) {
        if (!lv.mirror && lv.stripeCount < 2) continue;
        auto raid = assembleLvmStripedLv(extents, lv);
        if (raid) return raid;
    }
    return nullptr;
}

std::shared_ptr<VirtualRaid> assembleLvmFromEvidencePaths(const std::vector<std::string>& paths) {
    if (paths.size() < 2 || paths.size() > 16) return nullptr;
    std::vector<std::shared_ptr<DiskReader>> owned;
    std::vector<DiskReader*> raw;
    owned.reserve(paths.size());
    raw.reserve(paths.size());
    for (const auto& p : paths) {
        if (p.size() > 4096 || !isEvidenceImagePath(p)) return nullptr;
        auto r = std::make_shared<DiskReader>();
        std::string err;
        if (!r->attachEvidenceImage(p, &err)) return nullptr;
        raw.push_back(r.get());
        owned.push_back(std::move(r));
    }
    return assembleLvmFromOpenDisks(raw);
}

} // namespace byteback
