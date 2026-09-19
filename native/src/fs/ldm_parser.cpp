#include "fs/ldm_parser.h"
#include "byteback_io.h"
#include "fs/virtual_raid.h"
#include "io/volume_mapper_win.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace byteback {
namespace {

uint16_t rdBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t rdBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t rdBe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

int ldmRel(const uint8_t* buf, int n, int base, int offset) {
    const int b = base + offset;
    if (offset < 0 || b >= n) return -1;
    if (b + static_cast<int>(buf[b]) >= n) return -1;
    return static_cast<int>(buf[b]) + offset + 1;
}

uint64_t ldmVnum(const uint8_t* p, const uint8_t* end) {
    if (!p || p >= end) return 0;
    const unsigned len = *p++;
    if (len == 0 || len > 8 || p + len > end) return 0;
    uint64_t v = 0;
    for (unsigned i = 0; i < len; ++i) v = (v << 8) | p[i];
    return v;
}

bool ldmVstr(const uint8_t* p, const uint8_t* end, std::string& out) {
    out.clear();
    if (!p || p >= end) return false;
    const unsigned len = *p++;
    if (len == 0 || p + len > end) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

bool openVmdb(DiskReader& reader, const LdmPrivhead& ph, uint32_t& vblkSize, uint32_t& skip,
              uint32_t& finish) {
    if (ph.configSize < 19 || ph.configStart == 0) return false;
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    const uint64_t vmdbOff = (ph.configStart + 17) * ss;
    if (vmdbOff + ss > disk) return false;
    std::vector<uint8_t> sec(ss, 0);
    if (!readComplete(reader.readSectors(vmdbOff, ss, sec.data()), ss)) return false;
    if (std::memcmp(sec.data(), "VMDB", 4) != 0) return false;
    if (rdBe16(sec.data() + 0x12) != 4 || rdBe16(sec.data() + 0x14) != 10) return false;
    vblkSize = rdBe32(sec.data() + 8);
    const uint32_t vblkOff = rdBe32(sec.data() + 0x0C);
    const uint32_t lastSeq = rdBe32(sec.data() + 4);
    if (vblkSize < 64 || vblkSize > 512 || (512 % vblkSize) != 0 || lastSeq == 0) return false;
    skip = vblkOff >> 9;
    finish = static_cast<uint32_t>((static_cast<uint64_t>(vblkSize) * lastSeq) >> 9);
    return finish > skip;
}

} // namespace

bool parseLdmPrivhead(const uint8_t* buf, size_t n, LdmPrivhead& out) {
    out = LdmPrivhead{};
    if (!buf || n < 0x13B) return false;
    if (std::memcmp(buf, "PRIVHEAD", 8) != 0) return false;
    out.verMajor = rdBe16(buf + 0x0C);
    out.verMinor = rdBe16(buf + 0x0E);
    if (out.verMajor != 2 || (out.verMinor != 11 && out.verMinor != 12)) return false;
    out.logicalDiskStart = rdBe64(buf + 0x11B);
    out.logicalDiskSize = rdBe64(buf + 0x123);
    out.configStart = rdBe64(buf + 0x12B);
    out.configSize = rdBe64(buf + 0x133);
    if (out.logicalDiskSize == 0) return false;
    if (out.logicalDiskStart + out.logicalDiskSize > out.configStart) return false;
    out.diskGuid.assign(reinterpret_cast<const char*>(buf + 0x30), 64);
    const auto z = out.diskGuid.find('\0');
    if (z != std::string::npos) out.diskGuid.resize(z);
    return true;
}

bool readLdmPrivhead(DiskReader& reader, LdmPrivhead& out) {
    out = LdmPrivhead{};
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    auto tryAt = [&](uint64_t lba) -> bool {
        const uint64_t off = lba * ss;
        if (off + ss > disk) return false;
        std::vector<uint8_t> sec(ss, 0);
        if (!readComplete(reader.readSectors(off, ss, sec.data()), ss)) return false;
        return parseLdmPrivhead(sec.data(), sec.size(), out);
    };
    if (tryAt(6)) return true;
    if (disk >= ss && tryAt((disk / ss) - 1)) return true;
    return false;
}

bool looksLikeSpacedb(const uint8_t* buf, size_t n) {
    return buf && n >= 8 && std::memcmp(buf, "SPACEDB ", 8) == 0;
}

bool parseLdmPrt3(const uint8_t* buf, size_t n, LdmPrt3& out) {
    out = LdmPrt3{};
    if (!buf || n < 0x3C) return false;
    if (std::memcmp(buf, "VBLK", 4) != 0) return false;
    if (buf[0x13] != 0x33) return false;
    const int nn = static_cast<int>(n);
    const int rObj = ldmRel(buf, nn, 0x18, 0);
    if (rObj < 0) return false;
    const int rName = ldmRel(buf, nn, 0x18, rObj);
    if (rName < 0) return false;
    const int rSize = ldmRel(buf, nn, 0x34, rName);
    if (rSize < 0) return false;
    const int rParent = ldmRel(buf, nn, 0x34, rSize);
    if (rParent < 0) return false;
    int rDisk = ldmRel(buf, nn, 0x34, rParent);
    if (rDisk < 0) return false;
    int len = rDisk;
    if (buf[0x12] & 0x08) {
        const int rIndex = ldmRel(buf, nn, 0x34, rDisk);
        if (rIndex < 0) return false;
        len = rIndex;
    }
    len += 28;
    if (len > static_cast<int>(rdBe32(buf + 0x14))) return false;
    const int startOff = 0x24 + rName;
    const int volOff = 0x2C + rName;
    if (startOff + 8 > nn || volOff + 8 > nn) return false;
    out.start = rdBe64(buf + startOff);
    out.volumeOffset = rdBe64(buf + volOff);
    out.size = ldmVnum(buf + 0x34 + rName, buf + n);
    out.parentId = ldmVnum(buf + 0x34 + rSize, buf + n);
    out.diskId = ldmVnum(buf + 0x34 + rParent, buf + n);
    return out.size > 0;
}

bool parseLdmDsk3(const uint8_t* buf, size_t n, LdmDsk3& out) {
    out = LdmDsk3{};
    if (!buf || n < 0x1C) return false;
    if (std::memcmp(buf, "VBLK", 4) != 0) return false;
    if (buf[0x13] != 0x34 && buf[0x13] != 0x44) return false;
    const int nn = static_cast<int>(n);
    const int rObj = ldmRel(buf, nn, 0x18, 0);
    if (rObj < 0) return false;
    const int rName = ldmRel(buf, nn, 0x18, rObj);
    if (rName < 0) return false;
    out.objId = ldmVnum(buf + 0x18, buf + n);
    if (out.objId == 0) return false;
    if (buf[0x13] == 0x34) return ldmVstr(buf + 0x18 + rName, buf + n, out.guid);
    const uint8_t* g = buf + 0x18 + rName;
    if (g + 16 > buf + n) return false;
    const uint32_t d1 = static_cast<uint32_t>(g[0]) | (static_cast<uint32_t>(g[1]) << 8) |
                        (static_cast<uint32_t>(g[2]) << 16) | (static_cast<uint32_t>(g[3]) << 24);
    const uint16_t d2 = static_cast<uint16_t>(g[4] | (static_cast<uint16_t>(g[5]) << 8));
    const uint16_t d3 = static_cast<uint16_t>(g[6] | (static_cast<uint16_t>(g[7]) << 8));
    char s[37];
    std::snprintf(s, sizeof(s), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", d1, d2, d3,
                  g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    out.guid.assign(s);
    return true;
}

bool readLdmPrt3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmPrt3>& out) {
    out.clear();
    uint32_t vblkSize = 0, skip = 0, finish = 0;
    if (!openVmdb(reader, ph, vblkSize, skip, finish)) return false;
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    std::vector<uint8_t> sec(ss, 0);
    const uint32_t per = 512 / vblkSize;
    const uint32_t maxSec = finish < static_cast<uint32_t>(ph.configSize) ? finish : static_cast<uint32_t>(ph.configSize);
    for (uint32_t s = skip; s < maxSec; ++s) {
        const uint64_t off = (ph.configStart + 17 + s) * ss;
        if (off + ss > disk) break;
        if (!readComplete(reader.readSectors(off, ss, sec.data()), ss)) break;
        for (uint32_t v = 0; v < per; ++v) {
            LdmPrt3 p;
            if (parseLdmPrt3(sec.data() + v * vblkSize, vblkSize, p)) out.push_back(p);
        }
    }
    return !out.empty();
}

bool readLdmDsk3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmDsk3>& out) {
    out.clear();
    uint32_t vblkSize = 0, skip = 0, finish = 0;
    if (!openVmdb(reader, ph, vblkSize, skip, finish)) return false;
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    std::vector<uint8_t> sec(ss, 0);
    const uint32_t per = 512 / vblkSize;
    const uint32_t maxSec = finish < static_cast<uint32_t>(ph.configSize) ? finish : static_cast<uint32_t>(ph.configSize);
    for (uint32_t s = skip; s < maxSec; ++s) {
        const uint64_t off = (ph.configStart + 17 + s) * ss;
        if (off + ss > disk) break;
        if (!readComplete(reader.readSectors(off, ss, sec.data()), ss)) break;
        for (uint32_t v = 0; v < per; ++v) {
            LdmDsk3 d;
            if (parseLdmDsk3(sec.data() + v * vblkSize, vblkSize, d)) out.push_back(d);
        }
    }
    return !out.empty();
}

bool parseLdmCmp3(const uint8_t* buf, size_t n, LdmCmp3& out) {
    out = LdmCmp3{};
    if (!buf || n < 0x3C) return false;
    if (std::memcmp(buf, "VBLK", 4) != 0) return false;
    if (buf[0x13] != 0x32) return false;
    const int nn = static_cast<int>(n);
    const int rObj = ldmRel(buf, nn, 0x18, 0);
    if (rObj < 0) return false;
    const int rName = ldmRel(buf, nn, 0x18, rObj);
    if (rName < 0) return false;
    const int rState = ldmRel(buf, nn, 0x18, rName);
    if (rState < 0) return false;
    const int rChild = ldmRel(buf, nn, 0x1D, rState);
    if (rChild < 0) return false;
    const int rParent = ldmRel(buf, nn, 0x2D, rChild);
    if (rParent < 0) return false;
    int rStripe = 0;
    int len = rParent;
    if (buf[0x12] & 0x10) {
        rStripe = ldmRel(buf, nn, 0x2E, rParent);
        if (rStripe < 0) return false;
        const int rCols = ldmRel(buf, nn, 0x2E, rStripe);
        if (rCols < 0) return false;
        len = rCols;
    }
    len += 22;
    if (len != static_cast<int>(rdBe32(buf + 0x14))) return false;
    const int typeOff = 0x18 + rState;
    if (typeOff < 0 || typeOff >= nn) return false;
    out.objId = ldmVnum(buf + 0x18, buf + n);
    out.type = buf[typeOff];
    out.children = ldmVnum(buf + 0x1D + rState, buf + n);
    out.parentId = ldmVnum(buf + 0x2D + rChild, buf + n);
    if (rStripe > 0) out.chunkSectors = ldmVnum(buf + 0x2E + rParent, buf + n);
    return out.objId != 0;
}

bool readLdmCmp3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmCmp3>& out) {
    out.clear();
    uint32_t vblkSize = 0, skip = 0, finish = 0;
    if (!openVmdb(reader, ph, vblkSize, skip, finish)) return false;
    const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t disk = reader.getDiskSize();
    std::vector<uint8_t> sec(ss, 0);
    const uint32_t per = 512 / vblkSize;
    const uint32_t maxSec = finish < static_cast<uint32_t>(ph.configSize) ? finish : static_cast<uint32_t>(ph.configSize);
    for (uint32_t s = skip; s < maxSec; ++s) {
        const uint64_t off = (ph.configStart + 17 + s) * ss;
        if (off + ss > disk) break;
        if (!readComplete(reader.readSectors(off, ss, sec.data()), ss)) break;
        for (uint32_t v = 0; v < per; ++v) {
            LdmCmp3 c;
            if (parseLdmCmp3(sec.data() + v * vblkSize, vblkSize, c)) out.push_back(c);
        }
    }
    return !out.empty();
}

std::shared_ptr<VirtualRaid> assembleLdmFromOpenDisks(const std::vector<DiskReader*>& disks) {
    if (disks.size() < 2 || disks.size() > 16) return nullptr;
    struct Member {
        DiskReader* d = nullptr;
        LdmPrivhead ph;
        std::map<uint64_t, std::string> idToGuid;
    };
    std::vector<Member> members;
    members.reserve(disks.size());
    for (DiskReader* d : disks) {
        if (!d || !d->isOpen()) return nullptr;
        LdmPrivhead ph;
        if (!readLdmPrivhead(*d, ph)) continue;
        Member m;
        m.d = d;
        m.ph = std::move(ph);
        std::vector<LdmDsk3> dsks;
        if (readLdmDsk3(*d, m.ph, dsks)) {
            for (const auto& x : dsks) {
                if (x.objId != 0 && !x.guid.empty()) m.idToGuid[x.objId] = x.guid;
            }
        }
        members.push_back(std::move(m));
    }
    auto guidForDiskId = [&](uint64_t diskId) -> std::string {
        if (diskId == 0) return {};
        for (const auto& m : members) {
            const auto it = m.idToGuid.find(diskId);
            if (it != m.idToGuid.end()) return it->second;
        }
        return {};
    };
    auto memberForGuid = [&](const std::string& guid) -> const Member* {
        if (guid.empty()) return nullptr;
        for (const auto& m : members) {
            if (m.ph.diskGuid.size() == guid.size() &&
                std::equal(m.ph.diskGuid.begin(), m.ph.diskGuid.end(), guid.begin(),
                           [](unsigned char a, unsigned char b) {
                               return std::tolower(a) == std::tolower(b);
                           }))
                return &m;
        }
        return nullptr;
    };
    struct Ext {
        DiskReader* disk = nullptr;
        LdmPrivhead ph;
        LdmPrt3 prt;
    };
    std::vector<Ext> all;
    for (const auto& m : members) {
        std::vector<LdmPrt3> prts;
        if (!readLdmPrt3(*m.d, m.ph, prts)) continue;
        for (const auto& p : prts) {
            const Member* bound = &m;
            if (const auto* hit = memberForGuid(guidForDiskId(p.diskId))) bound = hit;
            all.push_back({bound->d, bound->ph, p});
        }
    }
    if (all.size() < 2) return nullptr;

    std::map<uint64_t, std::vector<Ext>> groups;
    for (const auto& e : all) {
        const uint64_t key = e.prt.parentId != 0 ? e.prt.parentId : (0x100000000ull + e.prt.start);
        auto& v = groups[key];
        bool dup = false;
        for (const auto& x : v) {
            if (x.prt.volumeOffset == e.prt.volumeOffset) dup = true;
        }
        if (!dup) v.push_back(e);
    }

    auto contiguous = [](const std::vector<Ext>& v) {
        if (v.size() < 2) return false;
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i].prt.volumeOffset != v[i - 1].prt.volumeOffset + v[i - 1].prt.size) return false;
        }
        return true;
    };

    auto buildJbod = [](const std::vector<Ext>& v) -> std::shared_ptr<VirtualRaid> {
        std::vector<std::shared_ptr<DiskReader>> members;
        std::vector<uint64_t> offs, lens;
        members.reserve(v.size());
        for (const auto& e : v) {
            auto c = e.disk->clone();
            if (!c) return nullptr;
            uint32_t ss = e.disk->getSectorSize();
            if (ss == 0) ss = 512;
            members.push_back(std::shared_ptr<DiskReader>(std::move(c)));
            offs.push_back((e.ph.logicalDiskStart + e.prt.start) * ss);
            lens.push_back(e.prt.size * ss);
        }
        try {
            return std::make_shared<VirtualRaid>(RaidLevel::JBOD, std::move(members), 0, 0,
                                                 raid_layout::Raid5Algorithm::LeftAsymmetric,
                                                 std::move(offs), std::move(lens));
        } catch (...) {
            return nullptr;
        }
    };

    auto buildStriped = [](RaidLevel level, const std::vector<Ext>& v,
                           uint64_t stripeBytes) -> std::shared_ptr<VirtualRaid> {
        if (stripeBytes == 0) return nullptr;
        std::vector<std::shared_ptr<DiskReader>> members;
        std::vector<uint64_t> offs, lens;
        members.reserve(v.size());
        for (const auto& e : v) {
            auto c = e.disk->clone();
            if (!c) return nullptr;
            uint32_t ss = e.disk->getSectorSize();
            if (ss == 0) ss = 512;
            members.push_back(std::shared_ptr<DiskReader>(std::move(c)));
            offs.push_back((e.ph.logicalDiskStart + e.prt.start) * ss);
            lens.push_back(e.prt.size * ss);
        }
        try {
            return std::make_shared<VirtualRaid>(level, std::move(members),
                                                 static_cast<size_t>(stripeBytes), 0,
                                                 raid_layout::Raid5Algorithm::LeftAsymmetric,
                                                 std::move(offs), std::move(lens));
        } catch (...) {
            return nullptr;
        }
    };

    std::map<uint64_t, LdmCmp3> cmps;
    for (const auto& m : members) {
        std::vector<LdmCmp3> raw;
        if (!readLdmCmp3(*m.d, m.ph, raw)) continue;
        for (const auto& c : raw) {
            if (c.objId != 0 && cmps.find(c.objId) == cmps.end()) cmps[c.objId] = c;
        }
    }

    std::shared_ptr<VirtualRaid> fallback;
    for (const auto& kv : cmps) {
        const auto& c = kv.second;
        if (c.chunkSectors == 0) continue;
        auto it = groups.find(c.objId);
        if (it == groups.end()) continue;
        auto v = it->second;
        std::sort(v.begin(), v.end(), [](const Ext& a, const Ext& b) {
            return a.prt.volumeOffset < b.prt.volumeOffset;
        });
        RaidLevel level = RaidLevel::RAID0;
        if (c.type == 1 && v.size() >= 2) level = RaidLevel::RAID0;
        else if (c.type == 3 && v.size() >= 3) level = RaidLevel::RAID5;
        else continue;
        uint32_t ss = v[0].disk->getSectorSize();
        if (ss == 0) ss = 512;
        std::vector<DiskReader*> uniq;
        for (const auto& e : v) {
            if (std::find(uniq.begin(), uniq.end(), e.disk) == uniq.end()) uniq.push_back(e.disk);
        }
        auto raid = buildStriped(level, v, c.chunkSectors * static_cast<uint64_t>(ss));
        if (!raid) continue;
        if (uniq.size() >= 2) return raid;
        if (!fallback) fallback = std::move(raid);
    }

    for (auto& g : groups) {
        if (cmps.count(g.first)) {
            const uint8_t t = cmps[g.first].type;
            if ((t == 1 || t == 3) && cmps[g.first].chunkSectors != 0) continue;
        }
        auto& v = g.second;
        std::sort(v.begin(), v.end(), [](const Ext& a, const Ext& b) {
            return a.prt.volumeOffset < b.prt.volumeOffset;
        });
        if (!contiguous(v)) continue;
        std::vector<DiskReader*> uniq;
        for (const auto& e : v) {
            if (std::find(uniq.begin(), uniq.end(), e.disk) == uniq.end()) uniq.push_back(e.disk);
        }
        auto raid = buildJbod(v);
        if (!raid) continue;
        if (uniq.size() >= 2) return raid;
        if (!fallback) fallback = std::move(raid);
    }
    return fallback;
}

std::shared_ptr<VirtualRaid> assembleLdmFromEvidencePaths(const std::vector<std::string>& paths) {
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
    return assembleLdmFromOpenDisks(raw);
}

} // namespace byteback
