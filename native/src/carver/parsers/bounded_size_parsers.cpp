// Size-bounding structural parsers for footer-less formats.
//
// CA-001 policy: a footer-less carve candidate may only be emitted when one
// of these parsers derives a real file end from its own structure. Anything
// unbounded would otherwise be emitted at the signature's maxSize — the
// phantom-record generator that flooded results.
#include "carver/structural_parsers.h"
#include <algorithm>
#include <cstring>

namespace byteback {
namespace carver {

namespace {
uint16_t rd16(const uint8_t* p, bool le) {
    return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
              : static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t rd32(const uint8_t* p, bool le) {
    if (le) return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint64_t rd64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
}

// TIFF: walk the IFD chain; the max StripOffsets[i]+StripByteCounts[i] is the
// true end for non-fragmented camera files (the dominant recovery case).
StructuralParseResult parseTiff(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (!data || size < 8) return r;
    const bool le = data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00;
    const bool be = data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A;
    if (!le && !be) return r;

    uint64_t end = 0;
    uint32_t ifd = rd32(data + 4, le);
    int chain = 0;
    bool sawStrips = false;
    while (ifd != 0 && ifd + 2 <= size && chain < 8) {
        const uint16_t count = rd16(data + ifd, le);
        if (count == 0 || count > 4096 || ifd + 2 + 12ull * count + 4 > size) return r;
        end = std::max(end, static_cast<uint64_t>(ifd) + 2 + 12ull * count + 4);
        const uint8_t* ent = data + ifd + 2;
        for (uint16_t i = 0; i < count; ++i, ent += 12) {
            const uint16_t tag = rd16(ent, le);
            const uint16_t type = rd16(ent + 2, le);
            const uint32_t cnt = rd32(ent + 4, le);
            if (type != 3 && type != 4) continue; // SHORT / LONG only
            const size_t vsz = (type == 3) ? 2u : 4u;
            const uint64_t total = 4ull * cnt;
            if (total > size) return r;
            const uint8_t* val = (total <= 4) ? ent + 8 : data + rd32(ent + 8, le);
            if (val + total > data + size) return r;
            auto valueAt = [&](uint32_t k) -> uint64_t {
                return (vsz == 2) ? rd16(val + 2 * k, le) : rd32(val + 4 * k, le);
            };
            if ((tag == 273 || tag == 324) && cnt > 0) { // StripOffsets / TileOffsets
                sawStrips = true;
                // Pair with StripByteCounts/TileByteCounts by index when present.
                uint64_t counts[4096];
                uint32_t ncounts = 0;
                for (uint16_t j = 0; j < count; ++j) {
                    const uint8_t* e2 = data + ifd + 2 + 12ull * j;
                    const uint16_t t2 = rd16(e2, le);
                    if (t2 != 279 && t2 != 325) continue;
                    const uint16_t ty2 = rd16(e2 + 2, le);
                    if (ty2 != 3 && ty2 != 4) continue;
                    const uint32_t c2 = rd32(e2 + 4, le);
                    if (c2 > 4096) continue;
                    const size_t vsz2 = (ty2 == 3) ? 2u : 4u;
                    const uint8_t* v2 = (4ull * c2 <= 4) ? e2 + 8 : data + rd32(e2 + 8, le);
                    if (v2 + 4ull * c2 > data + size) continue;
                    for (uint32_t k = 0; k < c2; ++k)
                        counts[k] = (vsz2 == 2) ? rd16(v2 + 2 * k, le) : rd32(v2 + 4 * k, le);
                    ncounts = c2;
                }
                for (uint32_t k = 0; k < cnt && k < 4096; ++k) {
                    const uint64_t off = valueAt(k);
                    const uint64_t len = k < ncounts ? counts[k] : 0;
                    if (off > size) return r;
                    end = std::max(end, off + len);
                }
            }
        }
        const uint8_t* after = data + ifd + 2 + 12ull * count;
        ifd = rd32(after, le);
        ++chain;
    }
    if (ifd != 0 && chain >= 8) return r; // runaway chain in probe
    if (!sawStrips) return r;             // no strip extent -> unbounded -> not carveable
    if (end < 8 || end > (100ull << 30)) return r;

    r.valid = true;
    r.size = end;
    r.confidence = 85;
    return r;
}

StructuralParseResult parseRiff(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (!data || size < 12) return r;
    if (!(data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F')) return r;
    const uint64_t declared = rd32(data + 4, true) + 8ull; // size field excludes the 8-byte RIFF header
    if (declared < 12) return r;
    r.valid = true;
    r.size = declared;
    r.confidence = 65;
    return r;
}

StructuralParseResult parseMpegTs(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (!data || size < 4 * 188) return r;
    for (int stride : {188, 192}) {
        const size_t off = (stride == 192) ? 4u : 0u;
        if (off + 4 > size || data[off] != 0x47) continue;
        size_t packets = 0, bad = 0;
        for (size_t pos = off; pos + 188 <= size; pos += stride) {
            if (data[pos] == 0x47) { ++packets; bad = 0; }
            else if (++bad >= 3) break;
        }
        if (packets >= 4) {
            r.valid = true;
            r.size = static_cast<uint64_t>(packets) * stride;
            r.confidence = 65;
            return r;
        }
    }
    return r;
}

StructuralParseResult parseSevenZip(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    static const uint8_t kMagic[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    if (!data || size < 32 || memcmp(data, kMagic, 6) != 0) return r;
    const uint64_t nho = rd64le(data + 12);
    const uint64_t nhs = rd64le(data + 20);
    if (nhs == 0 || nho + nhs > (64ull << 30)) return r;
    r.valid = true;
    r.size = 32 + nho + nhs;
    r.confidence = 70;
    return r;
}

StructuralParseResult parseCab(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (!data || size < 24) return r;
    if (!(data[0] == 'M' && data[1] == 'S' && data[2] == 'C' && data[3] == 'F')) return r;
    const uint64_t declared = rd32(data + 8, true);
    if (declared < 24) return r;
    r.valid = true;
    r.size = declared;
    r.confidence = 70;
    return r;
}

} // namespace carver
} // namespace byteback
