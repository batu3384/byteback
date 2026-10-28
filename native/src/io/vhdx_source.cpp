#include "io/vhd_source.h"
#include "io/vhd_source_internal.h"
#include "fs/refs_integrity.h"
#include "io/byte_source.h"
#include "recovery/path_util.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace byteback {
namespace {

constexpr size_t kVhdFooter = 512;
constexpr size_t kDynHeader = 1024;
constexpr uint32_t kVhdTypeFixed = 2;
constexpr uint32_t kVhdTypeDynamic = 3;
constexpr uint32_t kVhdTypeDifferencing = 4;
constexpr uint32_t kDynBlockSize = 65536;
constexpr size_t k1MiB = 1u << 20;
constexpr size_t k64KiB = 64u << 10;
constexpr size_t kVhdxHeader = 4096;

constexpr uint8_t kGuidBat[16] = {
    0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42,
    0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08};
constexpr uint8_t kGuidMeta[16] = {
    0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B,
    0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E};
constexpr uint8_t kGuidFileParams[16] = {
    0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D,
    0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B};
constexpr uint8_t kGuidVirtSize[16] = {
    0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48,
    0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8};
constexpr uint8_t kGuidDiskId[16] = {
    0xAB, 0x12, 0xCA, 0xBE, 0xE6, 0xB2, 0x23, 0x45,
    0x93, 0xEF, 0xC3, 0x09, 0xE0, 0x00, 0xC7, 0x46};
constexpr uint8_t kGuidLogiSec[16] = {
    0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47,
    0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F};
constexpr uint8_t kGuidPhysSec[16] = {
    0xC7, 0x48, 0xA3, 0xCD, 0x5D, 0x44, 0x71, 0x44,
    0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56};
// MS-VHDX Parent Locator metadata item {A8D35F2D-B30B-454D-ABF7-D3D84834AB0C}
constexpr uint8_t kGuidParentLocator[16] = {
    0x2D, 0x5F, 0xD3, 0xA8, 0x0B, 0xB3, 0x4D, 0x45,
    0xAB, 0xF7, 0xD3, 0xD8, 0x48, 0x34, 0xAB, 0x0C};
// VHDX parent locator type {B7A83E0B-4DDA-4DD4-958D-7C0CADD07C65}
constexpr uint8_t kGuidParentType[16] = {
    0x0B, 0x3E, 0xA8, 0xB7, 0xDA, 0x4D, 0xD4, 0x4D,
    0x95, 0x8D, 0x7C, 0x0C, 0xAD, 0xD0, 0x7C, 0x65};

void writeBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void writeBe64(uint8_t* p, uint64_t v) {
    writeBe32(p, static_cast<uint32_t>(v >> 32));
    writeBe32(p + 4, static_cast<uint32_t>(v));
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | readBe32(p + 4);
}

void writeLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void writeLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void writeLe64(uint8_t* p, uint64_t v) {
    writeLe32(p, static_cast<uint32_t>(v));
    writeLe32(p + 4, static_cast<uint32_t>(v >> 32));
}

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

uint32_t onesComplementSum(const uint8_t* p, size_t n, size_t skipOff, size_t skipLen) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i >= skipOff && i < skipOff + skipLen) continue;
        sum += p[i];
    }
    return ~sum;
}

void writeCrc32c(uint8_t* p, size_t n, size_t crcOff) {
    std::memset(p + crcOff, 0, 4);
    writeLe32(p + crcOff, refsCrc32c(p, n));
}

bool crc32cOk(const uint8_t* p, size_t n, size_t crcOff) {
    std::vector<uint8_t> tmp(p, p + n);
    std::memset(tmp.data() + crcOff, 0, 4);
    return readLe32(p + crcOff) == refsCrc32c(tmp.data(), n);
}

void writeFooter(uint8_t* f, uint64_t virtualSize, uint32_t diskType, uint64_t dataOffset) {
    std::memset(f, 0, kVhdFooter);
    std::memcpy(f, "conectix", 8);
    writeBe32(f + 8, 2);
    writeBe32(f + 0x0C, 0x00010000);
    writeBe64(f + 0x10, dataOffset);
    std::memcpy(f + 0x1C, "bytb", 4);
    writeBe64(f + 0x28, virtualSize);
    writeBe64(f + 0x30, virtualSize);
    writeBe32(f + 0x3C, diskType);
    writeBe32(f + 0x40, onesComplementSum(f, kVhdFooter, 0x40, 4));
}

void writeVhdxHeader(uint8_t* h, uint64_t seq) {
    std::memset(h, 0, kVhdxHeader);
    std::memcpy(h, "head", 4);
    writeLe64(h + 8, seq);
    std::memset(h + 16, 0x11, 16);
    std::memset(h + 32, 0x22, 16);
    writeLe16(h + 66, 1);
    writeCrc32c(h, kVhdxHeader, 4);
}

void writeRegionTable(uint8_t* t, uint64_t metaOff, uint64_t batOff) {
    std::memset(t, 0, k64KiB);
    std::memcpy(t, "regi", 4);
    writeLe32(t + 8, 2);
    uint8_t* e0 = t + 16;
    std::memcpy(e0, kGuidBat, 16);
    writeLe64(e0 + 16, batOff);
    writeLe32(e0 + 24, static_cast<uint32_t>(k1MiB));
    writeLe32(e0 + 28, 1);
    uint8_t* e1 = t + 48;
    std::memcpy(e1, kGuidMeta, 16);
    writeLe64(e1 + 16, metaOff);
    writeLe32(e1 + 24, static_cast<uint32_t>(k1MiB));
    writeLe32(e1 + 28, 1);
    writeCrc32c(t, k64KiB, 4);
}

void writeMetaEntry(uint8_t* e, const uint8_t* guid, uint32_t off, uint32_t len, uint32_t bits) {
    std::memcpy(e, guid, 16);
    writeLe32(e + 16, off);
    writeLe32(e + 20, len);
    writeLe32(e + 24, bits);
}

bool replayVhdxLog(std::vector<uint8_t>& file, const uint8_t* hdr, std::string& err) {
    const uint64_t logOff = readLe64(hdr + 72);
    const uint32_t logLen = readLe32(hdr + 68);
    if (logOff % k1MiB != 0 || logLen % k1MiB != 0 || logLen == 0 || logLen > 16u * k1MiB) {
        err = "VHDX log geometry invalid";
        return false;
    }
    if (logOff > file.size() || logLen > file.size() - logOff) {
        err = "VHDX log out of range";
        return false;
    }
    const uint8_t* guid = hdr + 48;
    struct Hit {
        uint32_t pos = 0;
        uint32_t len = 0;
        uint64_t seq = 0;
    };
    std::vector<Hit> hits;
    uint32_t pos = 0;
    while (pos + 64 <= logLen) {
        uint8_t* e = file.data() + static_cast<size_t>(logOff) + pos;
        if (std::memcmp(e, "loge", 4) != 0) {
            pos += 4096;
            continue;
        }
        const uint32_t elen = readLe32(e + 8);
        if (elen < 4096 || (elen % 4096) != 0 || elen > logLen - pos) {
            pos += 4096;
            continue;
        }
        const uint64_t seq = readLe64(e + 16);
        if (seq == 0 || std::memcmp(e + 32, guid, 16) != 0) {
            pos += 4096;
            continue;
        }
        std::vector<uint8_t> tmp(e, e + elen);
        std::memset(tmp.data() + 4, 0, 4);
        if (readLe32(e + 4) != refsCrc32c(tmp.data(), tmp.size())) {
            pos += 4096;
            continue;
        }
        hits.push_back({pos, elen, seq});
        pos += elen;
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.seq < b.seq; });
    for (const auto& hit : hits) {
        uint8_t* e = file.data() + static_cast<size_t>(logOff) + hit.pos;
        const uint32_t nDesc = readLe32(e + 24);
        if (nDesc > 126) {
            err = "VHDX log descriptor count invalid";
            return false;
        }
        const uint64_t seq = hit.seq;
        uint32_t dataI = 0;
        for (uint32_t i = 0; i < nDesc; ++i) {
            uint8_t* d = e + 64 + static_cast<size_t>(i) * 32;
            if (readLe64(d + 24) != seq) {
                err = "VHDX log descriptor sequence mismatch";
                return false;
            }
            const uint64_t foff = readLe64(d + 16);
            if ((foff % 4096) != 0) {
                err = "VHDX log offset unaligned";
                return false;
            }
            if (std::memcmp(d, "desc", 4) == 0) {
                const uint64_t dpos = 4096ull + static_cast<uint64_t>(dataI) * 4096ull;
                if (dpos + 4096 > hit.len) {
                    err = "VHDX log data sector missing";
                    return false;
                }
                uint8_t* ds = e + static_cast<size_t>(dpos);
                if (std::memcmp(ds, "data", 4) != 0) {
                    err = "VHDX log data signature invalid";
                    return false;
                }
                const uint64_t dseq = (static_cast<uint64_t>(readLe32(ds + 4)) << 32) | readLe32(ds + 4092);
                if (dseq != seq) {
                    err = "VHDX log data sequence mismatch";
                    return false;
                }
                if (foff + 4096 > file.size()) {
                    err = "VHDX log write out of range";
                    return false;
                }
                uint8_t sec[4096];
                std::memcpy(sec, d + 8, 8);
                std::memcpy(sec + 8, ds + 8, 4084);
                std::memcpy(sec + 4092, d + 4, 4);
                std::memcpy(file.data() + static_cast<size_t>(foff), sec, 4096);
                ++dataI;
            } else if (std::memcmp(d, "zero", 4) == 0) {
                const uint64_t zlen = readLe64(d + 8);
                if ((zlen % 4096) != 0 || foff + zlen > file.size()) {
                    err = "VHDX log zero range invalid";
                    return false;
                }
                std::memset(file.data() + static_cast<size_t>(foff), 0, static_cast<size_t>(zlen));
            } else {
                err = "VHDX log descriptor invalid";
                return false;
            }
        }
    }
    return true;
}

bool utf16EqualsAscii(const uint8_t* p, size_t n, const char* ascii) {
    size_t i = 0;
    for (; ascii[i]; ++i) {
        if (i * 2 + 2 > n) return false;
        if (p[i * 2] != static_cast<uint8_t>(ascii[i]) || p[i * 2 + 1] != 0) return false;
    }
    return i * 2 == n;
}

std::string utf16ToAscii(const uint8_t* p, size_t n) {
    if ((n % 2) != 0) return {};
    std::string s;
    s.reserve(n / 2);
    for (size_t i = 0; i < n; i += 2) {
        if (p[i + 1] != 0) return {};
        s.push_back(static_cast<char>(p[i]));
    }
    return s;
}

bool parseVhdxParentLocator(const uint8_t* loc, size_t len, std::string& rel, std::string& absPath) {
    rel.clear();
    absPath.clear();
    if (!loc || len < 32 || std::memcmp(loc, kGuidParentType, 16) != 0) return false;
    const uint16_t kv = readLe16(loc + 18);
    if (kv == 0 || kv > 8) return false;
    for (uint16_t i = 0; i < kv; ++i) {
        const uint8_t* e = loc + 20 + static_cast<size_t>(i) * 12;
        if (e + 12 > loc + len) return false;
        const uint32_t kOff = readLe32(e);
        const uint32_t vOff = readLe32(e + 4);
        const uint16_t kLen = readLe16(e + 8);
        const uint16_t vLen = readLe16(e + 10);
        if (kOff + kLen > len || vOff + vLen > len) return false;
        if (utf16EqualsAscii(loc + kOff, kLen, "relative_win32_path")) {
            rel = utf16ToAscii(loc + vOff, vLen);
        } else if (utf16EqualsAscii(loc + kOff, kLen, "absolute_win32_path")) {
            absPath = utf16ToAscii(loc + vOff, vLen);
        }
    }
    return !rel.empty() || !absPath.empty();
}

bool extractVhdxPayloadImpl(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                        std::string& err, const std::string& imagePath) {
    payload.clear();
    std::vector<uint8_t> scratch;
    if (n < 5 * k64KiB) {
        err = "VHDX too small";
        return false;
    }
    const uint8_t* bestHdr = nullptr;
    uint64_t bestSeq = 0;
    const size_t hdrOffs[2] = {k64KiB, 2 * k64KiB};
    for (size_t off : hdrOffs) {
        if (off + kVhdxHeader > n) continue;
        const uint8_t* h = image + off;
        if (std::memcmp(h, "head", 4) != 0) continue;
        if (!crc32cOk(h, kVhdxHeader, 4)) continue;
        if (readLe16(h + 66) != 1) continue;
        const uint64_t seq = readLe64(h + 8);
        if (!bestHdr || seq > bestSeq) {
            bestHdr = h;
            bestSeq = seq;
        }
    }
    if (!bestHdr) {
        err = "VHDX header invalid";
        return false;
    }
    const uint32_t logLen = readLe32(bestHdr + 68);
    bool logGuid = false;
    for (int i = 0; i < 16; ++i) {
        if (bestHdr[48 + i] != 0) logGuid = true;
    }
    if (logGuid && logLen != 0) {
        scratch.assign(image, image + n);
        const size_t hdrPos = static_cast<size_t>(bestHdr - image);
        if (!replayVhdxLog(scratch, scratch.data() + hdrPos, err)) {
            payload.clear();
            return false;
        }
        image = scratch.data();
        n = scratch.size();
        bestHdr = scratch.data() + hdrPos;
    }

    const uint8_t* region = nullptr;
    const size_t regOffs[2] = {3 * k64KiB, 4 * k64KiB};
    for (size_t off : regOffs) {
        if (off + k64KiB > n) continue;
        const uint8_t* t = image + off;
        if (std::memcmp(t, "regi", 4) != 0) continue;
        if (!crc32cOk(t, k64KiB, 4)) continue;
        region = t;
        break;
    }
    if (!region) {
        err = "VHDX region table invalid";
        return false;
    }

    uint64_t batOff = 0, metaOff = 0;
    uint32_t batLen = 0, metaLen = 0;
    const uint32_t entryCount = readLe32(region + 8);
    if (entryCount == 0 || entryCount > 2047) {
        err = "VHDX region table invalid";
        return false;
    }
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = region + 16 + static_cast<size_t>(i) * 32;
        if (e + 32 > region + k64KiB) {
            err = "VHDX region table invalid";
            return false;
        }
        const uint64_t fileOff = readLe64(e + 16);
        const uint32_t length = readLe32(e + 24);
        const uint32_t required = readLe32(e + 28) & 1u;
        if ((fileOff % k1MiB) != 0 || (length % k1MiB) != 0 || length == 0) {
            err = "VHDX region alignment invalid";
            return false;
        }
        if (std::memcmp(e, kGuidBat, 16) == 0) {
            batOff = fileOff;
            batLen = length;
        } else if (std::memcmp(e, kGuidMeta, 16) == 0) {
            metaOff = fileOff;
            metaLen = length;
        } else if (required) {
            err = "unsupported required VHDX region";
            return false;
        }
    }
    if (batOff == 0 || metaOff == 0 || batOff >= n || metaOff >= n ||
        batLen > n - batOff || metaLen > n - metaOff || metaLen < k64KiB) {
        err = "VHDX BAT or metadata region missing";
        return false;
    }

    const uint8_t* meta = image + static_cast<size_t>(metaOff);
    if (std::memcmp(meta, "metadata", 8) != 0) {
        err = "VHDX metadata signature mismatch";
        return false;
    }
    const uint16_t metaCount = readLe16(meta + 10);
    if (metaCount == 0 || metaCount > 2047) {
        err = "VHDX metadata table invalid";
        return false;
    }

    uint32_t blockSize = 0;
    uint32_t fileParamBits = 0;
    uint64_t virtualSize = 0;
    uint32_t logicalSector = 0;
    bool sawFile = false, sawSize = false, sawLogi = false, sawDiskId = false, sawPhys = false;
    const uint8_t* parentLoc = nullptr;
    size_t parentLocLen = 0;
    for (uint16_t i = 0; i < metaCount; ++i) {
        const uint8_t* e = meta + 32 + static_cast<size_t>(i) * 32;
        if (e + 32 > meta + k64KiB) {
            err = "VHDX metadata table invalid";
            return false;
        }
        const uint32_t itemOff = readLe32(e + 16);
        const uint32_t itemLen = readLe32(e + 20);
        const uint32_t bits = readLe32(e + 24);
        if (itemLen == 0 || itemOff < k64KiB ||
            static_cast<uint64_t>(itemOff) + itemLen > metaLen) {
            err = "VHDX metadata item out of range";
            return false;
        }
        const uint8_t* item = meta + itemOff;
        if (std::memcmp(e, kGuidFileParams, 16) == 0 && itemLen >= 8) {
            blockSize = readLe32(item);
            fileParamBits = readLe32(item + 4);
            sawFile = true;
        } else if (std::memcmp(e, kGuidVirtSize, 16) == 0 && itemLen >= 8) {
            virtualSize = readLe64(item);
            sawSize = true;
        } else if (std::memcmp(e, kGuidLogiSec, 16) == 0 && itemLen >= 4) {
            logicalSector = readLe32(item);
            sawLogi = true;
        } else if (std::memcmp(e, kGuidDiskId, 16) == 0 && itemLen >= 16) {
            sawDiskId = true;
        } else if (std::memcmp(e, kGuidPhysSec, 16) == 0 && itemLen >= 4) {
            sawPhys = true;
        } else if (std::memcmp(e, kGuidParentLocator, 16) == 0) {
            parentLoc = item;
            parentLocLen = itemLen;
        } else if (bits & 4u) {
            err = "unsupported required VHDX metadata";
            return false;
        }
    }
    if (!sawFile || !sawSize || !sawLogi || !sawDiskId || !sawPhys) {
        err = "VHDX required metadata missing";
        return false;
    }
    std::vector<uint8_t> parentPayload;
    if (fileParamBits & 2u) {
        if (imagePath.empty()) {
            err = "VHDX parent / differencing not supported";
            return false;
        }
        if (!parentLoc || parentLocLen == 0) {
            err = "VHDX parent locator missing";
            return false;
        }
        std::string rel, absPath;
        if (!parseVhdxParentLocator(parentLoc, parentLocLen, rel, absPath)) {
            err = "VHDX parent locator invalid";
            return false;
        }
        std::string parentPath = absPath;
        if (parentPath.empty()) {
            if (rel.find('\\') != std::string::npos || rel.find('/') != std::string::npos ||
                rel.find(':') != std::string::npos) {
                err = "VHDX parent relative path invalid";
                return false;
            }
            parentPath = pathToUtf8(utf8Path(imagePath).parent_path() / utf8Path(rel));
        }
        if (parentPath.empty() || isHttpUrl(parentPath) || parentPath == imagePath) {
            err = "VHDX parent path invalid";
            return false;
        }
        if (!extractVhdFile(parentPath, parentPayload, err)) return false;
    }
    if (logicalSector != 512) {
        err = "VHDX logical sector size must be 512";
        return false;
    }
    if (virtualSize == 0 || (virtualSize % 512) != 0) {
        err = "invalid VHDX virtual size";
        return false;
    }
    if (virtualSize > kVhdFamilyMaxExpandBytes) {
        err = "VHDX virtual size exceeds 64 MiB cap";
        return false;
    }
    if (blockSize < k1MiB || blockSize > (256u << 20) || (blockSize & (blockSize - 1)) != 0) {
        err = "invalid VHDX block size";
        return false;
    }

    const uint64_t dataBlocks = (virtualSize + blockSize - 1) / blockSize;
    const uint64_t chunkRatio = (1ull << 23) * static_cast<uint64_t>(logicalSector) / blockSize;
    if (chunkRatio == 0 || dataBlocks == 0 || dataBlocks > 65536) {
        err = "invalid VHDX BAT size";
        return false;
    }
    const uint64_t batEntries = dataBlocks + (dataBlocks - 1) / chunkRatio;
    const uint64_t batBytes = batEntries * 8;
    if (batBytes > batLen) {
        err = "VHDX BAT out of range";
        return false;
    }

    if (fileParamBits & 2u) {
        if (parentPayload.size() != virtualSize) {
            err = "VHDX parent virtual size mismatch";
            payload.clear();
            return false;
        }
        payload = std::move(parentPayload);
    } else {
        payload.assign(static_cast<size_t>(virtualSize), 0);
    }
    const uint8_t* bat = image + static_cast<size_t>(batOff);
    uint64_t payloadIndex = 0;
    for (uint64_t i = 0; i < batEntries && payloadIndex < dataBlocks; ++i) {
        const bool isBitmap = ((i + 1) % (chunkRatio + 1)) == 0;
        if (isBitmap) continue;
        const uint64_t entry = readLe64(bat + static_cast<size_t>(i) * 8);
        const uint32_t state = static_cast<uint32_t>(entry & 7ull);
        const uint64_t virtOff = payloadIndex * blockSize;
        ++payloadIndex;
        if (virtOff >= virtualSize) break;
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(blockSize, virtualSize - virtOff));
        if (state == 2) {
            std::memset(payload.data() + static_cast<size_t>(virtOff), 0, chunk);
            continue;
        }
        if (state != 6 && state != 7) continue;
        const uint64_t fileOff = (entry >> 20) * k1MiB;
        if (fileOff == 0 || fileOff > n || chunk > n - fileOff) {
            err = "VHDX payload block out of range";
            payload.clear();
            return false;
        }
        std::memcpy(payload.data() + static_cast<size_t>(virtOff), image + static_cast<size_t>(fileOff),
                    chunk);
    }
    return true;
}

} // namespace

bool extractVhdxPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                        std::string& err, const std::string& imagePath) {
    return extractVhdxPayloadImpl(image, n, payload, err, imagePath);
}

std::vector<uint8_t> wrapVhdx(const std::vector<uint8_t>& payload, bool hasParent,
                              const std::string& parentRelative) {
    const uint64_t virtualSize = payload.size();
    const uint32_t blockSize = static_cast<uint32_t>(k1MiB);
    const uint32_t blocks = virtualSize == 0
        ? 0
        : static_cast<uint32_t>((virtualSize + blockSize - 1) / blockSize);
    const uint64_t metaOff = k1MiB;
    const uint64_t batOff = 2ull * k1MiB;
    uint64_t nextData = 3ull * k1MiB;

    const uint64_t absent = hasParent ? 0ull : 2ull;
    std::vector<uint64_t> bat(blocks, absent);
    std::vector<uint8_t> dataBlobs;
    for (uint32_t i = 0; i < blocks; ++i) {
        const size_t off = static_cast<size_t>(i) * blockSize;
        const size_t n = std::min(static_cast<size_t>(blockSize), payload.size() - off);
        bool used = false;
        for (size_t j = 0; j < n; ++j) {
            if (payload[off + j] != 0) {
                used = true;
                break;
            }
        }
        if (!used) continue;
        bat[i] = (nextData / k1MiB) << 20 | 6ull;
        std::vector<uint8_t> blk(blockSize, 0);
        std::memcpy(blk.data(), payload.data() + off, n);
        dataBlobs.insert(dataBlobs.end(), blk.begin(), blk.end());
        nextData += k1MiB;
    }

    std::vector<uint8_t> out(static_cast<size_t>(3ull * k1MiB), 0);
    std::memcpy(out.data(), "vhdxfile", 8);
    const char* creator = "byteback";
    for (size_t i = 0; creator[i]; ++i) {
        out[8 + i * 2] = static_cast<uint8_t>(creator[i]);
    }
    writeVhdxHeader(out.data() + k64KiB, 1);
    writeVhdxHeader(out.data() + 2 * k64KiB, 0);
    writeRegionTable(out.data() + 3 * k64KiB, metaOff, batOff);
    writeRegionTable(out.data() + 4 * k64KiB, metaOff, batOff);

    uint8_t* meta = out.data() + static_cast<size_t>(metaOff);
    std::memcpy(meta, "metadata", 8);
    const bool loc = hasParent && !parentRelative.empty();
    writeLe16(meta + 10, loc ? 6 : 5);
    constexpr uint32_t kItemBase = static_cast<uint32_t>(k64KiB);
    writeMetaEntry(meta + 32, kGuidFileParams, kItemBase, 8, 4);
    writeMetaEntry(meta + 64, kGuidVirtSize, kItemBase + 8, 8, 6);
    writeMetaEntry(meta + 96, kGuidDiskId, kItemBase + 16, 16, 6);
    writeMetaEntry(meta + 128, kGuidLogiSec, kItemBase + 32, 4, 6);
    writeMetaEntry(meta + 160, kGuidPhysSec, kItemBase + 36, 4, 6);
    writeLe32(meta + kItemBase, blockSize);
    writeLe32(meta + kItemBase + 4, hasParent ? 2u : 0u);
    writeLe64(meta + kItemBase + 8, virtualSize);
    std::memset(meta + kItemBase + 16, 0xAB, 16);
    writeLe32(meta + kItemBase + 32, 512);
    writeLe32(meta + kItemBase + 36, 512);
    if (loc) {
        const char* key = "relative_win32_path";
        const uint32_t kLen = static_cast<uint32_t>(std::strlen(key) * 2);
        const uint32_t vLen = static_cast<uint32_t>(parentRelative.size() * 2);
        const uint32_t kOff = 32;
        const uint32_t vOff = kOff + kLen;
        const uint32_t locLen = vOff + vLen;
        const uint32_t locOff = kItemBase + 40;
        writeMetaEntry(meta + 192, kGuidParentLocator, locOff, locLen, 4);
        uint8_t* pl = meta + locOff;
        std::memcpy(pl, kGuidParentType, 16);
        writeLe16(pl + 18, 1);
        writeLe32(pl + 20, kOff);
        writeLe32(pl + 24, vOff);
        writeLe16(pl + 28, static_cast<uint16_t>(kLen));
        writeLe16(pl + 30, static_cast<uint16_t>(vLen));
        for (uint32_t i = 0; key[i]; ++i) {
            pl[kOff + i * 2] = static_cast<uint8_t>(key[i]);
        }
        for (size_t i = 0; i < parentRelative.size(); ++i) {
            pl[vOff + i * 2] = static_cast<uint8_t>(parentRelative[i]);
        }
    }

    uint8_t* batp = out.data() + static_cast<size_t>(batOff);
    for (uint32_t i = 0; i < blocks; ++i) writeLe64(batp + static_cast<size_t>(i) * 8, bat[i]);
    out.insert(out.end(), dataBlobs.begin(), dataBlobs.end());
    return out;
}

} // namespace byteback