// Size-bounding structural parsers for footer-less formats.
//
// CA-001 policy: a footer-less carve candidate may only be emitted when one
// of these parsers derives a real file end from its own structure. Anything
// unbounded would otherwise be emitted at the signature's maxSize — the
// phantom-record generator that flooded results.
#include "carver/structural_parsers.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace byteback {
namespace carver {

namespace {

// W2: real-disk fetch budget shared by the targeted parsers — a crafted
// stream must not turn a bounded walk into millions of sector reads.
constexpr uint64_t kFetchBudget = 65536;

// Fetch `len` bytes at absolute offset `abs`, serving from the probe window
// when possible, otherwise through the caller's readAt (budgeted).
bool fetchAt(const uint8_t* probe, size_t probeSize, uint64_t probeAbsOffset,
             const BoxHeaderReader& readAt, uint64_t& budget,
             uint64_t abs, uint32_t len, uint8_t* out) {
    if (abs >= probeAbsOffset && abs + len <= probeAbsOffset + probeSize) {
        std::memcpy(out, probe + static_cast<size_t>(abs - probeAbsOffset), len);
        return true;
    }
    if (budget == 0 || !readAt) return false;
    --budget;
    return readAt(abs, len, out);
}

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

} // namespace

// TIFF: walk the IFD chain; the max StripOffsets[i]+StripByteCounts[i] is the
// true end for non-fragmented camera files (the dominant recovery case).
// CA-038: TIFF-variant camera RAWs share the exact IFD layout with a variant
// magic at 0 — Olympus ORF "IIRO"/"IIRS" and Panasonic RW2 "IIU\0" — so the
// walker accepts them too; only the RAW signatures dispatch here with those
// extensions.
StructuralParseResult parseTiff(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (!data || size < 8) return r;
    const bool le = data[0] == 0x49 && data[1] == 0x49 &&
                    ((data[2] == 0x2A && data[3] == 0x00) ||
                     (data[2] == 0x52 && (data[3] == 0x4F || data[3] == 0x53)) ||
                     (data[2] == 0x55 && data[3] == 0x00));
    const bool be = data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A;
    if (!le && !be) return r;

    uint64_t end = 0;
    uint32_t ifd = rd32(data + 4, le);
    int chain = 0;
    bool sawStrips = false;
    while (ifd != 0 && ifd + 2 <= size && chain < 8) {
        const uint16_t count = rd16(data + ifd, le);
        if (count == 0 || count > 4096 || ifd + 2 + 12ull * count + 4 > size) return r;
        // CA-056: keep every operand uint64_t — on Linux uint64_t is
        // `unsigned long`, so mixing in `12ull` (unsigned long long) broke
        // std::max's template deduction (MSVC has a single 64-bit type).
        end = std::max(end, static_cast<uint64_t>(ifd) + 2 + 12 * static_cast<uint64_t>(count) + 4);
        const uint8_t* ent = data + ifd + 2;
        for (uint16_t i = 0; i < count; ++i, ent += 12) {
            const uint16_t tag = rd16(ent, le);
            const uint16_t type = rd16(ent + 2, le);
            const uint32_t cnt = rd32(ent + 4, le);
            if (type != 3 && type != 4) continue; // SHORT / LONG only
            const size_t vsz = (type == 3) ? 2u : 4u;
            // TIFF 6.0: the value is stored inline in bytes 8..11 when it fits
            // in 4 bytes — for SHORT that means count <= 2, not count <= 1.
            const uint64_t total = vsz * 1ull * cnt;
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
                    const uint8_t* v2 = (vsz2 * 1ull * c2 <= 4) ? e2 + 8 : data + rd32(e2 + 8, le);
                    if (v2 + vsz2 * 1ull * c2 > data + size) continue;
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

// X3F ("FOVb", Sigma/Foveon RAW) — deliberately NOT implemented as a size
// bound. Verified layout (kalpanika/x3f, src/x3f_io.c, x3f_new_from_file):
// magic "FOVb"@0, version u32@4, unique identifier 16B@8, then
// version-dependent geometry fields — and the DIRECTORY POINTER is read from
// the LAST FOUR BYTES of the file:
//     fseek(infile, -4, SEEK_END); fseek(infile, x3f_get4(infile), SEEK_SET);
// followed by the "SECd" directory (u32 version, u32 count, entries of
// {u32 offset, u32 size, u32 type id}). The size bound therefore exists only
// at the file END: deriving it from a start-anchored probe would require
// reading up to the signature's full 128 MiB maxSize to locate the directory,
// which CA-001 forbids (unbounded candidate work). The X3F signature stays
// maxSize-bounded and the expire path drops unboundable candidates — the
// documented behavior since CA-038. Ceiling: X3F carves remain coarse until a
// start-side bound is proven in the wild.

namespace {

// EBML variable-length integer: leading zero bits give the length; all value
// bits set means "unknown size" (unboundable). Returns false on truncation.
bool readEbmlVint(const uint8_t* p, uint32_t avail, uint64_t& value, uint32_t& len, bool& unknown) {
    unknown = false;
    if (avail == 0 || p[0] == 0) return false;
    uint32_t length = 1;
    while (length <= 8 && !(p[0] & (0x80u >> (length - 1)))) ++length;
    if (length > 8 || length > avail) return false;
    value = p[0] & (0xFFu >> length);
    for (uint32_t i = 1; i < length; ++i) value = (value << 8) | p[i];
    const uint64_t allBits = (1ull << (7 * length)) - 1;
    unknown = (value == allBits);
    len = length;
    return true;
}

} // namespace

// Matroska/WebM: the Segment element's vint size bounds the whole file from
// the first ~30 bytes, so even a multi-GB movie bounds from the 1MB probe.
StructuralParseResult parseMkvBounded(const uint8_t* probe, size_t probeSize,
                                      uint64_t probeAbsOffset, uint64_t maxBytes,
                                      const BoxHeaderReader& readAt) {
    StructuralParseResult r;
    uint8_t hdr[16];
    if (!probe || probeSize < 8) return r;
    uint64_t budget = kFetchBudget;
    if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget, probeAbsOffset, 12, hdr)) return r;
    static const uint8_t kEbml[4] = {0x1A, 0x45, 0xDF, 0xA3};
    if (std::memcmp(hdr, kEbml, 4) != 0) return r;

    uint64_t hdrSize = 0;
    uint32_t hdrLen = 0;
    bool unknown = false;
    if (!readEbmlVint(hdr + 4, 8, hdrSize, hdrLen, unknown) || unknown) return r;

    // Segment ID follows the whole EBML header element: ID + vint + payload.
    const uint64_t segIdOff = 4 + hdrLen + hdrSize;
    uint8_t seg[16];
    if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget, probeAbsOffset + segIdOff, 12, seg)) return r;
    static const uint8_t kSegment[4] = {0x18, 0x53, 0x80, 0x67};
    if (std::memcmp(seg, kSegment, 4) != 0) return r;

    uint64_t segSize = 0;
    uint32_t segLen = 0;
    if (!readEbmlVint(seg + 4, 8, segSize, segLen, unknown) || unknown || segSize == 0) return r;

    const uint64_t total = segIdOff + 4 + segLen + segSize;
    if (total > maxBytes) return r;
    r.valid = true;
    r.size = total;
    r.confidence = 80;
    return r;
}

// OGG page CRC: poly 0x04c11db7, init 0, unreflected, no final xor.
uint32_t oggCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04c11db7u) : (crc << 1);
        }
    }
    return crc;
}

// OGG: walk pages until the EOS flag or a broken capture. Every page's CRC
// is verified — junk after a magic must not validate as a "page walk".
// W2: at most three bulk fetches per page (header, lacing, body in separate
// exact-size reads) under a global fetch budget — per-byte lacing reads
// turned a large stream into millions of sector reads. NB: the header and
// lacing are fetched separately so a valid page flush against the image end
// never needs an overfetch past the end (past-end reads fail on raw/physical
// backends and would drop the trailing page).
StructuralParseResult parseOggBounded(const uint8_t* probe, size_t probeSize,
                                      uint64_t probeAbsOffset, uint64_t maxBytes,
                                      const BoxHeaderReader& readAt) {
    StructuralParseResult r;
    if (!probe || probeSize < 27) return r;

    uint64_t budget = kFetchBudget;
    std::vector<uint8_t> page;
    page.reserve(27 + 255 + 65025);
    uint64_t off = 0;
    uint64_t end = 0;
    while (off + 27 <= maxBytes) {
        // Header, then lacing table, in exact-size fetches.
        uint8_t head[27 + 255];
        if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                     probeAbsOffset + off, 27, head)) break;
        const uint8_t* hdr = head;
        if (std::memcmp(hdr, "OggS", 4) != 0) break; // slack or garbage after the stream
        if (hdr[4] != 0) break;                      // stream structure version
        if (hdr[5] & 0xF8) break;                    // reserved flag bits set
        const uint32_t nseg = hdr[26];
        if (nseg == 0) break;
        if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                     probeAbsOffset + off + 27, nseg, head + 27)) break;
        uint64_t body = 0;
        for (uint32_t i = 0; i < nseg; ++i) body += head[27 + i];
        const uint64_t pageLen = 27 + nseg + body;
        if (off + pageLen > maxBytes) break;

        // CRC covers header + lacing + payload with the checksum zeroed.
        page.assign(head, head + 27 + nseg);
        if (body > 0) {
            page.resize(static_cast<size_t>(pageLen));
            if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                         probeAbsOffset + off + 27 + nseg, static_cast<uint32_t>(body),
                         page.data() + 27 + nseg)) break;
        }
        std::memset(page.data() + 22, 0, 4);
        if (oggCrc32(page.data(), static_cast<size_t>(pageLen)) != rd32(hdr + 22, true)) {
            break; // checksum mismatch: not a real OGG page
        }
        off += pageLen;
        end = off;
        if (hdr[5] & 0x04) break; // EOS page: stream ends here
    }
    if (end < 28) return r;
    r.valid = true;
    r.size = end;
    r.confidence = 80;
    return r;
}

namespace {

// MPEG audio frame header tables (ISO/IEC 11172-3 / 13818-3).
struct Mp3FrameInfo {
    uint32_t frameLen;
    uint8_t version;  // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    uint8_t layer;    // 3=L1... 1=L3
    uint8_t sampIdx;
};

// Returns false unless the 4 bytes are a plausible MPEG audio frame header.
bool parseMp3Header(const uint8_t* h, Mp3FrameInfo& out) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false; // sync
    const uint8_t version = (h[1] >> 3) & 3;
    const uint8_t layer = (h[1] >> 1) & 3;
    if (version == 1 || layer == 0) return false; // reserved
    const uint8_t bitrateIdx = h[2] >> 4;
    const uint8_t sampIdx = (h[2] >> 2) & 3;
    const uint8_t padding = (h[2] >> 1) & 1;
    if (bitrateIdx == 0 || bitrateIdx == 15 || sampIdx == 3) return false;

    static const uint16_t kSampling[4][4] = {
        {11025, 12000, 8000, 0},  // MPEG2.5
        {0, 0, 0, 0},             // reserved
        {22050, 24000, 16000, 0}, // MPEG2
        {44100, 48000, 32000, 0}, // MPEG1
    };
    // kbps tables; idx 0 and 15 are invalid.
    static const uint16_t kBitrateMpeg1[4][16] = {
        {}, // reserved layer
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},  // L3
        {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0}, // L2
        {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0}, // L1
    };
    static const uint16_t kBitrateMpeg2[4][16] = {
        {}, // reserved
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},      // L3
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},      // L2
        {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0}, // L1
    };
    const uint16_t bitrate = (version == 3) ? kBitrateMpeg1[layer][bitrateIdx]
                                            : kBitrateMpeg2[layer][bitrateIdx];
    const uint16_t sampling = kSampling[version][sampIdx];
    if (bitrate == 0 || sampling == 0) return false;

    // Samples per frame via the standard length formulas:
    //   L1: (12*bit*1000/samp + pad) * 4
    //   L2 (all): 144*bit*1000/samp + pad ; L3 MPEG1: 144..., MPEG2/2.5: 72...
    uint64_t n;
    if (layer == 3) { // Layer I
        n = 12ull * bitrate * 1000 / sampling;
        out.frameLen = static_cast<uint32_t>(n + padding) * 4;
    } else {
        n = (layer == 2 || version == 3) ? 144ull : 72ull;
        out.frameLen = static_cast<uint32_t>(n * bitrate * 1000 / sampling + padding);
    }
    out.version = version;
    out.layer = layer;
    out.sampIdx = sampIdx;
    if (out.frameLen < 24 || out.frameLen > 2048) return false;
    return true;
}

} // namespace

StructuralParseResult parseMp3Bounded(const uint8_t* probe, size_t probeSize,
                                      uint64_t probeAbsOffset, uint64_t maxBytes,
                                      const BoxHeaderReader& readAt) {
    StructuralParseResult r;
    if (!probe || probeSize < 16) return r;
    uint64_t budget = kFetchBudget;

    uint64_t off = 0;
    // Skip ID3v2: 'ID3' + ver/rev + flags + 4 syncsafe size bytes (+footer).
    uint8_t hdr[10];
    if (probe[0] == 'I' && probe[1] == 'D' && probe[2] == '3' && probeSize >= 10) {
        std::memcpy(hdr, probe, 10);
        const uint64_t tagSize = (static_cast<uint64_t>(hdr[6] & 0x7F) << 21) |
                                 (static_cast<uint64_t>(hdr[7] & 0x7F) << 14) |
                                 (static_cast<uint64_t>(hdr[8] & 0x7F) << 7) |
                                 static_cast<uint64_t>(hdr[9] & 0x7F);
        off = 10 + tagSize + ((hdr[5] & 0x10) ? 10 : 0);
        if (off + 4 > maxBytes) return r;
        // Reload bytes at the frame start if the tag ran past the probe.
        uint8_t tmp[4];
        if (off + 4 <= probeSize) {
            std::memcpy(tmp, probe + off, 4);
        } else if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                            probeAbsOffset + off, 4, tmp)) {
            return r;
        }
        if (tmp[0] != 0xFF || (tmp[1] & 0xE0) != 0xE0) return r;
    }

    // Require a consistent run before accepting the walk.
    Mp3FrameInfo first{};
    uint64_t pos = off;
    int consistent = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos + 4 > maxBytes) break;
        uint8_t h[4];
        if (pos + 4 <= probeSize) {
            std::memcpy(h, probe + pos, 4);
        } else if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                            probeAbsOffset + pos, 4, h)) {
            break;
        }
        Mp3FrameInfo fi{};
        if (!parseMp3Header(h, fi)) break;
        if (consistent == 0) first = fi;
        else if (fi.version != first.version || fi.layer != first.layer ||
                 fi.sampIdx != first.sampIdx) break;
        pos += fi.frameLen;
        ++consistent;
    }
    if (consistent < 4) return r;

    // Walk the rest of the stream.
    int frames = consistent;
    while (pos + 4 <= maxBytes) {
        uint8_t h[4];
        if (pos + 4 <= probeSize) {
            std::memcpy(h, probe + pos, 4);
        } else if (!fetchAt(probe, probeSize, probeAbsOffset, readAt, budget,
                            probeAbsOffset + pos, 4, h)) {
            break;
        }
        Mp3FrameInfo fi{};
        if (!parseMp3Header(h, fi) || fi.version != first.version ||
            fi.layer != first.layer || fi.sampIdx != first.sampIdx ||
            fi.frameLen > maxBytes - pos) {
            break;
        }
        pos += fi.frameLen;
        ++frames;
    }

    // ~20 frames at 128 kbps is ~0.6 s of audio — below that, a chain of
    // coincidental headers is too cheap for an attacker to be worth keeping.
    if (frames < 20 || pos < 4) return r;
    r.valid = true;
    r.size = pos;
    r.confidence = 85;
    return r;
}

} // namespace carver
} // namespace byteback
