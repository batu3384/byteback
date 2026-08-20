#pragma once

// Fast Object Validation (FOV) for carved files.
//
// Aho-Corasick header/footer matching (the carving engine) is fast but
// imprecise: any byte sequence starting with FF D8 FF is accepted as JPEG,
// and ZIP / ODT / DOCX / EPUB share the PK 03 04 magic. These structural
// validators walk the *internal* layout of each candidate type and reject
// candidates whose body does not conform, which is what separates Scalpel
// (header/footer) from X-Ways / Garfinkel's validated carving.
//
// Each validator takes the candidate buffer [data, data+size) and returns a
// confidence in [0,100]: 0 = rejected, 100 = fully validated, intermediate
// values for partial validation (e.g. footer truncated but header sound).
// Validators are pure and side-effect free so they can be unit-tested.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace byteback {
namespace carver {

// Read a little-endian unsigned of N bytes (1..4) from a bounds-checked ptr.
inline uint32_t readLe(const uint8_t* p, size_t bytes) {
    uint32_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}

// Read a big-endian unsigned of N bytes (1..4). JPEG segment lengths and PNG
// chunk lengths/CRCs are stored big-endian; ZIP and GZIP are little-endian.
inline uint32_t readBe(const uint8_t* p, size_t bytes) {
    uint32_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v = (v << 8) | uint32_t(p[i]);
    return v;
}

// CRC-32 (IEEE 802.3, reflected), the polynomial PNG chunks and GZIP footers
// use. Inline here so validators can verify chunk/footer CRCs without linking
// an external hash library. Polynomial 0xEDB88320.
inline uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---- JPEG ----
// Structure: FF D8 (SOI), then a sequence of segments FF xx [len payload],
// terminated by FF D9 (EOI). We accept the candidate if SOI is present, at
// least one well-formed APP/DQT/SOF/DHT/SOS segment follows, and the buffer
// contains an EOI marker. We do not require EOI at the very end — a truncated
// tail is common in carved JPEGs and still yields a viewable image.
inline int validateJpeg(const uint8_t* data, size_t size) {
    if (size < 4) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0; // SOI

    bool sawSegment = false;
    bool sawSos = false;
    size_t i = 2;
    while (i + 1 < size) {
        if (data[i] != 0xFF) { i++; continue; }
        uint8_t marker = data[i + 1];
        if (marker == 0xD9) return sawSos ? 95 : (sawSegment ? 70 : 30); // EOI
        if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; } // stuffing / RST
        // Standalone markers (SOI/EOI/RSTn) handled above; the rest carry a length.
        if (i + 3 >= size) break;
        uint16_t segLen = static_cast<uint16_t>(readBe(data + i + 2, 2));
        if (segLen < 2) return 20; // malformed length
        sawSegment = true;
        if (marker == 0xDA) sawSos = true; // SOS -> entropy-coded scan follows
        i += 2 + segLen; // skip marker bytes + segment (length includes the 2 len bytes)
        // After SOS the entropy-coded data runs until the next non-stuffed FF marker;
        // resume scanning for EOI from there.
        if (sawSos) {
            while (i + 1 < size) {
                if (data[i] == 0xFF && data[i + 1] != 0x00 &&
                    !(data[i + 1] >= 0xD0 && data[i + 1] <= 0xD7)) break;
                i++;
            }
        }
    }
    // No EOI found but structure is sound -> partial (common for carving).
    return sawSos ? 75 : (sawSegment ? 50 : 20);
}

// ---- PNG ----
// Signature (8 bytes), then chunks: [length:4][type:4][data:length][crc:4].
// IHDR must be first; IEND terminates. We verify the CRC of every chunk we
// can reach — a correct CRC is extremely strong evidence the carve is real.
inline int validatePng(const uint8_t* data, size_t size) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || std::memcmp(data, SIG, 8) != 0) return 0;

    size_t i = 8;
    bool sawIhdr = false;
    bool sawIend = false;
    int checkedChunks = 0;
    while (i + 12 <= size) {
        uint32_t len = readBe(data + i, 4);
        const uint8_t* type = data + i + 4;
        // Chunk type must be ASCII letters.
        bool typeOk = true;
        for (int k = 0; k < 4; ++k) {
            uint8_t c = type[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) { typeOk = false; break; }
        }
        if (!typeOk) return sawIend ? 90 : 30;

        if (!sawIhdr) {
            if (std::memcmp(type, "IHDR", 4) != 0) return 10; // IHDR must be first
            sawIhdr = true;
        }
        if (std::memcmp(type, "IEND", 4) == 0) { sawIend = true; break; }

        uint64_t dataStart = i + 8;
        uint64_t crcPos = dataStart + len;
        if (crcPos + 4 > size) return sawIend ? 90 : 60; // truncated chunk

        uint32_t stored = readBe(data + crcPos, 4);
        uint32_t computed = crc32(data + i + 4, len + 4); // CRC covers type+data
        if (stored != computed) return 40; // CRC mismatch -> likely false positive
        checkedChunks++;
        i = crcPos + 4;
    }
    if (!sawIhdr) return 0;
    // IEND present + at least one validated chunk => very high confidence.
    if (sawIend && checkedChunks > 0) return 98;
    if (sawIend) return 80;
    return checkedChunks > 0 ? 70 : 40; // truncated but structurally sound
}

// ---- ZIP / DOCX / XLSX / ODT / EPUB / JAR ----
// All share the PK container. We look for the End Of Central Directory record
// (PK\x05\x06), which only a well-formed ZIP archive contains, and at least
// one Central Directory entry (PK\x01\x02). This distinguishes real archives
// from random data that merely starts with PK\x03\x04.
inline int validateZip(const uint8_t* data, size_t size) {
    if (size < 4) return 0;
    if (data[0] != 'P' || data[1] != 'K' || data[2] != 0x03 || data[3] != 0x04) return 0;

    bool sawCentral = false;
    bool sawEocd = false;
    // Scan from the end for EOCD (most ZIPs keep it in the last 64KB).
    size_t scanStart = size > 65557 ? size - 65557 : 0;
    for (size_t i = scanStart; i + 4 <= size; ++i) {
        if (data[i] == 'P' && data[i + 1] == 'K') {
            if (data[i + 2] == 0x01 && data[i + 3] == 0x02) sawCentral = true;
            if (data[i + 2] == 0x05 && data[i + 3] == 0x06) sawEocd = true;
        }
    }
    if (sawEocd && sawCentral) return 95;
    if (sawEocd) return 70;
    if (sawCentral) return 60;
    return 30; // local header only — weak
}

// ---- PDF ----
// Header %PDF-1.x, then objects; trailer must contain %%EOF. We accept if the
// header is well-formed and at least one xref/trailer/obj marker is present.
inline int validatePdf(const uint8_t* data, size_t size) {
    if (size < 8) return 0;
    if (std::memcmp(data, "%PDF-", 5) != 0) return 0;
    // Version digit must be present.
    if (data[5] < '0' || data[5] > '9') return 20;

    bool sawObj = false;
    bool sawEof = false;
    // Search the tail for %%EOF; scan body for an "obj" keyword.
    size_t tailStart = size > 1024 ? size - 1024 : 0;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (!sawObj && data[i] == 'o' && data[i + 1] == 'b' && data[i + 2] == 'j') sawObj = true;
    }
    for (size_t i = tailStart; i + 5 <= size; ++i) {
        if (data[i] == '%' && data[i + 1] == '%' && data[i + 2] == 'E' &&
            data[i + 3] == 'O' && data[i + 4] == 'F') { sawEof = true; break; }
    }
    if (sawObj && sawEof) return 90;
    if (sawObj) return 55; // no EOF -> truncated
    if (sawEof) return 50; // EOF without any obj -> suspicious
    return 25;
}

// ---- RIFF container (WebP / AVI / WAV) ----
// The RIFF header carries the subtype at bytes 8..11 ("WEBP", "AVI ", "WAVE");
// carving by the bare magic reports the same bytes once and the subtype
// resolves the real extension at emit time (CA-006).
inline const char* detectRiffSubtype(const uint8_t* data, size_t size) {
    if (size < 12) return nullptr;
    if (std::memcmp(data + 8, "WEBP", 4) == 0) return "webp";
    if (std::memcmp(data + 8, "AVI ", 4) == 0) return "avi";
    if (std::memcmp(data + 8, "WAVE", 4) == 0) return "wav";
    return nullptr;
}

inline int validateRiff(const uint8_t* data, size_t size) {
    if (size < 12) return 0;
    if (std::memcmp(data, "RIFF", 4) != 0) return 0;
    // A trailing RIFF tag is optional in practice; subtype presence is the
    // structural evidence we can rely on.
    return detectRiffSubtype(data, size) ? 90 : 15;
}

// ---- GZIP ----
// Header: 1F 8B [method] [flags] [mtime:4] [xfl] [os]. Method must be 0x08
// (deflate). The footer is a CRC-32 of the uncompressed data + uncompressed
// size (modulo 2^32). We validate the header structurally; full CRC requires
// inflating, which is out of scope for the validator (left to the recovery
// engine). Returns high confidence for a sound header.
inline int validateGzip(const uint8_t* data, size_t size) {
    if (size < 10) return 0;
    if (data[0] != 0x1F || data[1] != 0x8B) return 0;
    if (data[2] != 0x08) return 20; // compression method must be deflate
    uint8_t flags = data[3];
    if (flags & 0xE0) return 20; // reserved bits must be zero

    size_t i = 10;
    if (flags & 0x04) { // FEXTRA
        if (i + 2 > size) return 30;
        uint16_t xlen = readLe(data + i, 2);
        i += 2 + xlen;
        if (i > size) return 30;
    }
    if (flags & 0x08) { // FNAME (NUL-terminated)
        while (i < size && data[i] != 0) i++;
        if (i >= size) return 30;
        i++;
    }
    if (flags & 0x10) { // FCOMMENT (NUL-terminated)
        while (i < size && data[i] != 0) i++;
        if (i >= size) return 30;
        i++;
    }
    // A deflated body + 8-byte footer must follow the header fields we skipped.
    if (i + 8 > size) return 40;
    return 85;
}

// ---- MPEG Transport Stream ----
// 188-byte packets each begin with sync byte 0x47. A lone 0x47 0x40 0x00
// prefix is extremely common in random disk data; require several aligned
// packets before accepting a carve (CA-008).
inline int validateMpegTs(const uint8_t* data, size_t size) {
    constexpr size_t kPacket = 188;
    constexpr int kPackets = 5;
    if (size < kPacket * kPackets) return 0;
    for (int i = 0; i < kPackets; ++i) {
        if (data[i * kPacket] != 0x47) return 0;
    }
    return 92;
}

} // namespace carver
} // namespace byteback
