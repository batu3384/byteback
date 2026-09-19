#include "io/vhd_source.h"
#include "fs/refs_integrity.h"
#include "io/byte_source.h"
#include "recovery/path_util.h"
#include "zlib.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
constexpr uint64_t kMaxExpand = 64ull << 20;

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

bool extractVhdxPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
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
    if (virtualSize > kMaxExpand) {
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

bool descriptorHasParent(const uint8_t* d, size_t n) {
    const char key[] = "parentCID=";
    const size_t klen = sizeof(key) - 1;
    if (!d || n < klen + 8) return false;
    for (size_t i = 0; i + klen + 8 <= n; ++i) {
        if (std::memcmp(d + i, key, klen) != 0) continue;
        uint32_t cid = 0;
        bool ok = true;
        for (int j = 0; j < 8; ++j) {
            const unsigned char c = d[i + klen + static_cast<size_t>(j)];
            cid <<= 4;
            if (c >= '0' && c <= '9') cid |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cid |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cid |= static_cast<uint32_t>(c - 'A' + 10);
            else {
                ok = false;
                break;
            }
        }
        if (ok && cid != 0xFFFFFFFFu) return true;
    }
    return false;
}

bool descriptorParentHint(const uint8_t* d, size_t n, std::string& hint) {
    hint.clear();
    const char key[] = "parentFileNameHint=\"";
    const size_t klen = sizeof(key) - 1;
    if (!d || n < klen + 1) return false;
    for (size_t i = 0; i + klen < n; ++i) {
        if (std::memcmp(d + i, key, klen) != 0) continue;
        std::string s;
        for (size_t j = i + klen; j < n; ++j) {
            if (d[j] == '"') {
                hint = std::move(s);
                return !hint.empty();
            }
            if (d[j] < 32) return false;
            s.push_back(static_cast<char>(d[j]));
            if (s.size() > 255) return false;
        }
        return false;
    }
    return false;
}

bool extractVmdkPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                        std::string& err, const std::string& imagePath) {
    payload.clear();
    if (!image || n < 512 || std::memcmp(image, "KDMV", 4) != 0) {
        err = "not a VMDK sparse header";
        return false;
    }
    const uint32_t version = readLe32(image + 4);
    const uint32_t flags = readLe32(image + 8);
    const uint64_t capacity = readLe64(image + 12);
    const uint64_t grainSize = readLe64(image + 20);
    const uint64_t descOff = readLe64(image + 28);
    const uint64_t descSize = readLe64(image + 36);
    const uint32_t gtesPerGt = readLe32(image + 44);
    const uint64_t gdOff = readLe64(image + 56);
    const uint16_t compress = readLe16(image + 77);
    if (version < 1 || version > 3) {
        err = "unsupported VMDK version";
        return false;
    }
    if (compress > 1) {
        err = "unsupported VMDK compress algorithm";
        return false;
    }
    const bool zipped = ((flags & (1u << 16)) != 0) || compress != 0;
    if (capacity == 0 || grainSize == 0 || grainSize > 2048 || (grainSize & (grainSize - 1)) != 0) {
        err = "invalid VMDK grain size";
        return false;
    }
    if (gtesPerGt == 0 || gtesPerGt > 512) {
        err = "invalid VMDK grain table";
        return false;
    }
    const uint64_t virtBytes = capacity * 512;
    if (virtBytes > kMaxExpand) {
        err = "VMDK virtual size exceeds 64 MiB cap";
        return false;
    }
    if (descSize > 0 && descOff > 0) {
        const uint64_t descBytes = descSize * 512;
        const uint64_t descByteOff = descOff * 512;
        if (descByteOff > n || descBytes > n - descByteOff) {
            err = "VMDK descriptor out of range";
            return false;
        }
        if (descriptorHasParent(image + static_cast<size_t>(descByteOff),
                                static_cast<size_t>(descBytes))) {
            if (imagePath.empty()) {
                err = "VMDK parent / differencing not supported";
                return false;
            }
            std::string hint;
            if (!descriptorParentHint(image + static_cast<size_t>(descByteOff),
                                      static_cast<size_t>(descBytes), hint) ||
                hint.find('\\') != std::string::npos || hint.find('/') != std::string::npos ||
                hint.find(':') != std::string::npos) {
                err = "VMDK parentFileNameHint invalid";
                return false;
            }
            const std::string parentPath =
                pathToUtf8(utf8Path(imagePath).parent_path() / utf8Path(hint));
            if (parentPath.empty() || isHttpUrl(parentPath) || parentPath == imagePath) {
                err = "VMDK parent path invalid";
                return false;
            }
            if (!extractVhdFile(parentPath, payload, err)) return false;
        }
    }
    const uint64_t numGrains = (capacity + grainSize - 1) / grainSize;
    const uint64_t numGts = (numGrains + gtesPerGt - 1) / gtesPerGt;
    if (numGts == 0 || numGts > 4096) {
        err = "invalid VMDK grain directory";
        return false;
    }
    const uint64_t gdByteOff = gdOff * 512;
    const uint64_t gdBytes = numGts * 4;
    if (gdOff == 0 || gdByteOff > n || gdBytes > n - gdByteOff) {
        err = "VMDK grain directory out of range";
        return false;
    }
    if (payload.empty()) payload.assign(static_cast<size_t>(virtBytes), 0);
    if (payload.size() != virtBytes) {
        err = "VMDK parent virtual size mismatch";
        payload.clear();
        return false;
    }
    const uint8_t* gd = image + static_cast<size_t>(gdByteOff);
    const uint64_t grainBytes = grainSize * 512;
    for (uint64_t gtIndex = 0; gtIndex < numGts; ++gtIndex) {
        const uint32_t gtSector = readLe32(gd + static_cast<size_t>(gtIndex) * 4);
        if (gtSector == 0) continue;
        const uint64_t gtOff = static_cast<uint64_t>(gtSector) * 512;
        const uint64_t gtBytes = static_cast<uint64_t>(gtesPerGt) * 4;
        if (gtOff > n || gtBytes > n - gtOff) {
            err = "VMDK grain table out of range";
            payload.clear();
            return false;
        }
        const uint8_t* gt = image + static_cast<size_t>(gtOff);
        for (uint32_t gte = 0; gte < gtesPerGt; ++gte) {
            const uint64_t grainIndex = gtIndex * gtesPerGt + gte;
            if (grainIndex >= numGrains) break;
            const uint32_t grainSector = readLe32(gt + static_cast<size_t>(gte) * 4);
            if (grainSector == 0) continue;
            const uint64_t srcOff = static_cast<uint64_t>(grainSector) * 512;
            const uint64_t dstOff = grainIndex * grainBytes;
            const size_t chunk = static_cast<size_t>(std::min(grainBytes, virtBytes - dstOff));
            if (srcOff > n) {
                err = "VMDK grain out of range";
                payload.clear();
                return false;
            }
            if (!zipped) {
                if (chunk > n - srcOff) {
                    err = "VMDK grain out of range";
                    payload.clear();
                    return false;
                }
                std::memcpy(payload.data() + static_cast<size_t>(dstOff),
                            image + static_cast<size_t>(srcOff), chunk);
                continue;
            }
            if (n - srcOff < 12) {
                err = "VMDK compressed grain marker truncated";
                payload.clear();
                return false;
            }
            const uint32_t zlen = readLe32(image + static_cast<size_t>(srcOff) + 8);
            if (zlen == 0 || zlen > grainBytes * 2 || 12ull + zlen > n - srcOff) {
                err = "VMDK compressed grain out of range";
                payload.clear();
                return false;
            }
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit(&zs) != Z_OK) {
                err = "VMDK compressed grain inflate init failed";
                payload.clear();
                return false;
            }
            std::vector<uint8_t> plain(static_cast<size_t>(grainBytes), 0);
            zs.next_in = const_cast<Bytef*>(image + static_cast<size_t>(srcOff) + 12);
            zs.avail_in = zlen;
            zs.next_out = plain.data();
            zs.avail_out = static_cast<uInt>(plain.size());
            const int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END) {
                err = "VMDK compressed grain inflate failed";
                payload.clear();
                return false;
            }
            std::memcpy(payload.data() + static_cast<size_t>(dstOff), plain.data(), chunk);
        }
    }
    return true;
}

bool findVdiParentPath(const std::string& childPath, const uint8_t* parentUuid, std::string& out,
                       std::string& err) {
    out.clear();
    std::error_code ec;
    const auto dir = utf8Path(childPath).parent_path();
    int seen = 0;
    for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (++seen > 32) break;
        if (!ent.is_regular_file(ec)) continue;
        auto ext = pathToUtf8(ent.path().extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".vdi") continue;
        const std::string cand = pathToUtf8(ent.path());
        if (cand == childPath) continue;
        std::ifstream in(utf8Path(cand), std::ios::binary);
        if (!in) continue;
        uint8_t hdr[424]{};
        in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (static_cast<size_t>(in.gcount()) < sizeof(hdr)) continue;
        if (readLe32(hdr + 64) != 0xBEDA107Fu) continue;
        if (std::memcmp(hdr + 392, parentUuid, 16) == 0) {
            out = cand;
            return true;
        }
    }
    err = "VDI parent image not found";
    return false;
}

bool extractVdiPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                       std::string& err, const std::string& imagePath) {
    payload.clear();
    if (!image || n < 400 || readLe32(image + 64) != 0xBEDA107Fu) {
        err = "not a VDI header";
        return false;
    }
    const uint32_t type = readLe32(image + 76);
    const uint32_t offBmap = readLe32(image + 340);
    const uint32_t offData = readLe32(image + 344);
    const uint32_t sectorSize = readLe32(image + 360);
    const uint64_t diskSize = readLe64(image + 368);
    const uint32_t blockSize = readLe32(image + 376);
    const uint32_t blocks = readLe32(image + 384);
    if (type == 4 || type == 3) {
        if (imagePath.empty()) {
            err = "VDI differencing / undo not supported";
            return false;
        }
        if (n < 424) {
            err = "VDI parent UUID missing";
            return false;
        }
        std::string parentPath;
        if (!findVdiParentPath(imagePath, image + 408, parentPath, err)) return false;
        if (isHttpUrl(parentPath) || parentPath == imagePath) {
            err = "VDI parent path invalid";
            return false;
        }
        if (!extractVhdFile(parentPath, payload, err)) return false;
    } else if (type != 1 && type != 2) {
        err = "unsupported VDI type";
        return false;
    }
    if (sectorSize != 512) {
        err = "VDI sector size must be 512";
        return false;
    }
    if (diskSize == 0 || (diskSize % 512) != 0 || diskSize > kMaxExpand) {
        err = "invalid VDI virtual size";
        return false;
    }
    if (blockSize < 512 || (blockSize & (blockSize - 1)) != 0 || blocks == 0 ||
        blocks > 65536) {
        err = "invalid VDI block map";
        return false;
    }
    const uint64_t bmapBytes = static_cast<uint64_t>(blocks) * 4;
    if (offBmap == 0 || static_cast<uint64_t>(offBmap) > n || bmapBytes > n - offBmap) {
        err = "VDI block map out of range";
        return false;
    }
    if (payload.empty()) payload.assign(static_cast<size_t>(diskSize), 0);
    if (payload.size() != diskSize) {
        err = "VDI parent virtual size mismatch";
        payload.clear();
        return false;
    }
    const uint8_t* bmap = image + offBmap;
    for (uint32_t i = 0; i < blocks; ++i) {
        const uint32_t idx = readLe32(bmap + static_cast<size_t>(i) * 4);
        if (idx == 0xFFFFFFFFu || idx == 0xFFFFFFFEu) continue;
        const uint64_t virtOff = static_cast<uint64_t>(i) * blockSize;
        if (virtOff >= diskSize) break;
        const uint64_t srcOff = static_cast<uint64_t>(offData) + static_cast<uint64_t>(idx) * blockSize;
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(blockSize, diskSize - virtOff));
        if (srcOff > n || chunk > n - srcOff) {
            err = "VDI data block out of range";
            payload.clear();
            return false;
        }
        std::memcpy(payload.data() + static_cast<size_t>(virtOff), image + static_cast<size_t>(srcOff),
                    chunk);
    }
    return true;
}

constexpr uint64_t kQcowOffMask = 0x00fffffffffffe00ULL;
constexpr uint64_t kQcowCompressed = 1ull << 62;

bool extractQcow2Payload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                         std::string& err, const std::string& imagePath) {
    payload.clear();
    if (!image || n < 72 || std::memcmp(image, "QFI\xfb", 4) != 0) {
        err = "not a QCOW2 header";
        return false;
    }
    const uint32_t version = readBe32(image + 4);
    if (version != 2 && version != 3) {
        err = "unsupported QCOW version";
        return false;
    }
    const uint64_t backOff = readBe64(image + 8);
    const uint32_t backLen = readBe32(image + 16);
    if (backOff != 0 || backLen != 0) {
        if (imagePath.empty()) {
            err = "QCOW2 backing file not supported";
            return false;
        }
        if (backOff == 0 || backLen == 0 || backLen > 255 || backOff > n ||
            backLen > n - backOff) {
            err = "QCOW2 backing file invalid";
            return false;
        }
        std::string hint(reinterpret_cast<const char*>(image + static_cast<size_t>(backOff)),
                         backLen);
        const auto z = hint.find('\0');
        if (z != std::string::npos) hint.resize(z);
        if (hint.empty() || hint.find('\\') != std::string::npos ||
            hint.find('/') != std::string::npos || hint.find(':') != std::string::npos) {
            err = "QCOW2 backing file name invalid";
            return false;
        }
        const std::string parentPath =
            pathToUtf8(utf8Path(imagePath).parent_path() / utf8Path(hint));
        if (parentPath.empty() || isHttpUrl(parentPath) || parentPath == imagePath) {
            err = "QCOW2 backing path invalid";
            return false;
        }
        if (!extractVhdFile(parentPath, payload, err)) return false;
    }
    const uint32_t clusterBits = readBe32(image + 20);
    const uint64_t virt = readBe64(image + 24);
    const uint32_t crypt = readBe32(image + 32);
    const uint32_t l1Size = readBe32(image + 36);
    const uint64_t l1Off = readBe64(image + 40);
    const uint32_t nSnap = readBe32(image + 60);
    if (crypt != 0) {
        err = "QCOW2 encryption not supported";
        return false;
    }
    if (nSnap != 0) {
        err = "QCOW2 snapshots not supported";
        return false;
    }
    if (clusterBits < 9 || clusterBits > 21) {
        err = "invalid QCOW2 cluster_bits";
        return false;
    }
    if (virt == 0 || (virt % 512) != 0 || virt > kMaxExpand) {
        err = "invalid QCOW2 virtual size";
        return false;
    }
    if (version >= 3 && n >= 80 && readBe64(image + 72) != 0) {
        err = "QCOW2 incompatible features not supported";
        return false;
    }
    const uint64_t cluster = 1ull << clusterBits;
    const uint64_t l2Entries = cluster / 8;
    const uint64_t l1Bytes = static_cast<uint64_t>(l1Size) * 8;
    if (l2Entries == 0 || l1Size == 0 || l1Size > 4096 || l1Off > n || l1Bytes > n - l1Off) {
        err = "invalid QCOW2 L1 table";
        return false;
    }
    if (payload.empty()) payload.assign(static_cast<size_t>(virt), 0);
    if (payload.size() != virt) {
        err = "QCOW2 backing virtual size mismatch";
        payload.clear();
        return false;
    }
    const uint64_t nClusters = (virt + cluster - 1) / cluster;
    for (uint64_t i = 0; i < nClusters; ++i) {
        const uint64_t l1Index = i / l2Entries;
        if (l1Index >= l1Size) break;
        const uint64_t l1e = readBe64(image + static_cast<size_t>(l1Off + l1Index * 8));
        if (l1e & kQcowCompressed) {
            err = "QCOW2 compressed cluster not supported";
            payload.clear();
            return false;
        }
        const uint64_t l2Off = l1e & kQcowOffMask;
        if (l2Off == 0) continue;
        if (l2Off > n || cluster > n - l2Off) {
            err = "QCOW2 L2 table out of range";
            payload.clear();
            return false;
        }
        const uint64_t l2Index = i % l2Entries;
        const uint64_t l2e = readBe64(image + static_cast<size_t>(l2Off + l2Index * 8));
        const uint64_t virtOff = i * cluster;
        const size_t chunk = static_cast<size_t>(std::min(cluster, virt - virtOff));
        if (l2e & kQcowCompressed) {
            const uint32_t cshift = 62u - (clusterBits - 8u);
            const uint64_t cmask = (1ull << (clusterBits - 8u)) - 1u;
            const uint64_t nb = ((l2e >> cshift) & cmask) + 1u;
            const uint64_t host = l2e & ((1ull << cshift) - 1u);
            const uint64_t nComp = nb * 512u;
            if (host == 0 || nb == 0 || host > n || nComp > n - host) {
                err = "QCOW2 compressed cluster out of range";
                payload.clear();
                return false;
            }
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -12) != Z_OK) {
                err = "QCOW2 compressed cluster inflate init failed";
                payload.clear();
                return false;
            }
            std::vector<uint8_t> plain(static_cast<size_t>(cluster), 0);
            zs.next_in = const_cast<Bytef*>(image + static_cast<size_t>(host));
            zs.avail_in = static_cast<uInt>(nComp);
            zs.next_out = plain.data();
            zs.avail_out = static_cast<uInt>(plain.size());
            const int rc = inflate(&zs, Z_FINISH);
            const size_t produced = static_cast<size_t>(zs.total_out);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END || produced < chunk) {
                err = "QCOW2 compressed cluster not supported";
                payload.clear();
                return false;
            }
            std::memcpy(payload.data() + static_cast<size_t>(virtOff), plain.data(), chunk);
            continue;
        }
        if (l2e & 1ull) continue;
        const uint64_t host = l2e & kQcowOffMask;
        if (host == 0) continue;
        if (host > n || chunk > n - host) {
            err = "QCOW2 data cluster out of range";
            payload.clear();
            return false;
        }
        std::memcpy(payload.data() + static_cast<size_t>(virtOff), image + static_cast<size_t>(host),
                    chunk);
    }
    return true;
}

} // namespace

std::vector<uint8_t> wrapFixedVhd(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = payload;
    out.resize(payload.size() + kVhdFooter, 0);
    writeFooter(out.data() + payload.size(), payload.size(), kVhdTypeFixed, ~uint64_t{0});
    return out;
}

std::vector<uint8_t> wrapDynamicVhd(const std::vector<uint8_t>& payload,
                                    const std::string& parentRelative) {
    const uint64_t virtualSize = payload.size();
    const uint32_t entries = virtualSize == 0
        ? 0
        : static_cast<uint32_t>((virtualSize + kDynBlockSize - 1) / kDynBlockSize);
    const uint32_t batBytes = std::max(512u, ((entries * 4u + 511u) / 512u) * 512u);
    const uint32_t bitmapBytes = 512;
    const uint32_t blockOnDisk = bitmapBytes + kDynBlockSize;
    const uint32_t firstDataSector = static_cast<uint32_t>((kVhdFooter + kDynHeader + batBytes) / 512);

    std::vector<uint32_t> bat(entries, 0xFFFFFFFFu);
    std::vector<uint8_t> dataBlobs;
    uint32_t nextSector = firstDataSector;
    for (uint32_t i = 0; i < entries; ++i) {
        const size_t off = static_cast<size_t>(i) * kDynBlockSize;
        const size_t n = std::min(static_cast<size_t>(kDynBlockSize), payload.size() - off);
        bool used = false;
        for (size_t j = 0; j < n; ++j) {
            if (payload[off + j] != 0) {
                used = true;
                break;
            }
        }
        if (!used) continue;
        bat[i] = nextSector;
        std::vector<uint8_t> blk(blockOnDisk, 0);
        std::memset(blk.data(), 0xFF, 16);
        std::memcpy(blk.data() + bitmapBytes, payload.data() + off, n);
        dataBlobs.insert(dataBlobs.end(), blk.begin(), blk.end());
        nextSector += blockOnDisk / 512;
    }

    std::vector<uint8_t> out(kVhdFooter + kDynHeader + batBytes, 0);
    uint8_t* h = out.data() + kVhdFooter;
    std::memcpy(h, "cxsparse", 8);
    std::memset(h + 8, 0xFF, 8);
    writeBe64(h + 16, kVhdFooter + kDynHeader);
    writeBe32(h + 24, 0x00010000);
    writeBe32(h + 28, entries);
    writeBe32(h + 32, kDynBlockSize);
    if (!parentRelative.empty()) {
        for (size_t i = 0; i < parentRelative.size() && i < 255; ++i) {
            h[64 + i * 2] = static_cast<uint8_t>(parentRelative[i]);
        }
    }
    writeBe32(h + 36, onesComplementSum(h, kDynHeader, 36, 4));

    uint8_t* batp = out.data() + kVhdFooter + kDynHeader;
    for (uint32_t i = 0; i < entries; ++i) writeBe32(batp + i * 4, bat[i]);
    out.insert(out.end(), dataBlobs.begin(), dataBlobs.end());

    std::vector<uint8_t> footer(kVhdFooter, 0);
    writeFooter(footer.data(), virtualSize,
                parentRelative.empty() ? kVhdTypeDynamic : kVhdTypeDifferencing, kVhdFooter);
    std::memcpy(out.data(), footer.data(), kVhdFooter);
    out.insert(out.end(), footer.begin(), footer.end());
    return out;
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

std::vector<uint8_t> wrapVmdk(const std::vector<uint8_t>& payload, bool hasParent,
                              bool compressed, const std::string& parentRelative) {
    constexpr uint64_t kGrainSectors = 128;
    constexpr uint32_t kGtesPerGt = 512;
    constexpr uint64_t kDescSectors = 20;
    const uint64_t capacity = payload.size() / 512;
    const uint64_t numGrains = capacity == 0 ? 0 : (capacity + kGrainSectors - 1) / kGrainSectors;
    const uint64_t numGts = numGrains == 0 ? 1 : (numGrains + kGtesPerGt - 1) / kGtesPerGt;
    const uint64_t gdOff = 1 + kDescSectors;
    const uint64_t gtOff = gdOff + 1;
    const uint64_t grainOff = kGrainSectors; // 64 KiB aligned

    std::vector<uint32_t> gt(static_cast<size_t>(numGts * kGtesPerGt), 0);
    std::vector<uint8_t> grains;
    uint64_t nextGrain = grainOff;
    for (uint64_t i = 0; i < numGrains; ++i) {
        const size_t off = static_cast<size_t>(i * kGrainSectors * 512);
        const size_t n = std::min(static_cast<size_t>(kGrainSectors * 512), payload.size() - off);
        bool used = false;
        for (size_t j = 0; j < n; ++j) {
            if (payload[off + j] != 0) {
                used = true;
                break;
            }
        }
        if (!used) continue;
        std::vector<uint8_t> g(static_cast<size_t>(kGrainSectors * 512), 0);
        std::memcpy(g.data(), payload.data() + off, n);
        if (!compressed) {
            gt[static_cast<size_t>(i)] = static_cast<uint32_t>(nextGrain);
            grains.insert(grains.end(), g.begin(), g.end());
            nextGrain += kGrainSectors;
            continue;
        }
        uLong zlen = compressBound(static_cast<uLong>(g.size()));
        std::vector<uint8_t> z(static_cast<size_t>(zlen));
        if (compress(z.data(), &zlen, g.data(), static_cast<uLong>(g.size())) != Z_OK) continue;
        const size_t packed = 12 + static_cast<size_t>(zlen);
        const uint64_t sectors = (packed + 511) / 512;
        std::vector<uint8_t> blob(static_cast<size_t>(sectors * 512), 0);
        writeLe64(blob.data(), i * kGrainSectors);
        writeLe32(blob.data() + 8, static_cast<uint32_t>(zlen));
        std::memcpy(blob.data() + 12, z.data(), static_cast<size_t>(zlen));
        gt[static_cast<size_t>(i)] = static_cast<uint32_t>(nextGrain);
        grains.insert(grains.end(), blob.begin(), blob.end());
        nextGrain += sectors;
    }

    const size_t fileSectors = static_cast<size_t>(std::max(grainOff, nextGrain));
    std::vector<uint8_t> out(fileSectors * 512, 0);
    writeLe32(out.data(), 0x564d444b);
    writeLe32(out.data() + 4, 1);
    writeLe32(out.data() + 8, compressed ? (1u | (1u << 16)) : 1u);
    writeLe64(out.data() + 12, capacity);
    writeLe64(out.data() + 20, kGrainSectors);
    writeLe64(out.data() + 28, 1);
    writeLe64(out.data() + 36, kDescSectors);
    writeLe32(out.data() + 44, kGtesPerGt);
    writeLe64(out.data() + 56, gdOff);
    writeLe64(out.data() + 64, grainOff);
    out[73] = '\n';
    out[74] = ' ';
    out[75] = '\r';
    out[76] = '\n';
    if (compressed) writeLe16(out.data() + 77, 1);

    char desc[1024];
    if (!parentRelative.empty()) {
        std::snprintf(desc, sizeof(desc),
                      "# Disk DescriptorFile\n"
                      "version=1\n"
                      "CID=fffffffe\n"
                      "parentCID=%s\n"
                      "parentFileNameHint=\"%s\"\n"
                      "createType=\"monolithicSparse\"\n"
                      "RW %llu SPARSE \"byteback.vmdk\"\n",
                      hasParent ? "abcdef01" : "ffffffff",
                      parentRelative.c_str(),
                      static_cast<unsigned long long>(capacity));
    } else {
        std::snprintf(desc, sizeof(desc),
                      "# Disk DescriptorFile\n"
                      "version=1\n"
                      "CID=fffffffe\n"
                      "parentCID=%s\n"
                      "createType=\"monolithicSparse\"\n"
                      "RW %llu SPARSE \"byteback.vmdk\"\n",
                      hasParent ? "abcdef01" : "ffffffff",
                      static_cast<unsigned long long>(capacity));
    }
    std::memcpy(out.data() + 512, desc, std::strlen(desc));

    writeLe32(out.data() + static_cast<size_t>(gdOff * 512), static_cast<uint32_t>(gtOff));
    uint8_t* gtp = out.data() + static_cast<size_t>(gtOff * 512);
    for (size_t i = 0; i < gt.size(); ++i) writeLe32(gtp + i * 4, gt[i]);
    if (!grains.empty()) {
        std::memcpy(out.data() + static_cast<size_t>(grainOff * 512), grains.data(), grains.size());
    }
    return out;
}

std::vector<uint8_t> wrapVdi(const std::vector<uint8_t>& payload, bool differencing) {
    const uint32_t blockSize = static_cast<uint32_t>(k1MiB);
    const uint32_t blocks = payload.empty()
        ? 0
        : static_cast<uint32_t>((payload.size() + blockSize - 1) / blockSize);
    const uint32_t offBmap = 512;
    const uint32_t bmapBytes = std::max(512u, ((blocks * 4u + 511u) / 512u) * 512u);
    const uint32_t offData = offBmap + bmapBytes;

    std::vector<uint32_t> bmap(blocks, 0xFFFFFFFEu);
    std::vector<uint8_t> dataBlobs;
    uint32_t allocated = 0;
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
        bmap[i] = allocated++;
        std::vector<uint8_t> blk(blockSize, 0);
        std::memcpy(blk.data(), payload.data() + off, n);
        dataBlobs.insert(dataBlobs.end(), blk.begin(), blk.end());
    }

    std::vector<uint8_t> out(static_cast<size_t>(offData) + dataBlobs.size(), 0);
    const char* info = "<<< Oracle VM VirtualBox Disk Image >>>\n";
    std::memcpy(out.data(), info, std::strlen(info));
    writeLe32(out.data() + 64, 0xBEDA107Fu);
    writeLe32(out.data() + 68, 0x00010001u);
    writeLe32(out.data() + 72, 400);
    writeLe32(out.data() + 76, differencing ? 4u : 1u);
    writeLe32(out.data() + 340, offBmap);
    writeLe32(out.data() + 344, offData);
    writeLe32(out.data() + 360, 512);
    writeLe64(out.data() + 368, payload.size());
    writeLe32(out.data() + 376, blockSize);
    writeLe32(out.data() + 384, blocks);
    writeLe32(out.data() + 388, allocated);
    std::memset(out.data() + 392, differencing ? 0xCE : 0xCD, 16);
    if (differencing) std::memset(out.data() + 408, 0xCD, 16);
    for (uint32_t i = 0; i < blocks; ++i) writeLe32(out.data() + offBmap + i * 4, bmap[i]);
    if (!dataBlobs.empty()) {
        std::memcpy(out.data() + offData, dataBlobs.data(), dataBlobs.size());
    }
    return out;
}

std::vector<uint8_t> wrapQcow2(const std::vector<uint8_t>& payload, bool hasBacking,
                               bool compressed, const std::string& backingRelative) {
    constexpr uint32_t kClusterBits = 16;
    constexpr uint32_t kCluster = 1u << kClusterBits;
    constexpr uint64_t kCopied = 1ull << 63;
    constexpr uint32_t kCsizeShift = 62 - (kClusterBits - 8);
    const uint64_t virt = payload.size();
    const uint32_t nClusters =
        virt == 0 ? 0 : static_cast<uint32_t>((virt + kCluster - 1) / kCluster);
    const uint64_t l1Off = kCluster;
    const uint64_t l2Off = 2ull * kCluster;
    const uint64_t dataOff = 3ull * kCluster;

    if (!compressed) {
        const bool namedBack = !backingRelative.empty();
        uint32_t used = 0;
        if (namedBack) {
            for (uint32_t i = 0; i < nClusters; ++i) {
                const size_t src = static_cast<size_t>(i) * kCluster;
                if (src >= payload.size()) break;
                const size_t n = std::min(payload.size() - src, static_cast<size_t>(kCluster));
                bool nz = false;
                for (size_t j = 0; j < n; ++j) {
                    if (payload[src + j] != 0) {
                        nz = true;
                        break;
                    }
                }
                if (nz) ++used;
            }
        } else {
            used = nClusters;
        }
        const uint64_t outSize = dataOff + static_cast<uint64_t>(used) * kCluster;
        std::vector<uint8_t> out(static_cast<size_t>(outSize), 0);
        out[0] = 'Q';
        out[1] = 'F';
        out[2] = 'I';
        out[3] = 0xFB;
        writeBe32(out.data() + 4, 2);
        if (namedBack) {
            writeBe64(out.data() + 8, 72);
            writeBe32(out.data() + 16, static_cast<uint32_t>(backingRelative.size()));
            std::memcpy(out.data() + 72, backingRelative.data(), backingRelative.size());
        } else if (hasBacking) {
            writeBe64(out.data() + 8, 1);
            writeBe32(out.data() + 16, 1);
        }
        writeBe32(out.data() + 20, kClusterBits);
        writeBe64(out.data() + 24, virt);
        writeBe32(out.data() + 36, 1);
        writeBe64(out.data() + 40, l1Off);
        writeBe64(out.data() + l1Off, l2Off | kCopied);
        uint32_t next = 0;
        for (uint32_t i = 0; i < nClusters; ++i) {
            const size_t src = static_cast<size_t>(i) * kCluster;
            if (src >= payload.size()) break;
            const size_t n = std::min(payload.size() - src, static_cast<size_t>(kCluster));
            if (namedBack) {
                bool nz = false;
                for (size_t j = 0; j < n; ++j) {
                    if (payload[src + j] != 0) {
                        nz = true;
                        break;
                    }
                }
                if (!nz) continue;
            }
            const uint64_t host = dataOff + static_cast<uint64_t>(next++) * kCluster;
            writeBe64(out.data() + l2Off + static_cast<size_t>(i) * 8, host | kCopied);
            std::memcpy(out.data() + static_cast<size_t>(host), payload.data() + src, n);
        }
        return out;
    }

    std::vector<std::vector<uint8_t>> blobs(nClusters);
    size_t packed = 0;
    for (uint32_t i = 0; i < nClusters; ++i) {
        const size_t src = static_cast<size_t>(i) * kCluster;
        if (src >= payload.size()) break;
        const size_t n = std::min(payload.size() - src, static_cast<size_t>(kCluster));
        std::vector<uint8_t> cluster(kCluster, 0);
        std::memcpy(cluster.data(), payload.data() + src, n);
        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -12, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return {};
        std::vector<uint8_t> blob(compressBound(static_cast<uLong>(kCluster)));
        zs.next_in = cluster.data();
        zs.avail_in = static_cast<uInt>(kCluster);
        zs.next_out = blob.data();
        zs.avail_out = static_cast<uInt>(blob.size());
        const int rc = deflate(&zs, Z_FINISH);
        const size_t produced = blob.size() - zs.avail_out;
        deflateEnd(&zs);
        if (rc != Z_STREAM_END || produced == 0) return {};
        blob.resize(produced);
        const uint32_t nb = static_cast<uint32_t>((produced + 511) / 512);
        if (nb == 0 || nb > 256) return {};
        blob.resize(static_cast<size_t>(nb) * 512, 0);
        packed += blob.size();
        blobs[i] = std::move(blob);
    }
    std::vector<uint8_t> out(static_cast<size_t>(dataOff + packed), 0);
    out[0] = 'Q';
    out[1] = 'F';
    out[2] = 'I';
    out[3] = 0xFB;
    writeBe32(out.data() + 4, 2);
    if (hasBacking) {
        writeBe64(out.data() + 8, 1);
        writeBe32(out.data() + 16, 1);
    }
    writeBe32(out.data() + 20, kClusterBits);
    writeBe64(out.data() + 24, virt);
    writeBe32(out.data() + 36, 1);
    writeBe64(out.data() + 40, l1Off);
    writeBe64(out.data() + l1Off, l2Off | kCopied);
    uint64_t host = dataOff;
    for (uint32_t i = 0; i < nClusters; ++i) {
        if (blobs[i].empty()) continue;
        const uint32_t nb = static_cast<uint32_t>(blobs[i].size() / 512);
        const uint64_t desc = host | (static_cast<uint64_t>(nb - 1) << kCsizeShift) | (1ull << 62);
        writeBe64(out.data() + l2Off + static_cast<size_t>(i) * 8, desc);
        std::memcpy(out.data() + static_cast<size_t>(host), blobs[i].data(), blobs[i].size());
        host += blobs[i].size();
    }
    return out;
}

constexpr uint32_t kDmgKoly = 512;
constexpr uint32_t kDmgMishHdr = 204;
constexpr uint32_t kDmgRun = 40;
constexpr uint32_t kDmgRaw = 1;
constexpr uint32_t kDmgZero = 2;
constexpr uint32_t kDmgAdc = 0x80000004u;
constexpr uint32_t kDmgZlib = 0x80000005u;
constexpr uint32_t kDmgTerm = 0xFFFFFFFFu;

std::string dmgB64Enc(const uint8_t* p, size_t n) {
    static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(p[i]) << 16) |
                           (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? static_cast<uint32_t>(p[i + 2]) : 0);
        o.push_back(kTab[(v >> 18) & 63]);
        o.push_back(kTab[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? kTab[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? kTab[v & 63] : '=');
    }
    return o;
}

int dmgB64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool dmgB64Dec(const std::string& s, std::vector<uint8_t>& out) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        t.push_back(c);
    }
    if (t.empty() || (t.size() % 4) != 0) return false;
    out.clear();
    out.reserve((t.size() / 4) * 3);
    for (size_t i = 0; i < t.size(); i += 4) {
        const int a = dmgB64Val(t[i]);
        const int b = dmgB64Val(t[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (t[i + 2] != '=') {
            const int c = dmgB64Val(t[i + 2]);
            if (c < 0) return false;
            out.push_back(static_cast<uint8_t>(((b & 15) << 4) | (c >> 2)));
            if (t[i + 3] != '=') {
                const int d = dmgB64Val(t[i + 3]);
                if (d < 0) return false;
                out.push_back(static_cast<uint8_t>(((c & 3) << 6) | d));
            }
        }
    }
    return true;
}

bool dmgZlibDeflate(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return false;
    const uLong bound = compressBound(static_cast<uLong>(inLen));
    out.resize(bound);
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(inLen);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const size_t n = out.size() - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;
    out.resize(n);
    return true;
}

bool dmgZlibInflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap, size_t* produced) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(inLen);
    zs.next_out = out;
    zs.avail_out = static_cast<uInt>(outCap);
    const int rc = inflate(&zs, Z_FINISH);
    *produced = zs.total_out;
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

void adcCompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    while (i < inLen) {
        if (i >= 1) {
            size_t run = 0;
            while (i + run < inLen && in[i + run] == in[i - 1] && run < 34) ++run;
            if (run >= 3) {
                out.push_back(static_cast<uint8_t>(0x80 | ((run - 3) << 2)));
                out.push_back(0);
                i += run;
                continue;
            }
        }
        size_t n = std::min(size_t{64}, inLen - i);
        size_t lit = 1;
        while (lit < n) {
            if (lit >= 3 && in[i + lit] == in[i + lit - 1] &&
                in[i + lit - 1] == in[i + lit - 2]) {
                lit -= 2;
                break;
            }
            ++lit;
        }
        out.push_back(static_cast<uint8_t>(lit - 1));
        out.insert(out.end(), in + i, in + i + lit);
        i += lit;
    }
}

bool adcDecompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t* produced) {
    *produced = 0;
    if (!src || !dst) return false;
    size_t i = 0, o = 0;
    while (i < srcLen) {
        const uint8_t c = src[i++];
        size_t len = 0, off = 0;
        if (c & 0x80) {
            if (i >= srcLen) return false;
            len = static_cast<size_t>(((c >> 2) & 0x1F) + 3);
            off = static_cast<size_t>(((c & 3) << 8) | src[i]) + 1;
            ++i;
        } else if (c & 0x40) {
            if (i + 1 >= srcLen) return false;
            len = static_cast<size_t>((c & 0x3F) + 4);
            off = (static_cast<size_t>(src[i]) << 8) + src[i + 1] + 1;
            i += 2;
        } else {
            len = static_cast<size_t>(c) + 1;
            if (i + len > srcLen || o + len > dstCap) return false;
            std::memcpy(dst + o, src + i, len);
            i += len;
            o += len;
            continue;
        }
        if (off == 0 || off > o || o + len > dstCap) return false;
        for (size_t k = 0; k < len; ++k) {
            dst[o] = dst[o - off];
            ++o;
        }
    }
    *produced = o;
    return o > 0;
}

bool extractDmgPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload, std::string& err) {
    payload.clear();
    if (!image || n < kDmgKoly) {
        err = "DMG too small for koly footer";
        return false;
    }
    const uint8_t* k = image + (n - kDmgKoly);
    if (std::memcmp(k, "koly", 4) != 0) {
        err = "not a UDIF koly footer";
        return false;
    }
    if (readBe32(k + 4) != 4 || readBe32(k + 8) != kDmgKoly) {
        err = "unsupported UDIF koly version";
        return false;
    }
    const uint64_t dataOff = readBe64(k + 24);
    const uint64_t dataLen = readBe64(k + 32);
    const uint64_t xmlOff = readBe64(k + 216);
    const uint64_t xmlLen = readBe64(k + 224);
    const uint64_t sectors = readBe64(k + 492);
    if (sectors == 0 || sectors > (kMaxExpand / 512)) {
        err = "UDIF sector count exceeds 64 MiB cap";
        return false;
    }
    if (dataOff > n - kDmgKoly || dataLen > n - kDmgKoly - dataOff) {
        err = "UDIF data fork out of range";
        return false;
    }
    if (xmlOff > n - kDmgKoly || xmlLen == 0 || xmlLen > n - kDmgKoly - xmlOff) {
        err = "UDIF XML out of range";
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(image + xmlOff), static_cast<size_t>(xmlLen));
    if (xml.find("CEncryptedEncoding") != std::string::npos ||
        xml.find("encrypted-encoding") != std::string::npos) {
        err = "encrypted UDIF not supported";
        return false;
    }
    const auto blkx = xml.find("<key>blkx</key>");
    if (blkx == std::string::npos) {
        err = "UDIF XML missing blkx";
        return false;
    }
    const auto arrayEnd = xml.find("</array>", blkx);
    if (arrayEnd == std::string::npos) {
        err = "UDIF XML missing blkx array";
        return false;
    }
    payload.assign(static_cast<size_t>(sectors * 512), 0);
    size_t search = blkx;
    bool any = false;
    while (search < arrayEnd) {
        const auto ds = xml.find("<data>", search);
        if (ds == std::string::npos || ds >= arrayEnd) break;
        const auto de = xml.find("</data>", ds);
        if (de == std::string::npos || de >= arrayEnd) {
            err = "UDIF XML truncated data";
            payload.clear();
            return false;
        }
        search = de + 7;
        std::vector<uint8_t> mish;
        if (!dmgB64Dec(xml.substr(ds + 6, de - (ds + 6)), mish) || mish.size() < kDmgMishHdr + kDmgRun) {
            err = "UDIF mish decode failed";
            payload.clear();
            return false;
        }
        if (std::memcmp(mish.data(), "mish", 4) != 0) continue;
        const uint32_t nRuns = readBe32(mish.data() + 200);
        if (nRuns == 0 || nRuns > 4096) {
            err = "UDIF mish run count invalid";
            payload.clear();
            return false;
        }
        const uint64_t chunkStart = readBe64(mish.data() + 8);
        if (kDmgMishHdr + static_cast<uint64_t>(nRuns) * kDmgRun > mish.size()) {
            err = "UDIF mish truncated";
            payload.clear();
            return false;
        }
        for (uint32_t i = 0; i < nRuns; ++i) {
            const uint8_t* run = mish.data() + kDmgMishHdr + i * kDmgRun;
            const uint32_t type = readBe32(run);
            if (type == kDmgTerm) break;
            const uint64_t sec0 = readBe64(run + 8);
            const uint64_t nsec = readBe64(run + 16);
            const uint64_t cOff = readBe64(run + 24);
            const uint64_t cLen = readBe64(run + 32);
            if (nsec == 0) continue;
            if (nsec > sectors || chunkStart > sectors || sec0 > sectors - chunkStart) {
                err = "UDIF run sector out of range";
                payload.clear();
                return false;
            }
            const uint64_t destOff = (chunkStart + sec0) * 512;
            const uint64_t destLen = nsec * 512;
            if (destOff + destLen > payload.size()) {
                err = "UDIF run exceeds volume";
                payload.clear();
                return false;
            }
            if (type == kDmgZero) continue;
            if (type != kDmgRaw && type != kDmgZlib && type != kDmgAdc) {
                err = "UDIF compression not supported";
                payload.clear();
                return false;
            }
            if (cOff > dataLen || cLen > dataLen - cOff) {
                err = "UDIF run data fork out of range";
                payload.clear();
                return false;
            }
            const uint8_t* src = image + dataOff + cOff;
            if (type == kDmgRaw) {
                const size_t ncopy = static_cast<size_t>(std::min(cLen, destLen));
                std::memcpy(payload.data() + static_cast<size_t>(destOff), src, ncopy);
            } else if (type == kDmgAdc) {
                size_t produced = 0;
                if (!adcDecompress(src, static_cast<size_t>(cLen), payload.data() + static_cast<size_t>(destOff),
                                   static_cast<size_t>(destLen), &produced) ||
                    produced == 0) {
                    err = "UDIF ADC decompress failed";
                    payload.clear();
                    return false;
                }
            } else {
                size_t produced = 0;
                if (!dmgZlibInflate(src, static_cast<size_t>(cLen), payload.data() + static_cast<size_t>(destOff),
                                    static_cast<size_t>(destLen), &produced) ||
                    produced == 0) {
                    err = "UDIF zlib inflate failed";
                    payload.clear();
                    return false;
                }
            }
            any = true;
        }
    }
    if (!any) {
        err = "UDIF had no usable blkx runs";
        payload.clear();
        return false;
    }
    return true;
}

std::vector<uint8_t> wrapDmg(const std::vector<uint8_t>& payload, uint32_t blockType, bool encrypted) {
    const uint64_t sectors = payload.empty() ? 0 : (payload.size() + 511) / 512;
    std::vector<uint8_t> data;
    if (blockType == kDmgZlib) {
        if (!dmgZlibDeflate(payload.data(), payload.size(), data)) return {};
    } else if (blockType == kDmgAdc) {
        adcCompress(payload.data(), payload.size(), data);
    } else {
        data = payload;
    }
    std::vector<uint8_t> mish(kDmgMishHdr + 2 * kDmgRun, 0);
    std::memcpy(mish.data(), "mish", 4);
    writeBe32(mish.data() + 4, 1);
    writeBe64(mish.data() + 16, sectors);
    writeBe32(mish.data() + 200, 2);
    writeBe32(mish.data() + kDmgMishHdr, blockType);
    writeBe64(mish.data() + kDmgMishHdr + 16, sectors);
    writeBe64(mish.data() + kDmgMishHdr + 32, data.size());
    writeBe32(mish.data() + kDmgMishHdr + kDmgRun, kDmgTerm);
    std::string b64 = dmgB64Enc(mish.data(), mish.size());
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict><key>resource-fork</key><dict>";
    if (encrypted) xml += "<key>encrypted-encoding</key><dict><key>EncodingName</key>"
                          "<string>CEncryptedEncoding</string></dict>";
    xml += "<key>blkx</key><array><dict><key>Data</key><data>";
    xml += b64;
    xml += "</data></dict></array></dict></dict></plist>\n";
    std::vector<uint8_t> out(data.size() + xml.size() + kDmgKoly, 0);
    if (!data.empty()) std::memcpy(out.data(), data.data(), data.size());
    std::memcpy(out.data() + data.size(), xml.data(), xml.size());
    uint8_t* koly = out.data() + data.size() + xml.size();
    std::memcpy(koly, "koly", 4);
    writeBe32(koly + 4, 4);
    writeBe32(koly + 8, kDmgKoly);
    writeBe64(koly + 32, data.size());
    writeBe32(koly + 56, 1);
    writeBe32(koly + 60, 1);
    writeBe64(koly + 216, data.size());
    writeBe64(koly + 224, xml.size());
    writeBe32(koly + 488, 1);
    writeBe64(koly + 492, sectors);
    return out;
}

bool extractVhdPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                       std::string& err, const std::string& imagePath) {
    if (image && n >= 4 && std::memcmp(image, "QFI\xfb", 4) == 0) {
        return extractQcow2Payload(image, n, payload, err, imagePath);
    }
    if (image && n >= kDmgKoly && std::memcmp(image + (n - kDmgKoly), "koly", 4) == 0) {
        return extractDmgPayload(image, n, payload, err);
    }
    if (image && n >= 8 && std::memcmp(image, "vhdxfile", 8) == 0) {
        return extractVhdxPayload(image, n, payload, err, imagePath);
    }
    if (image && n >= 4 && std::memcmp(image, "KDMV", 4) == 0) {
        return extractVmdkPayload(image, n, payload, err, imagePath);
    }
    if (image && n >= 68 && readLe32(image + 64) == 0xBEDA107Fu) {
        return extractVdiPayload(image, n, payload, err, imagePath);
    }
    payload.clear();
    if (!image || n < kVhdFooter) {
        err = "VHD too small for footer";
        return false;
    }
    const uint8_t* f = image + (n - kVhdFooter);
    if (std::memcmp(f, "conectix", 8) != 0) {
        err = "not a Connectix VHD footer";
        return false;
    }
    if (readBe32(f + 0x40) != onesComplementSum(f, kVhdFooter, 0x40, 4)) {
        err = "VHD footer checksum mismatch";
        return false;
    }
    const uint32_t diskType = readBe32(f + 0x3C);
    const uint64_t virtualSize = readBe64(f + 0x30);
    if (virtualSize == 0 || (virtualSize % 512) != 0) {
        err = "invalid VHD virtual size";
        return false;
    }
    if (diskType == kVhdTypeDifferencing && imagePath.empty()) {
        err = "unsupported VHD type";
        return false;
    }
    if (diskType == kVhdTypeFixed) {
        if (virtualSize > n - kVhdFooter) {
            err = "invalid VHD virtual size";
            return false;
        }
        payload.assign(image, image + static_cast<size_t>(virtualSize));
        return true;
    }
    if (diskType != kVhdTypeDynamic && diskType != kVhdTypeDifferencing) {
        err = "unsupported VHD type";
        return false;
    }
    // ponytail: expand BAT into RAM; sparse ByteSource if virtual disks grow past 64 MiB.
    if (virtualSize > kMaxExpand) {
        err = "dynamic VHD virtual size exceeds 64 MiB cap";
        return false;
    }
    const uint64_t dataOffset = readBe64(f + 0x10);
    if (dataOffset > n - kDynHeader) {
        err = "dynamic VHD header out of range";
        return false;
    }
    const uint8_t* h = image + static_cast<size_t>(dataOffset);
    if (std::memcmp(h, "cxsparse", 8) != 0) {
        err = "not a dynamic VHD header";
        return false;
    }
    if (readBe32(h + 36) != onesComplementSum(h, kDynHeader, 36, 4)) {
        err = "dynamic VHD header checksum mismatch";
        return false;
    }
    if (diskType == kVhdTypeDifferencing) {
        std::string hint;
        for (size_t i = 0; i < 255; ++i) {
            const uint8_t lo = h[64 + i * 2];
            const uint8_t hi = h[64 + i * 2 + 1];
            if (lo == 0 && hi == 0) break;
            if (hi != 0) {
                err = "VHD parent name invalid";
                return false;
            }
            hint.push_back(static_cast<char>(lo));
        }
        if (hint.empty() || hint.find('\\') != std::string::npos ||
            hint.find('/') != std::string::npos || hint.find(':') != std::string::npos) {
            err = "VHD parent name invalid";
            return false;
        }
        const std::string parentPath =
            pathToUtf8(utf8Path(imagePath).parent_path() / utf8Path(hint));
        if (parentPath.empty() || isHttpUrl(parentPath) || parentPath == imagePath) {
            err = "VHD parent path invalid";
            return false;
        }
        if (!extractVhdFile(parentPath, payload, err)) return false;
    }
    const uint64_t tableOffset = readBe64(h + 16);
    const uint32_t entries = readBe32(h + 28);
    const uint32_t blockSize = readBe32(h + 32);
    if (blockSize < 512 || blockSize > (2u << 20) || (blockSize & (blockSize - 1)) != 0) {
        err = "invalid dynamic VHD block size";
        return false;
    }
    if (entries == 0 || entries > 65536) {
        err = "invalid dynamic VHD BAT size";
        return false;
    }
    const uint64_t batBytes = static_cast<uint64_t>(entries) * 4;
    if (tableOffset > n || batBytes > n - tableOffset) {
        err = "dynamic VHD BAT out of range";
        return false;
    }
    uint32_t bitmapBytes = ((blockSize / 512) + 7) / 8;
    bitmapBytes = (bitmapBytes + 511) / 512 * 512;
    if (payload.empty()) payload.assign(static_cast<size_t>(virtualSize), 0);
    if (payload.size() != virtualSize) {
        err = "VHD parent virtual size mismatch";
        payload.clear();
        return false;
    }
    const uint8_t* bat = image + static_cast<size_t>(tableOffset);
    for (uint32_t i = 0; i < entries; ++i) {
        const uint32_t sector = readBe32(bat + i * 4);
        if (sector == 0xFFFFFFFFu) continue;
        const uint64_t virtOff = static_cast<uint64_t>(i) * blockSize;
        if (virtOff >= virtualSize) break;
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(blockSize, virtualSize - virtOff));
        const uint64_t dataOff = static_cast<uint64_t>(sector) * 512 + bitmapBytes;
        if (dataOff > n || chunk > n - dataOff) {
            err = "dynamic VHD block out of range";
            payload.clear();
            return false;
        }
        std::memcpy(payload.data() + static_cast<size_t>(virtOff), image + static_cast<size_t>(dataOff),
                    chunk);
    }
    return true;
}

bool extractVhdFile(const std::string& path, std::vector<uint8_t>& payload, std::string& err) {
    payload.clear();
    std::ifstream in(utf8Path(path), std::ios::binary | std::ios::ate);
    if (!in) {
        err = "could not open VHD/VHDX/VMDK/VDI/QCOW2 file";
        return false;
    }
    const std::streamoff sz = in.tellg();
    if (sz <= 0 || sz > static_cast<std::streamoff>(64ull << 20)) {
        err = "virtual disk file exceeds 64 MiB cap";
        return false;
    }
    const size_t n = static_cast<size_t>(sz);
    in.seekg(0);
    std::vector<uint8_t> image(n);
    in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(n));
    if (static_cast<size_t>(in.gcount()) != n) {
        err = "could not read virtual disk file";
        return false;
    }
    return extractVhdPayload(image.data(), image.size(), payload, err, path);
}

} // namespace byteback
