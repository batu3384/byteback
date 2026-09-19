#include "fs/udf_parser.h"
#include "fs/ntfs_util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace byteback {
namespace {

constexpr uint32_t kMediaSec = 2048;
constexpr int kMaxDepth = 32;
constexpr size_t kMaxFiles = 65536;
constexpr uint32_t kMaxDirBytes = 4u * 1024u * 1024u;

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

std::string udfDstring(const uint8_t* p, size_t n) {
    if (!p || n < 2) return {};
    const uint8_t used = p[n - 1];
    if (used < 1 || static_cast<size_t>(used) >= n) return {};
    if (p[0] == 8) {
        size_t len = static_cast<size_t>(used) - 1;
        while (len > 0 && (p[len] == 0 || p[len] == ' ')) --len;
        if (len == 0) return {};
        return std::string(reinterpret_cast<const char*>(p + 1), len);
    }
    if (p[0] == 16 && ((used - 1) & 1u) == 0) {
        const size_t chars = (static_cast<size_t>(used) - 1) / 2;
        std::vector<uint16_t> u(chars);
        for (size_t i = 0; i < chars; ++i)
            u[i] = static_cast<uint16_t>((p[1 + 2 * i] << 8) | p[2 + 2 * i]);
        size_t nch = chars;
        while (nch > 0 && (u[nch - 1] == 0 || u[nch - 1] == static_cast<uint16_t>(' '))) --nch;
        if (nch == 0) return {};
        return ntfs::utf16leToUtf8(u.data(), nch);
    }
    return {};
}

bool validTag(const uint8_t* p, size_t n, uint16_t ident, uint32_t loc) {
    if (!p || n < 16) return false;
    if (rd16(p) != ident) return false;
    if (rd32(p + 12) != loc) return false;
    uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) {
        if (i != 4) sum = static_cast<uint8_t>(sum + p[i]);
    }
    if (p[4] != sum) return false;
    const uint16_t crcLen = rd16(p + 10);
    if (crcLen == 0 || static_cast<size_t>(16) + crcLen > n) return false;
    return rd16(p + 8) == udfCrc16(p + 16, crcLen);
}

bool readAt(DiskReader& r, uint64_t off, uint32_t n, uint8_t* buf) {
    return buf && n > 0 && readComplete(r.readBytes(off, n, buf), n);
}

struct LongAd {
    uint32_t len = 0;
    uint32_t lbn = 0;
    uint16_t part = 0;
};
LongAd parseLongAd(const uint8_t* p) {
    LongAd a;
    a.len = rd32(p) & 0x3FFFFFFFu;
    a.lbn = rd32(p + 4);
    a.part = rd16(p + 8);
    return a;
}

struct Part {
    uint32_t start = 0;
    uint32_t length = 0;
};

struct Walk {
    DiskReader* reader = nullptr;
    FileSystemParser::FileRecordCallback* cb = nullptr;
    std::atomic<bool>* running = nullptr;
    uint64_t partOff = 0;
    uint32_t blockSize = 2048;
    std::map<uint16_t, Part> parts;
    std::vector<uint32_t> vat;
    size_t emitted = 0;
    std::set<uint64_t> seen;
    uint16_t metaPart = 0xFFFF;
    std::vector<uint8_t> metaBits;
};

uint32_t mapLbn(const Walk& w, uint32_t lbn) {
    if (lbn < w.vat.size() && w.vat[lbn] != 0xFFFFFFFFu) return w.vat[lbn];
    return lbn;
}

uint64_t lbnBytes(const Walk& w, uint16_t part, uint32_t lbn) {
    auto it = w.parts.find(part);
    const uint32_t start = (it == w.parts.end()) ? 0 : it->second.start;
    return w.partOff + (static_cast<uint64_t>(start) + mapLbn(w, lbn)) * w.blockSize;
}

std::string ostaName(const uint8_t* id, uint8_t idLen) {
    if (!id || idLen < 2) return {};
    if (id[0] == 8) return std::string(reinterpret_cast<const char*>(id + 1), idLen - 1);
    if (id[0] == 16 && ((idLen - 1) % 2u) == 0) {
        const size_t n = (idLen - 1) / 2;
        std::vector<uint16_t> u(n);
        for (size_t i = 0; i < n; ++i) u[i] = static_cast<uint16_t>((id[1 + 2 * i] << 8) | id[2 + 2 * i]);
        return ntfs::utf16leToUtf8(u.data(), u.size());
    }
    return {};
}

bool readFe(Walk& w, const LongAd& icb, std::vector<uint8_t>& fe) {
    if (icb.len == 0 || w.blockSize == 0) return false;
    const uint32_t n = std::max(w.blockSize, icb.len);
    if (n > 64u * 1024u) return false;
    fe.assign(n, 0);
    if (!readAt(*w.reader, lbnBytes(w, icb.part, icb.lbn), n, fe.data()) || fe.size() < 16)
        return false;
    const uint16_t ident = rd16(fe.data());
    if (ident != 261 && ident != 266) return false;
    return validTag(fe.data(), fe.size(), ident, icb.lbn);
}

struct FeView {
    uint8_t fileType = 0;
    uint16_t flags = 0;
    uint64_t info = 0;
    uint32_t lenEa = 0;
    uint32_t lenAd = 0;
    const uint8_t* body = nullptr;
    uint32_t bodyMax = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
};

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t udfTimestampUnix(const uint8_t* p) {
    if (!p) return 0;
    const int year = static_cast<int16_t>(rd16(p + 2));
    const unsigned m = p[4];
    const unsigned d = p[5];
    if (year < 1970 || year > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    const int hh = p[6];
    const int mm = p[7];
    const int ss = p[8];
    if (hh > 23 || mm > 59 || ss > 59) return 0;
    int64_t t = daysFromCivil(year, m, d) * 86400 +
                static_cast<int64_t>(hh) * 3600 + static_cast<int64_t>(mm) * 60 + ss;
    const int tz = static_cast<int>(rd16(p) & 0x0FFFu);
    if (tz != 0x800) {
        const int signedTz = (tz & 0x0800) ? tz - 0x1000 : tz;
        t -= static_cast<int64_t>(signedTz) * 60;
    }
    return t;
}

bool viewFe(const std::vector<uint8_t>& fe, FeView& v) {
    if (fe.size() < 176) return false;
    const uint16_t ident = rd16(fe.data());
    const uint32_t hdr = (ident == 266) ? 216u : 176u;
    const uint32_t eaOff = (ident == 266) ? 208u : 168u;
    const uint32_t adOff = (ident == 266) ? 212u : 172u;
    if (fe.size() < hdr) return false;
    v.fileType = fe[16 + 11];
    v.flags = rd16(fe.data() + 16 + 18);
    v.info = rd64(fe.data() + 56);
    v.lenEa = rd32(fe.data() + eaOff);
    v.lenAd = rd32(fe.data() + adOff);
    if (hdr + v.lenEa > fe.size()) return false;
    v.body = fe.data() + hdr + v.lenEa;
    v.bodyMax = static_cast<uint32_t>(fe.size() - hdr - v.lenEa);
    const uint32_t tsOff = (ident == 266) ? 92u : 84u;
    if (fe.size() >= tsOff + 12) v.mtime = udfTimestampUnix(fe.data() + tsOff);
    if (ident == 266 && fe.size() >= 116) v.ctime = udfTimestampUnix(fe.data() + 104);
    return true;
}

void loadVat(Walk& w) {
    uint32_t last = 0;
    for (const auto& kv : w.parts) {
        if (kv.second.length == 0) continue;
        last = std::max(last, kv.second.start + kv.second.length - 1);
    }
    if (last == 0 || w.blockSize == 0) return;
    std::vector<uint8_t> fe(w.blockSize, 0);
    const uint64_t off = w.partOff + static_cast<uint64_t>(last) * w.blockSize;
    if (!readAt(*w.reader, off, w.blockSize, fe.data())) return;
    const uint16_t ident = rd16(fe.data());
    if (ident != 261 && ident != 266) return;
    if (!validTag(fe.data(), fe.size(), ident, last)) return;
    FeView v;
    if (!viewFe(fe, v) || v.fileType != 248) return;
    if ((v.flags & 7u) != 3u || v.lenAd < 4 || v.lenAd > v.bodyMax) return;
    if (v.lenAd > 4u * 1024u * 1024u) return;
    const uint32_t n = v.lenAd / 4u;
    w.vat.resize(n);
    for (uint32_t i = 0; i < n; ++i) w.vat[i] = rd32(v.body + i * 4);
}

void parsePartitionMaps(Walk& w, const uint8_t* lvd, size_t n) {
    if (!lvd || n < 442) return;
    const uint32_t mapLen = rd32(lvd + 264);
    const uint32_t nMaps = rd32(lvd + 268);
    if (mapLen < 6 || nMaps == 0 || nMaps > 16 || 440 + mapLen > n) return;
    const uint8_t* maps = lvd + 440;
    size_t o = 0;
    for (uint32_t ref = 0; ref < nMaps && o + 2 <= mapLen; ++ref) {
        const uint8_t type = maps[o];
        const uint8_t len = maps[o + 1];
        if (len < 2 || o + len > mapLen) break;
        if (type == 1 && len >= 6) {
            const uint16_t pdNum = rd16(maps + o + 4);
            auto it = w.parts.find(pdNum);
            if (it != w.parts.end()) w.parts[static_cast<uint16_t>(ref)] = it->second;
        } else if (type == 2 && len >= 64 &&
                   std::memcmp(maps + o + 5, "*UDF Metadata Partition", 23) == 0) {
            const uint16_t pdNum = rd16(maps + o + 38);
            const uint32_t primary = rd32(maps + o + 40);
            const uint32_t mirror = rd32(maps + o + 44);
            auto loadMeta = [&](uint32_t feLbn) -> bool {
                LongAd icb;
                icb.len = w.blockSize;
                icb.lbn = feLbn;
                icb.part = pdNum;
                std::vector<uint8_t> fe;
                FeView v;
                if (!readFe(w, icb, fe) || !viewFe(fe, v)) return false;
                if (v.fileType != 250 && v.fileType != 251) return false;
                if ((v.flags & 7u) != 0u || v.lenAd < 8 || v.lenAd > v.bodyMax || w.blockSize == 0)
                    return false;
                const uint32_t extLen = rd32(v.body) & 0x3FFFFFFFu;
                const uint32_t extPos = rd32(v.body + 4);
                auto it = w.parts.find(pdNum);
                const uint32_t pstart = (it == w.parts.end()) ? 0 : it->second.start;
                if (extLen < w.blockSize) return false;
                Part mp;
                mp.start = pstart + extPos;
                mp.length = extLen / w.blockSize;
                w.parts[static_cast<uint16_t>(ref)] = mp;
                return true;
            };
            bool loaded = loadMeta(primary);
            if (!loaded && mirror != 0 && mirror != primary) loaded = loadMeta(mirror);
            if (!loaded) {
                o += len;
                continue;
            }
            w.metaPart = static_cast<uint16_t>(ref);
            const uint32_t bitmap = rd32(maps + o + 48);
            if (bitmap == 0) {
                o += len;
                continue;
            }
            LongAd bicb;
            bicb.len = w.blockSize;
            bicb.lbn = bitmap;
            bicb.part = pdNum;
            std::vector<uint8_t> bfe;
            FeView bv;
            if (!readFe(w, bicb, bfe) || !viewFe(bfe, bv) || bv.fileType != 252) {
                o += len;
                continue;
            }
            if ((bv.flags & 7u) == 3u && bv.lenAd > 0 && bv.lenAd <= bv.bodyMax &&
                bv.lenAd <= 64u * 1024u) {
                w.metaBits.assign(bv.body, bv.body + bv.lenAd);
            } else if ((bv.flags & 7u) == 0u && bv.lenAd >= 8 && bv.lenAd <= bv.bodyMax) {
                const uint32_t extLen = rd32(bv.body) & 0x3FFFFFFFu;
                const uint32_t extPos = rd32(bv.body + 4);
                if (extLen > 0 && extLen <= 64u * 1024u) {
                    w.metaBits.assign(extLen, 0);
                    if (!readAt(*w.reader, lbnBytes(w, pdNum, extPos), extLen, w.metaBits.data()))
                        w.metaBits.clear();
                }
            }
        }
        o += len;
    }
}

void fillUdfMeta(FileRecord& fr, const std::string& parent, const std::string& name, uint64_t size,
                 int64_t mtime = 0, int64_t ctime = 0) {
    fr.name = name;
    fr.path = parent.empty() ? ("/" + name) : (parent + "/" + name);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size()) fr.extension = name.substr(dot + 1);
    fr.sizeBytes = size;
    fr.status = 1;
    fr.confidence = 90;
    fr.category = "Unknown";
    fr.source = "udf";
    fr.modifiedAt = mtime;
    fr.createdAt = ctime;
}

void emitFile(Walk& w, const std::string& parent, const std::string& name, uint64_t size,
              const uint8_t* resident, uint32_t residentLen, int64_t mtime = 0, int64_t ctime = 0) {
    FileRecord fr;
    fillUdfMeta(fr, parent, name, size, mtime, ctime);
    if (resident && residentLen) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(size, residentLen));
        fr.residentData.assign(resident, resident + n);
    }
    w.cb->operator()(fr);
    ++w.emitted;
}

void emitAdFile(Walk& w, uint16_t defaultPart, const std::string& parent, const std::string& name,
                uint64_t size, const uint8_t* ads, uint32_t lenAd, bool longAd, int64_t mtime = 0,
                int64_t ctime = 0) {
    const uint32_t ss = w.reader->getSectorSize() ? w.reader->getSectorSize() : 512;
    const uint32_t stride = longAd ? 16u : 8u;
    FileRecord fr;
    fillUdfMeta(fr, parent, name, size, mtime, ctime);
    uint64_t firstByteIn = 0;
    bool first = true;
    for (uint32_t i = 0; i + stride <= lenAd && fr.runs.size() < 256; i += stride) {
        const uint32_t raw = rd32(ads + i);
        const uint32_t typ = raw >> 30;
        const uint32_t extLen = raw & 0x3FFFFFFFu;
        const uint32_t extPos = rd32(ads + i + 4);
        const uint16_t part = longAd ? rd16(ads + i + 8) : defaultPart;
        if (typ != 0 || extLen == 0) continue;
        const uint64_t byteOff = lbnBytes(w, part, extPos);
        const uint64_t startSec = byteOff / ss;
        const uint64_t byteIn = byteOff % ss;
        const uint64_t need = static_cast<uint64_t>(extLen) + byteIn;
        const uint64_t nsec = (need + ss - 1) / ss;
        if (nsec == 0) continue;
        if (first) {
            firstByteIn = byteIn;
            first = false;
        }
        fr.runs.push_back({startSec, nsec});
    }
    if (fr.runs.empty()) return;
    fr.startSector = fr.runs.front().startSector;
    fr.startByteOffset = firstByteIn;
    uint64_t end = 0;
    for (const auto& r : fr.runs) end = std::max(end, r.startSector + r.sectorCount);
    fr.endSector = end ? end - 1 : fr.startSector;
    w.cb->operator()(fr);
    ++w.emitted;
}

bool walkIcb(Walk& w, const LongAd& icb, const std::string& parent, int depth);

bool parseFids(Walk& w, const uint8_t* data, uint32_t n, uint32_t loc, const std::string& parent,
               int depth) {
    size_t i = 0;
    while (i + 38 <= n) {
        if (w.running && !(*w.running)) return false;
        if (w.emitted >= kMaxFiles) return true;
        if (data[i] == 0) break;
        const uint8_t lfi = data[i + 19];
        const uint16_t liu = rd16(data + i + 36);
        size_t raw = 38u + lfi + liu;
        size_t padded = (raw + 3u) & ~3u;
        if (padded < 40 || i + padded > n) break;
        if (!validTag(data + i, padded, 257, loc)) {
            i += padded;
            continue;
        }
        const uint8_t chars = data[i + 18];
        if (chars & 0x04u || chars & 0x08u) {
            i += padded;
            continue;
        }
        const std::string name = ostaName(data + i + 38, lfi);
        if (name.empty()) {
            i += padded;
            continue;
        }
        const LongAd kid = parseLongAd(data + i + 20);
        if (chars & 0x02u) {
            const std::string next = parent.empty() ? ("/" + name) : (parent + "/" + name);
            if (!walkIcb(w, kid, next, depth + 1)) return false;
        } else {
            std::vector<uint8_t> fe;
            FeView v;
            if (readFe(w, kid, fe) && viewFe(fe, v)) {
                w.seen.insert((static_cast<uint64_t>(kid.part) << 32) | kid.lbn);
                if ((v.flags & 7u) == 3u && v.lenAd > 0 && v.lenAd <= v.bodyMax) {
                    emitFile(w, parent, name, v.info, v.body, v.lenAd, v.mtime, v.ctime);
                } else if ((v.flags & 7u) == 0u && v.lenAd >= 8 && v.lenAd <= v.bodyMax) {
                    emitAdFile(w, kid.part, parent, name, v.info, v.body, v.lenAd, false, v.mtime,
                               v.ctime);
                } else if ((v.flags & 7u) == 1u && v.lenAd >= 16 && v.lenAd <= v.bodyMax) {
                    emitAdFile(w, kid.part, parent, name, v.info, v.body, v.lenAd, true, v.mtime,
                               v.ctime);
                }
            }
        }
        i += padded;
    }
    return true;
}

bool walkIcb(Walk& w, const LongAd& icb, const std::string& parent, int depth) {
    if (depth > kMaxDepth) return true;
    if (w.running && !(*w.running)) return false;
    const uint64_t key = (static_cast<uint64_t>(icb.part) << 32) | icb.lbn;
    if (!w.seen.insert(key).second) return true;
    std::vector<uint8_t> fe;
    if (!readFe(w, icb, fe)) return true;
    FeView v;
    if (!viewFe(fe, v)) return true;
    if (v.fileType == 4) {
        if ((v.flags & 7u) == 3u) {
            const uint32_t n = std::min(v.lenAd, v.bodyMax);
            if (n > kMaxDirBytes) return true;
            return parseFids(w, v.body, n, icb.lbn, parent, depth);
        }
        if ((v.flags & 7u) == 0u && v.lenAd >= 8 && 8 <= v.bodyMax) {
            const uint32_t extLen = rd32(v.body) & 0x3FFFFFFFu;
            const uint32_t extPos = rd32(v.body + 4);
            if (extLen == 0 || extLen > kMaxDirBytes) return true;
            std::vector<uint8_t> dir(extLen, 0);
            if (!readAt(*w.reader, lbnBytes(w, icb.part, extPos), extLen, dir.data())) return true;
            return parseFids(w, dir.data(), extLen, extPos, parent, depth);
        }
        return true;
    }
    if (v.fileType == 5 && parent.empty()) {
        (void)v.info;
    }
    return true;
}

void scanMetaOrphans(Walk& w) {
    if (w.metaPart == 0xFFFF || w.metaBits.empty() || !w.cb || w.blockSize == 0) return;
    auto it = w.parts.find(w.metaPart);
    if (it == w.parts.end() || it->second.length == 0) return;
    const uint32_t nLbn = it->second.length;
    const uint32_t maxBits = static_cast<uint32_t>(w.metaBits.size() * 8u);
    const uint32_t last = std::min(nLbn, std::min(maxBits, 1u << 20));
    for (uint32_t lbn = 0; lbn < last; ++lbn) {
        if (w.running && !(*w.running)) return;
        if (w.emitted >= kMaxFiles) return;
        if ((w.metaBits[lbn / 8] & (1u << (lbn % 8))) == 0) continue;
        const uint64_t key = (static_cast<uint64_t>(w.metaPart) << 32) | lbn;
        if (w.seen.count(key)) continue;
        LongAd icb;
        icb.len = w.blockSize;
        icb.lbn = lbn;
        icb.part = w.metaPart;
        std::vector<uint8_t> fe;
        FeView v;
        if (!readFe(w, icb, fe) || !viewFe(fe, v) || v.fileType != 5) continue;
        w.seen.insert(key);
        const std::string name = "UDF_LBN" + std::to_string(lbn);
        const std::string parent = "/.udf_unalloc";
        if ((v.flags & 7u) == 3u && v.lenAd > 0 && v.lenAd <= v.bodyMax) {
            FileRecord fr;
            fillUdfMeta(fr, parent, name, v.info, v.mtime, v.ctime);
            fr.source = "udf_meta_orphan";
            fr.status = 0;
            fr.confidence = 60;
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(v.info, v.lenAd));
            fr.residentData.assign(v.body, v.body + n);
            w.cb->operator()(fr);
            ++w.emitted;
        } else if ((v.flags & 7u) == 0u && v.lenAd >= 8 && v.lenAd <= v.bodyMax) {
            emitAdFile(w, icb.part, parent, name, v.info, v.body, v.lenAd, false, v.mtime, v.ctime);
        } else if ((v.flags & 7u) == 1u && v.lenAd >= 16 && v.lenAd <= v.bodyMax) {
            emitAdFile(w, icb.part, parent, name, v.info, v.body, v.lenAd, true, v.mtime, v.ctime);
        }
    }
}

} // namespace

uint16_t udfCrc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    if (!p) return 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(p[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

bool UdfParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0);
}

bool UdfParser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                       uint64_t partitionOffsetBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;
    std::vector<uint8_t> avdp(kMediaSec, 0);
    auto tryAvdp = [&](uint32_t loc) {
        const uint64_t off = partitionOffsetBytes + static_cast<uint64_t>(loc) * kMediaSec;
        return readAt(reader, off, kMediaSec, avdp.data()) && validTag(avdp.data(), 512, 2, loc);
    };
    bool gotAvdp = tryAvdp(256);
    if (!gotAvdp) {
        const uint64_t vol = reader.getDiskSize();
        if (vol > partitionOffsetBytes) {
            const uint64_t media = (vol - partitionOffsetBytes) / kMediaSec;
            if (media >= 257) {
                const uint32_t backup = static_cast<uint32_t>(media - 1 - 256);
                if (backup != 256) gotAvdp = tryAvdp(backup);
            }
        }
    }
    if (!gotAvdp) return false;
    const uint32_t vdsLen = rd32(avdp.data() + 16);
    const uint32_t vdsLoc = rd32(avdp.data() + 20);
    if (vdsLen < kMediaSec || vdsLen > 64u * kMediaSec) return false;

    Walk w;
    w.reader = &reader;
    w.cb = &callback;
    w.running = isRunning;
    w.partOff = partitionOffsetBytes;
    LongAd fsdAd;
    bool gotLvd = false;
    std::vector<uint8_t> lvdSec;
    std::vector<uint8_t> pvdSec;
    const uint32_t nDesc = vdsLen / kMediaSec;
    std::vector<uint8_t> sec(kMediaSec, 0);
    for (uint32_t i = 0; i < nDesc; ++i) {
        const uint32_t loc = vdsLoc + i;
        if (!readAt(reader, partitionOffsetBytes + static_cast<uint64_t>(loc) * kMediaSec, kMediaSec,
                    sec.data()))
            continue;
        if (validTag(sec.data(), sec.size(), 8, loc)) break;
        if (validTag(sec.data(), sec.size(), 1, loc)) {
            pvdSec = sec;
        } else if (validTag(sec.data(), sec.size(), 5, loc)) {
            Part p;
            p.start = rd32(sec.data() + 188);
            p.length = rd32(sec.data() + 192);
            w.parts[rd16(sec.data() + 22)] = p;
        } else if (validTag(sec.data(), sec.size(), 6, loc)) {
            w.blockSize = rd32(sec.data() + 212);
            fsdAd = parseLongAd(sec.data() + 248);
            gotLvd = w.blockSize >= 512 && w.blockSize <= 4096 &&
                     (w.blockSize & (w.blockSize - 1)) == 0 && fsdAd.len > 0;
            if (gotLvd) lvdSec = sec;
        }
    }
    if (!gotLvd || w.parts.empty()) return false;
    if (!lvdSec.empty()) {
        parsePartitionMaps(w, lvdSec.data(), lvdSec.size());
        if (lvdSec.size() >= 84 + 128) {
            const std::string vid = udfDstring(lvdSec.data() + 84, 128);
            if (!vid.empty() && w.cb) {
                FileRecord fr;
                fr.name = vid;
                fr.path = "/";
                fr.status = 1;
                fr.confidence = 5;
                fr.category = "System";
                fr.source = "udf_vol_id";
                (*w.cb)(fr);
                ++w.emitted;
            }
        }
    }
    if (pvdSec.size() >= 24 + 32) {
        const std::string pvdId = udfDstring(pvdSec.data() + 24, 32);
        if (!pvdId.empty() && w.cb) {
            FileRecord fr;
            fr.name = pvdId;
            fr.path = "/";
            fr.status = 1;
            fr.confidence = 5;
            fr.category = "System";
            fr.source = "udf_pvd_id";
            (*w.cb)(fr);
            ++w.emitted;
        }
    }
    loadVat(w);

    std::vector<uint8_t> fsd(w.blockSize, 0);
    if (!readAt(reader, lbnBytes(w, fsdAd.part, fsdAd.lbn), w.blockSize, fsd.data()) ||
        !validTag(fsd.data(), fsd.size(), 256, fsdAd.lbn))
        return false;
    if (fsd.size() >= 304 + 32) {
        const std::string fsdId = udfDstring(fsd.data() + 304, 32);
        if (!fsdId.empty() && w.cb) {
            FileRecord fr;
            fr.name = fsdId;
            fr.path = "/";
            fr.status = 1;
            fr.confidence = 5;
            fr.category = "System";
            fr.source = "udf_fsd_id";
            (*w.cb)(fr);
            ++w.emitted;
        }
    }
    const LongAd root = parseLongAd(fsd.data() + 400);
    if (root.len == 0) return false;
    if (!walkIcb(w, root, "", 0)) return false;
    scanMetaOrphans(w);
    return w.emitted > 0;
}

} // namespace byteback
