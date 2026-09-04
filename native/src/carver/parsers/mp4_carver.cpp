#include "carver/structural_parsers.h"
#include <algorithm>
#include <cstring>

namespace byteback {
namespace carver {

namespace {

uint32_t readBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t readBe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | uint64_t(p[i]);
    return v;
}

struct AtomWalk {
    uint64_t endOffset = 0;
    bool hasFtyp = false;
    bool hasMoov = false;
    bool hasMdat = false;
    uint64_t mdatEnd = 0;
};

bool walkAtoms(const uint8_t* data, size_t size, AtomWalk& out) {
    size_t i = 0;
    while (i + 8 <= size) {
        uint64_t atomSize = readBe32(data + i);
        const char* type = reinterpret_cast<const char*>(data + i + 4);
        uint64_t headerSize = 8;
        if (atomSize == 1) {
            if (i + 16 > size) break;
            atomSize = readBe64(data + i + 8);
            headerSize = 16;
        } else if (atomSize == 0) {
            atomSize = size - i;
        }
        if (atomSize < headerSize) break;
        uint64_t atomEnd = i + atomSize;
        if (atomEnd > size) atomEnd = size;

        if (std::memcmp(type, "ftyp", 4) == 0) out.hasFtyp = true;
        if (std::memcmp(type, "moov", 4) == 0) out.hasMoov = true;
        if (std::memcmp(type, "mdat", 4) == 0) {
            out.hasMdat = true;
            out.mdatEnd = atomEnd;
        }
        out.endOffset = std::max(out.endOffset, atomEnd);

        if (atomSize == 0) break;
        i += static_cast<size_t>(atomSize);
    }
    return out.hasFtyp;
}

bool findTailMoov(const uint8_t* data, size_t size) {
    if (size < 16) return false;
    size_t scanStart = size > 65536 ? size - 65536 : 0;
    for (size_t i = size - 8; i >= scanStart; --i) {
        if (std::memcmp(data + i + 4, "moov", 4) == 0 && i >= 4) {
            uint32_t atomSize = readBe32(data + i);
            if (atomSize >= 8 && i + atomSize <= size) return true;
        }
        if (i == 0) break;
    }
    return false;
}

} // namespace

StructuralParseResult parseMp4Mov(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (size < 12) return r;

    AtomWalk walk;
    if (!walkAtoms(data, size, walk)) return r;

    r.valid = true;
    r.extension = "mp4";
    r.confidence = 55;
    if (walk.hasMoov) r.confidence = 80;
    else if (findTailMoov(data, size)) r.confidence = 70;
    if (walk.hasMdat) r.confidence = std::max(r.confidence, 75);

    if (walk.hasMdat && walk.mdatEnd > 0) {
        r.size = walk.mdatEnd;
    } else {
        r.size = walk.endOffset;
    }
    if (r.size == 0) r.size = size;

    if (walk.hasMoov && walk.hasMdat && r.size <= size) r.confidence = 90;
    return r;
}

// CA-018: box walk with targeted reads. The old probe-only walk clamped mdat
// to the 1MB window, so a 2GB movie carved as "1MB valid" (moov in probe) or
// not at all (moov beyond probe). Only box headers are fetched here; the
// payload is never read.
StructuralParseResult parseIsobmffBounded(const uint8_t* probe, size_t probeSize,
                                          uint64_t probeAbsOffset, uint64_t maxBytes,
                                          const BoxHeaderReader& readAt) {
    StructuralParseResult r;
    if (!probe || probeSize < 8) return r;

    bool hasFtyp = false, hasMoov = false, hasMdat = false;
    uint64_t walkEnd = 0; // bytes from carve start
    uint64_t off = 0;
    const uint64_t limit = maxBytes;
    // W2: crafted 8-byte-box chains must not turn the walk into millions of
    // sector reads. Real files have well under 8k top-level boxes.
    uint64_t fetchBudget = 8192;
    uint8_t hdr[16];

    while (off + 8 <= limit) {
        const uint64_t abs = probeAbsOffset + off;
        const size_t inProbe = (off + 16 <= probeSize) ? 16 : (off + 8 <= probeSize ? 8 : 0);
        if (inProbe > 0) {
            std::memcpy(hdr, probe + off, inProbe);
        } else if (!readAt || fetchBudget == 0) {
            break;
        } else {
            --fetchBudget;
            if (!readAt(abs, 8, hdr)) break;
        }

        uint64_t atomSize = readBe32(hdr);
        const char* type = reinterpret_cast<const char*>(hdr + 4);
        const bool isMdat = std::memcmp(type, "mdat", 4) == 0;
        uint64_t headerSize = 8;
        if (atomSize == 1) {
            if (inProbe >= 16) {
                std::memcpy(hdr, probe + off, 16);
            } else if (!readAt || fetchBudget == 0) {
                break;
            } else {
                --fetchBudget;
                if (!readAt(abs, 16, hdr)) break;
            }
            atomSize = readBe64(hdr + 8);
            headerSize = 16;
        }
        if (atomSize == 0) {
            // Size 0 = "extends to end of file" — legal in unfinalized
            // recordings (power-loss dashcam/phone clips: mdat size 0). Only
            // a KNOWN payload type extends the file to the bound; a zero size
            // on an unknown type is slack after a complete file (zeros), and
            // must keep the walk end at the last complete box.
            if (isMdat || std::memcmp(type, "moov", 4) == 0) {
                walkEnd = limit;
                off = limit;
            }
            break;
        }
        if (atomSize < headerSize) break;

        if (std::memcmp(type, "ftyp", 4) == 0) hasFtyp = true;
        else if (std::memcmp(type, "moov", 4) == 0) hasMoov = true;
        else if (isMdat) hasMdat = true;

        if (atomSize > limit - off) {
            // Truncated at the carve bound: the movie payload itself ending
            // here still bounds the file; anything else stops at the last
            // complete box.
            if (isMdat) {
                walkEnd = limit;
                off = limit;
            }
            break;
        }
        off += atomSize;
        walkEnd = off;
    }

    if (!hasFtyp) return r;
    r.valid = true;
    r.extension = "mp4";
    r.confidence = 65;
    if (hasMdat) r.confidence = 80;
    if (hasMoov) r.confidence = 90;
    r.size = walkEnd > 0 ? walkEnd : off;
    return r;
}

} // namespace carver
} // namespace byteback
