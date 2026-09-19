#include "fs/iso9660_parser.h"
#include "fs/ntfs_util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace byteback {
namespace {

constexpr uint32_t kIsoSec = 2048;
constexpr int kMaxDepth = 32;
constexpr size_t kMaxFiles = 65536;
constexpr uint32_t kMaxDirBytes = 4u * 1024u * 1024u;

uint16_t rdBoth16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t rdBoth32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t iso7ByteUnix(const uint8_t* p) {
    if (!p) return 0;
    const unsigned m = p[1];
    const unsigned d = p[2];
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    const int hh = p[3];
    const int mm = p[4];
    const int ss = p[5];
    if (hh > 23 || mm > 59 || ss > 59) return 0;
    const int64_t t = daysFromCivil(1900 + p[0], m, d) * 86400 +
                      static_cast<int64_t>(hh) * 3600 + static_cast<int64_t>(mm) * 60 + ss;
    return t - static_cast<int64_t>(static_cast<int8_t>(p[6])) * 15 * 60;
}

int64_t isoRecordingUnix(const uint8_t* rec) {
    return rec ? iso7ByteUnix(rec + 18) : 0;
}

int64_t suspTf(const uint8_t* p, size_t n) {
    if (!p) return 0;
    size_t i = 0;
    while (i + 5 <= n) {
        const uint8_t len = p[i + 2];
        if (len < 5 || i + len > n) break;
        if (p[i] == 'T' && p[i + 1] == 'F') {
            const uint8_t flags = p[i + 4];
            const uint8_t step = (flags & 0x80u) ? 17 : 7;
            size_t c = i + 5;
            int64_t modify = 0;
            for (uint8_t bit = 0x01; bit != 0 && bit <= 0x40; bit = static_cast<uint8_t>(bit << 1)) {
                if ((flags & bit) == 0) continue;
                if (c + step > i + len) break;
                if (bit == 0x02 && step == 7) modify = iso7ByteUnix(p + c);
                c += step;
            }
            return modify;
        }
        i += len;
    }
    return 0;
}

int64_t rockRidgeTf(const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    if (!rec || recLen < 34) return 0;
    size_t i = static_cast<size_t>(33) + idLen;
    if (i & 1u) ++i;
    if (i >= recLen) return 0;
    return suspTf(rec + i, recLen - i);
}

bool looksPvd(const uint8_t* p, size_t n) {
    return n >= 7 && p[0] == 1 && std::memcmp(p + 1, "CD001", 5) == 0 && p[6] == 1;
}

bool looksElTorito(const uint8_t* p, size_t n) {
    return n >= 75 && p[0] == 0 && std::memcmp(p + 1, "CD001", 5) == 0 && p[6] == 1 &&
           std::memcmp(p + 7, "EL TORITO SPECIFICATION", 23) == 0;
}

uint16_t rdLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

bool looksJolietSvd(const uint8_t* p, size_t n) {
    if (n < 91 || p[0] != 2 || std::memcmp(p + 1, "CD001", 5) != 0 || p[6] != 1) return false;
    return p[88] == 0x25 && p[89] == 0x2F && (p[90] == 0x40 || p[90] == 0x43 || p[90] == 0x45);
}

bool readVolumeDesc(DiskReader& reader, uint64_t partOff, uint8_t typeWanted, bool joliet, uint8_t* out) {
    for (uint32_t s = 16; s < 32; ++s) {
        const uint64_t off = partOff + static_cast<uint64_t>(s) * kIsoSec;
        if (!readComplete(reader.readSectors(off, kIsoSec, out), kIsoSec)) continue;
        if (out[0] == 255 && std::memcmp(out + 1, "CD001", 5) == 0) break;
        if (joliet) {
            if (looksJolietSvd(out, kIsoSec)) return true;
        } else if (typeWanted == 1 && looksPvd(out, kIsoSec)) {
            return true;
        }
    }
    return false;
}

bool readPvd(DiskReader& reader, uint64_t partOff, uint8_t* out) {
    return readVolumeDesc(reader, partOff, 1, false, out);
}

std::string stripIsoName(std::string s) {
    const auto semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s;
}

std::string isoVolumeId(const uint8_t* desc, bool joliet) {
    if (!desc) return {};
    if (joliet) {
        uint16_t u[16];
        for (int i = 0; i < 16; ++i)
            u[i] = static_cast<uint16_t>((desc[40 + 2 * i] << 8) | desc[40 + 2 * i + 1]);
        int n = 16;
        while (n > 0 && (u[n - 1] == 0 || u[n - 1] == static_cast<uint16_t>(' '))) --n;
        if (n <= 0) return {};
        return ntfs::utf16leToUtf8(u, static_cast<size_t>(n));
    }
    size_t n = 32;
    while (n > 0 && (desc[40 + n - 1] == ' ' || desc[40 + n - 1] == 0)) --n;
    if (n == 0) return {};
    return std::string(reinterpret_cast<const char*>(desc + 40), n);
}

std::string isoName(const uint8_t* id, uint8_t idLen, bool joliet) {
    if (idLen == 0) return {};
    if (idLen == 1 && (id[0] == 0 || id[0] == 1)) return {};
    if (joliet) {
        if (idLen < 2 || (idLen % 2u) != 0) return {};
        std::vector<uint16_t> u(idLen / 2);
        for (size_t i = 0; i < u.size(); ++i)
            u[i] = static_cast<uint16_t>((id[2 * i] << 8) | id[2 * i + 1]);
        return stripIsoName(ntfs::utf16leToUtf8(u.data(), u.size()));
    }
    return stripIsoName(std::string(reinterpret_cast<const char*>(id), idLen));
}

std::string suspNm(const uint8_t* p, size_t n) {
    if (!p) return {};
    size_t i = 0;
    while (i + 4 <= n) {
        const uint8_t len = p[i + 2];
        if (len < 4 || i + len > n) break;
        if (p[i] == 'N' && p[i + 1] == 'M' && len >= 5) {
            const uint8_t flags = p[i + 4];
            if ((flags & 0x06u) == 0 && len > 5) {
                return std::string(reinterpret_cast<const char*>(p + i + 5),
                                   static_cast<size_t>(len - 5));
            }
        }
        i += len;
    }
    return {};
}

std::string rockRidgeNm(const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    if (!rec || recLen < 34) return {};
    size_t i = static_cast<size_t>(33) + idLen;
    if (i & 1u) ++i;
    if (i >= recLen) return {};
    return suspNm(rec + i, recLen - i);
}

std::string suspSl(const uint8_t* p, size_t n) {
    if (!p) return {};
    size_t i = 0;
    while (i + 5 <= n) {
        const uint8_t len = p[i + 2];
        if (len < 5 || i + len > n) break;
        if (p[i] == 'S' && p[i + 1] == 'L') {
            std::string out;
            size_t c = i + 5;
            const size_t end = i + len;
            while (c + 2 <= end) {
                const uint8_t cf = p[c];
                const uint8_t cl = p[c + 1];
                c += 2;
                if (cf & 0x08u) {
                    if (out.empty()) out.push_back('/');
                }
                if (cf & 0x02u) {
                    if (!out.empty() && out.back() != '/') out.push_back('/');
                    out.push_back('.');
                } else if (cf & 0x04u) {
                    if (!out.empty() && out.back() != '/') out.push_back('/');
                    out.append("..");
                } else if (cl > 0 && c + cl <= end) {
                    if (!out.empty() && out.back() != '/') out.push_back('/');
                    out.append(reinterpret_cast<const char*>(p + c), cl);
                    c += cl;
                }
            }
            return out;
        }
        i += len;
    }
    return {};
}

bool readRockRidgeCe(DiskReader& reader, uint64_t partOff, uint32_t blockSize,
                     const uint8_t* rec, uint8_t recLen, uint8_t idLen,
                     std::vector<uint8_t>* out) {
    if (!rec || !out || recLen < 34 || blockSize == 0) return false;
    size_t i = static_cast<size_t>(33) + idLen;
    if (i & 1u) ++i;
    while (i + 28 <= recLen) {
        const uint8_t len = rec[i + 2];
        if (len < 4 || i + len > recLen) break;
        if (rec[i] == 'C' && rec[i + 1] == 'E' && len >= 28) {
            const uint32_t lba = rdBoth32(rec + i + 4);
            const uint32_t off = rdBoth32(rec + i + 12);
            const uint32_t n = rdBoth32(rec + i + 20);
            if (n < 5 || n > 4096) return false;
            out->assign(n, 0);
            const uint64_t byteOff = partOff + static_cast<uint64_t>(lba) * blockSize + off;
            if (!readComplete(reader.readBytes(byteOff, n, out->data()), n)) return false;
            return true;
        }
        i += len;
    }
    return false;
}

std::string rockRidgeNmFromCe(DiskReader& reader, uint64_t partOff, uint32_t blockSize,
                              const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    std::vector<uint8_t> buf;
    if (!readRockRidgeCe(reader, partOff, blockSize, rec, recLen, idLen, &buf)) return {};
    return suspNm(buf.data(), buf.size());
}

std::string rockRidgeSl(const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    if (!rec || recLen < 34) return {};
    size_t i = static_cast<size_t>(33) + idLen;
    if (i & 1u) ++i;
    if (i >= recLen) return {};
    return suspSl(rec + i, recLen - i);
}

std::string rockRidgeSlFromCe(DiskReader& reader, uint64_t partOff, uint32_t blockSize,
                              const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    std::vector<uint8_t> buf;
    if (!readRockRidgeCe(reader, partOff, blockSize, rec, recLen, idLen, &buf)) return {};
    return suspSl(buf.data(), buf.size());
}

int64_t rockRidgeTfFromCe(DiskReader& reader, uint64_t partOff, uint32_t blockSize,
                          const uint8_t* rec, uint8_t recLen, uint8_t idLen) {
    std::vector<uint8_t> buf;
    if (!readRockRidgeCe(reader, partOff, blockSize, rec, recLen, idLen, &buf)) return 0;
    return suspTf(buf.data(), buf.size());
}

void emitFile(FileSystemParser::FileRecordCallback& cb, const std::string& parent,
              const std::string& name, uint64_t byteOff, uint32_t size, uint32_t sectorSize,
              const uint8_t* resident = nullptr, uint32_t residentLen = 0, int64_t modifiedAt = 0) {
    FileRecord fr;
    fr.name = name;
    fr.path = parent.empty() ? ("/" + name) : (parent + "/" + name);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size()) fr.extension = name.substr(dot + 1);
    fr.sizeBytes = size;
    fr.startSector = byteOff / sectorSize;
    fr.startByteOffset = byteOff % sectorSize;
    fr.endSector = (byteOff + (size ? size - 1 : 0)) / sectorSize;
    const uint64_t need = size + fr.startByteOffset;
    fr.runs.push_back({fr.startSector, (need + sectorSize - 1) / sectorSize});
    if (resident && residentLen) {
        const uint32_t n = std::min(residentLen, size);
        fr.residentData.assign(resident, resident + n);
    }
    fr.status = 1;
    fr.confidence = 90;
    fr.category = "Unknown";
    fr.source = "iso9660";
    fr.modifiedAt = modifiedAt;
    cb(fr);
}

struct Walk {
    DiskReader* reader = nullptr;
    FileSystemParser::FileRecordCallback* cb = nullptr;
    std::atomic<bool>* running = nullptr;
    uint64_t partOff = 0;
    uint32_t blockSize = 2048;
    uint32_t sectorSize = 512;
    size_t emitted = 0;
    std::set<uint64_t> seen;
    bool joliet = false;
};

void emitStitched(Walk& w, const std::string& parent, const std::string& name,
                  const std::vector<std::pair<uint32_t, uint32_t>>& exts, int64_t modifiedAt) {
    if (exts.empty() || !w.cb || w.sectorSize == 0) return;
    uint64_t size = 0;
    for (const auto& e : exts) size += e.second;
    if (size == 0) return;

    FileRecord fr;
    fr.name = name;
    fr.path = parent.empty() ? ("/" + name) : (parent + "/" + name);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size()) fr.extension = name.substr(dot + 1);
    fr.sizeBytes = size;
    fr.status = 1;
    fr.confidence = 90;
    fr.category = "Unknown";
    fr.source = "iso9660";
    fr.modifiedAt = modifiedAt;
    bool first = true;
    for (const auto& e : exts) {
        if (e.second == 0) continue;
        const uint64_t byteOff = w.partOff + static_cast<uint64_t>(e.first) * w.blockSize;
        const uint64_t startSec = byteOff / w.sectorSize;
        const uint64_t skip = byteOff % w.sectorSize;
        const uint64_t need = static_cast<uint64_t>(e.second) + skip;
        uint64_t secCount = (need + w.sectorSize - 1) / w.sectorSize;
        if (secCount == 0) continue;
        fr.runs.push_back({startSec, secCount, e.second});
        if (first) {
            fr.startSector = startSec;
            fr.startByteOffset = skip;
            first = false;
        }
        fr.endSector = startSec + secCount - 1;
    }
    if (fr.runs.empty()) return;
    (*w.cb)(fr);
    ++w.emitted;
}

bool walkDir(Walk& w, uint32_t lba, uint32_t dataLen, const std::string& parent, int depth) {
    if (depth > kMaxDepth || dataLen == 0 || dataLen > kMaxDirBytes) return true;
    if (w.running && !(*w.running)) return false;
    const uint64_t key = (static_cast<uint64_t>(lba) << 32) | dataLen;
    if (!w.seen.insert(key).second) return true;
    std::vector<uint8_t> buf(dataLen, 0);
    const uint64_t off = w.partOff + static_cast<uint64_t>(lba) * w.blockSize;
    if (!readComplete(w.reader->readBytes(off, dataLen, buf.data()), dataLen)) return true;
    std::vector<std::pair<uint32_t, uint32_t>> pending;
    std::string pendingName;
    int64_t pendingMtime = 0;
    auto flushPending = [&]() {
        if (pending.empty()) return;
        emitStitched(w, parent, pendingName, pending, pendingMtime);
        pending.clear();
        pendingName.clear();
        pendingMtime = 0;
    };
    size_t i = 0;
    while (i < buf.size()) {
        if (w.running && !(*w.running)) return false;
        if (w.emitted >= kMaxFiles) return true;
        const uint8_t recLen = buf[i];
        if (recLen == 0) {
            const size_t next = ((i / w.blockSize) + 1) * w.blockSize;
            if (next <= i || next >= buf.size()) break;
            i = next;
            continue;
        }
        if (recLen < 34 || i + recLen > buf.size()) break;
        const uint8_t* rec = buf.data() + i;
        const uint8_t flags = rec[25];
        const uint8_t idLen = rec[32];
        if (static_cast<size_t>(33) + idLen > recLen) {
            i += recLen;
            continue;
        }
        if (flags & 0x04u) {
            i += recLen;
            continue;
        }
        const uint32_t extLba = rdBoth32(rec + 2);
        const uint32_t extLen = rdBoth32(rec + 10);
        const std::string iso = isoName(rec + 33, idLen, w.joliet);
        std::string rr = w.joliet ? std::string() : rockRidgeNm(rec, recLen, idLen);
        if (rr.empty() && !w.joliet && w.reader)
            rr = rockRidgeNmFromCe(*w.reader, w.partOff, w.blockSize, rec, recLen, idLen);
        const std::string slRec = w.joliet ? std::string() : rockRidgeSl(rec, recLen, idLen);
        const std::string slCe = (slRec.empty() && !w.joliet && w.reader)
            ? rockRidgeSlFromCe(*w.reader, w.partOff, w.blockSize, rec, recLen, idLen)
            : std::string();
        const std::string sl = slRec.empty() ? slCe : slRec;
        const std::string name = rr.empty() ? iso : rr;
        const int64_t tfRec = w.joliet ? 0 : rockRidgeTf(rec, recLen, idLen);
        const int64_t tfCe = (tfRec == 0 && !w.joliet && w.reader)
            ? rockRidgeTfFromCe(*w.reader, w.partOff, w.blockSize, rec, recLen, idLen)
            : 0;
        const int64_t tf = tfRec != 0 ? tfRec : tfCe;
        const int64_t mtime = tf != 0 ? tf : isoRecordingUnix(rec);
        if (name.empty()) {
            i += recLen;
            continue;
        }
        if (flags & 0x02u) {
            flushPending();
            if (!walkDir(w, extLba, extLen, parent.empty() ? ("/" + name) : (parent + "/" + name),
                         depth + 1))
                return false;
        } else if (flags & 0x80u) {
            if (!pending.empty() && pendingName != name) flushPending();
            if (pending.empty()) pendingMtime = mtime;
            pendingName = name;
            pending.push_back({extLba, extLen});
        } else if (!pending.empty() && pendingName == name) {
            pending.push_back({extLba, extLen});
            flushPending();
        } else {
            flushPending();
            if (!sl.empty()) {
                FileRecord fr;
                fr.name = name;
                fr.path = parent.empty() ? ("/" + name) : (parent + "/" + name);
                fr.sizeBytes = sl.size();
                fr.residentData.assign(sl.begin(), sl.end());
                fr.status = 1;
                fr.confidence = 85;
                fr.category = "Unknown";
                fr.source = "iso9660_rr_sl";
                fr.modifiedAt = mtime;
                (*w.cb)(fr);
                ++w.emitted;
            } else {
                const uint64_t byteOff = w.partOff + static_cast<uint64_t>(extLba) * w.blockSize;
                emitFile(*w.cb, parent, name, byteOff, extLen, w.sectorSize, nullptr, 0, mtime);
                ++w.emitted;
            }
        }
        i += recLen;
    }
    flushPending();
    return true;
}

void emitElTorito(Walk& w) {
    uint8_t desc[kIsoSec];
    uint32_t catalogLba = 0;
    for (uint32_t s = 16; s < 32; ++s) {
        const uint64_t off = w.partOff + static_cast<uint64_t>(s) * kIsoSec;
        if (!readComplete(w.reader->readSectors(off, kIsoSec, desc), kIsoSec)) continue;
        if (desc[0] == 255 && std::memcmp(desc + 1, "CD001", 5) == 0) break;
        if (!looksElTorito(desc, kIsoSec)) continue;
        catalogLba = rdBoth32(desc + 71);
        break;
    }
    if (catalogLba == 0) return;
    uint8_t cat[kIsoSec];
    const uint64_t catOff = w.partOff + static_cast<uint64_t>(catalogLba) * w.blockSize;
    if (!readComplete(w.reader->readBytes(catOff, kIsoSec, cat), kIsoSec)) return;
    if (cat[0] != 0x01 || cat[0x1E] != 0x55 || cat[0x1F] != 0xAA) return;
    uint16_t sum = 0;
    for (int i = 0; i < 16; ++i)
        sum = static_cast<uint16_t>(sum + rdLe16(cat + 2 * i));
    if (sum != 0) return;
    for (size_t off = 32; off + 32 <= kIsoSec; off += 32) {
        const uint8_t* ent = cat + off;
        const uint8_t id = ent[0];
        if (id == 0x90 || id == 0x91) continue;
        if (id != 0x88) continue;
        const uint16_t sectCount = rdLe16(ent + 6);
        const uint32_t rba = rdBoth32(ent + 8);
        if (rba == 0) continue;
        uint64_t size = sectCount ? static_cast<uint64_t>(sectCount) * 512u : kIsoSec;
        if (size > 32ull * 1024u * 1024u) size = 32ull * 1024u * 1024u;
        const uint64_t byteOff = w.partOff + static_cast<uint64_t>(rba) * w.blockSize;
        emitFile(*w.cb, "", "BOOT.IMG", byteOff, static_cast<uint32_t>(size), w.sectorSize);
        ++w.emitted;
        break;
    }
}

} // namespace

bool Iso9660Parser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0);
}

bool Iso9660Parser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                           uint64_t partitionOffsetBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;
    uint8_t pvd[kIsoSec];
    if (!readPvd(reader, partitionOffsetBytes, pvd)) return false;
    const uint16_t blockSize = rdBoth16(pvd + 128);
    if (blockSize < 512 || blockSize > 4096 || (blockSize & (blockSize - 1)) != 0) return false;
    bool joliet = false;
    uint32_t rootLba = 0;
    uint32_t rootLenBytes = 0;
    uint8_t svd[kIsoSec];
    if (readVolumeDesc(reader, partitionOffsetBytes, 2, true, svd)) {
        const uint8_t jLen = svd[156];
        if (jLen >= 34 && static_cast<size_t>(156) + jLen <= kIsoSec) {
            rootLba = rdBoth32(svd + 156 + 2);
            rootLenBytes = rdBoth32(svd + 156 + 10);
            joliet = rootLba != 0 && rootLenBytes != 0;
        }
    }
    if (!joliet) {
        const uint8_t rootLen = pvd[156];
        if (rootLen < 34 || static_cast<size_t>(156) + rootLen > kIsoSec) return false;
        rootLba = rdBoth32(pvd + 156 + 2);
        rootLenBytes = rdBoth32(pvd + 156 + 10);
    }
    if (rootLba == 0 || rootLenBytes == 0) return false;
    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;
    Walk w;
    w.reader = &reader;
    w.cb = &callback;
    w.running = isRunning;
    w.partOff = partitionOffsetBytes;
    w.blockSize = blockSize;
    w.sectorSize = ss;
    w.joliet = joliet;
    {
        std::string vid = isoVolumeId(joliet ? svd : pvd, joliet);
        if (vid.empty() && joliet) vid = isoVolumeId(pvd, false);
        if (!vid.empty()) {
            FileRecord fr;
            fr.name = vid;
            fr.path = "/";
            fr.status = 1;
            fr.confidence = 5;
            fr.category = "System";
            fr.source = "iso9660_vol_id";
            callback(fr);
            ++w.emitted;
        }
    }
    if (!walkDir(w, rootLba, rootLenBytes, "", 0)) return false;
    emitElTorito(w);
    return w.emitted > 0;
}

} // namespace byteback
