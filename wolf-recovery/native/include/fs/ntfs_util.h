#pragma once

// NTFS parsing helpers shared between the MFT carving parser, the journal
// parsers (UsnJrnl / LogFile) and the index (INDX) walker. Centralising these
// keeps the byte-level details (USA fixup, FILETIME conversion, UTF-16LE ->
// UTF-8) consistent and unit-testable.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wolf {
namespace ntfs {

// Attribute type constants (NTFS $MFT attribute table).
constexpr uint32_t ATTR_STANDARD_INFORMATION = 0x10;
constexpr uint32_t ATTR_ATTRIBUTE_LIST       = 0x20;
constexpr uint32_t ATTR_FILE_NAME            = 0x30;
constexpr uint32_t ATTR_OBJECT_ID            = 0x40;
constexpr uint32_t ATTR_SECURITY_DESCRIPTOR  = 0x50;
constexpr uint32_t ATTR_VOLUME_NAME          = 0x60;
constexpr uint32_t ATTR_VOLUME_INFORMATION   = 0x70;
constexpr uint32_t ATTR_DATA                 = 0x80;
constexpr uint32_t ATTR_INDEX_ROOT           = 0x90;
constexpr uint32_t ATTR_INDEX_ALLOCATION     = 0xA0;
constexpr uint32_t ATTR_INDEX_BITMAP         = 0xB0;
constexpr uint32_t ATTR_REPARSE_POINT        = 0xC0;
constexpr uint32_t ATTR_EA_INFORMATION       = 0xD0;
constexpr uint32_t ATTR_EA                   = 0xE0;
constexpr uint32_t ATTR_LOGGED_UTILITY_STREAM= 0x100;
constexpr uint32_t ATTR_END_MARKER           = 0xFFFFFFFF;

// MFT record flags.
constexpr uint16_t RECORD_FLAG_IN_USE    = 0x01;
constexpr uint16_t RECORD_FLAG_DIRECTORY = 0x02;

// $FILE_NAME name-type flags.
constexpr uint8_t NAME_TYPE_POSIX     = 0x00;
constexpr uint8_t NAME_TYPE_LONG      = 0x01; // Win32 (long, mixed case)
constexpr uint8_t NAME_TYPE_SHORT     = 0x02; // DOS 8.3
constexpr uint8_t NAME_TYPE_LONG_SHORT = 0x03; // Both (valid long + short pair)

// FILETIME epoch is 1601-01-01, 11644473600 seconds before Unix epoch (1970).
constexpr uint64_t FILETIME_UNIX_EPOCH_OFFSET_100NS = 116444736000000000ULL;
constexpr uint64_t FILETIME_TICKS_PER_SECOND        = 10000000ULL;

// Convert a Windows FILETIME (uint64, 100ns ticks since 1601) to Unix epoch
// seconds. Returns 0 for empty/invalid/underflow values so callers can store
// "unknown" without branching on negative numbers. Resolution is intentionally
// seconds (the forensic store uses second-resolution timestamps); sub-second
// precision is dropped.
inline int64_t filetimeToUnix(uint64_t filetime) {
    if (filetime == 0 || filetime < FILETIME_UNIX_EPOCH_OFFSET_100NS) return 0;
    return static_cast<int64_t>((filetime - FILETIME_UNIX_EPOCH_OFFSET_100NS) /
                                FILETIME_TICKS_PER_SECOND);
}

// Decode a UTF-16LE code unit run into UTF-8. Surrogate pairs are handled so
// non-BMP characters (emoji, rare scripts) survive intact; malformed sequences
// are replaced with U+FFFD (replacement char) rather than truncating the name.
//
// This replaces the previous ASCII-only decoder that mangled every non-ASCII
// filename (Turkish ç/ğ/ş/ı, CJK, Cyrillic, etc.) into underscores.
inline std::string utf16leToUtf8(const uint16_t* units, size_t count) {
    if (!units || count == 0) return {};

    std::string out;
    out.reserve(count * 2); // most BMP text averages ~2 UTF-8 bytes/codepoint

    auto appendUtf8 = [&out](uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };

    for (size_t i = 0; i < count; ++i) {
        uint16_t u = units[i];
        if (u == 0) break; // NT name strings are NUL-terminated

        // High surrogate -> expect a low surrogate next.
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < count) {
                uint16_t lo = units[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    uint32_t cp = 0x10000 +
                                  ((static_cast<uint32_t>(u) - 0xD800) << 10) +
                                  (static_cast<uint32_t>(lo) - 0xDC00);
                    appendUtf8(cp);
                    ++i;
                    continue;
                }
            }
            appendUtf8(0xFFFD); // dangling high surrogate
            continue;
        }
        // Unexpected low surrogate without preceding high.
        if (u >= 0xDC00 && u <= 0xDFFF) {
            appendUtf8(0xFFFD);
            continue;
        }
        appendUtf8(u);
    }
    return out;
}

// Apply the NTFS Update Sequence Array (USA) fixup to a sector-straddling
// record (FILE / INDX). Before flushing a record to disk, NTFS overwrites the
// last two bytes of each constituent sector with a running sequence number and
// stores the original bytes in the USA. Readers must restore those originals
// before parsing, otherwise attribute offsets that land on sector boundaries
// will read garbage.
//
// record:           pointer to the start of the record ("FILE"/"INDX" sig).
// recordSize:       total record length in bytes (typically 1024 or 4096).
// sectorSize:       physical sector size (512 or 4096).
// usaOffset:        offset to the USA from record start (from the record header).
// usaCount:         number of entries in the USA INCLUDING the 2-byte sequence
//                   number at usa[0] (i.e. equal to number_of_sectors + 1).
//
// Returns true if the fixup applied cleanly and the sequence number matched
// every sector's trailing bytes. Returns false on any mismatch — callers may
// still parse the record but should treat attribute reads crossing a sector
// boundary with suspicion.
inline bool applyUsaFixup(uint8_t* record, size_t recordSize, size_t sectorSize,
                          uint16_t usaOffset, uint16_t usaCount) {
    if (!record || recordSize == 0 || sectorSize == 0) return false;
    if (usaCount < 2) return false;                       // need seq + >=1 entry
    if (usaCount - 1 > recordSize / sectorSize) return false; // more entries than sectors

    if (static_cast<size_t>(usaOffset) + sizeof(uint16_t) * usaCount > recordSize) return false;

    const uint16_t* usa = reinterpret_cast<const uint16_t*>(record + usaOffset);
    uint16_t seq = usa[0];

    for (uint16_t i = 1; i < usaCount; ++i) {
        size_t sectorEnd = static_cast<size_t>(i) * sectorSize - sizeof(uint16_t);
        if (sectorEnd + sizeof(uint16_t) > recordSize) return false;

        uint16_t* tail = reinterpret_cast<uint16_t*>(record + sectorEnd);
        if (*tail != seq) {
            // Sequence mismatch — record was partially overwritten or comes
            // from stale slack. Stop restoring; caller decides whether to keep.
            return false;
        }
        *tail = usa[i]; // restore the original bytes the record held on disk
    }
    return true;
}

// Decompress an LZNT1 stream (the algorithm NTFS uses for compressed files).
//
// LZNT1 is a chunked LZ77 variant. A stream is a sequence of chunks, each
// preceded by a 2-byte little-endian header:
//   - bit 15 set   -> the chunk is compressed; payload size = (header & 0x0FFF) + 1
//   - bit 15 clear -> literal chunk; copy (header & 0x0FFF) + 1 bytes verbatim
//   - header == 0  -> end-of-stream marker
//
// Inside a compressed chunk the payload is a sequence of (flag byte, 8 tokens).
// Flag bit clear = literal byte; bit set = 16-bit back-reference token. The
// split between length and displacement inside a token depends on how far into
// the chunk we have already decompressed: the displacement field grows (and the
// length field shrinks) as the output position advances past each power of two,
// starting at 16. This matches RtlDecompressBufferEx on Windows.
//
// Returns the number of bytes written to dst, or -1 on malformed input. Output
// is capped at dstCapacity; a partial decompression is still returned (not -1)
// as long as the stream was structurally valid — callers decide whether the
// truncated result is useful.
inline int lznt1Decompress(const uint8_t* src, size_t srcSize,
                            uint8_t* dst, size_t dstCapacity) {
    if (!src || !dst) return -1;
    size_t sp = 0, dp = 0;

    while (sp + 1 < srcSize) {
        uint16_t hdr = static_cast<uint16_t>(src[sp]) |
                       (static_cast<uint16_t>(src[sp + 1]) << 8);
        sp += 2;
        if (hdr == 0) break; // end-of-stream marker

        size_t chunkLen = (hdr & 0x0FFF) + 1;
        if (sp + chunkLen > srcSize) return -1; // truncated chunk

        if (!(hdr & 0x8000)) {
            // Uncompressed chunk: literal copy.
            size_t copy = (dp + chunkLen <= dstCapacity) ? chunkLen : (dstCapacity - dp);
            if (copy > 0) {
                std::memmove(dst + dp, src + sp, copy);
            }
            dp += chunkLen;
            sp += chunkLen;
            continue;
        }

        // Compressed chunk.
        const size_t chunkEnd = sp + chunkLen;
        const size_t chunkBase = dp; // output position at chunk start

        while (sp < chunkEnd) {
            uint8_t flags = src[sp++];
            for (int bit = 0; bit < 8 && sp < chunkEnd; ++bit) {
                if (!(flags & (1u << bit))) {
                    // Literal byte.
                    if (dp < dstCapacity) dst[dp] = src[sp];
                    ++dp;
                    ++sp;
                    continue;
                }

                // Back-reference token (2 bytes LE).
                if (sp + 1 >= chunkEnd) return -1;
                uint16_t token = static_cast<uint16_t>(src[sp]) |
                                 (static_cast<uint16_t>(src[sp + 1]) << 8);
                sp += 2;

                // The displacement field width grows with output position:
                // u = smallest power of two strictly greater than (dp - chunkBase),
                // minimum 16. dispBits = log2(u).
                size_t posInChunk = dp - chunkBase;
                size_t u = 0x10;
                int dispBits = 4;
                while (u <= posInChunk) { u <<= 1; ++dispBits; }
                // Guard: displacement field must fit in 16 bits with room for length.
                if (dispBits > 12) return -1;

                int lenBits = 16 - dispBits;
                size_t length = 3 + (token & ((1u << lenBits) - 1));
                size_t displacement = (token >> lenBits) + 1;

                if (displacement > dp - chunkBase) return -1; // would read before chunk start

                // Copy byte-by-byte (matches may overlap — LZ77 style).
                for (size_t k = 0; k < length; ++k) {
                    if (dp < dstCapacity) dst[dp] = dst[dp - displacement];
                    ++dp;
                }
            }
        }
    }
    return static_cast<int>(dp);
}

// Parsed USN (Update Sequence Number) journal record. These live in
// $Extend\$UsnJrnl:$J and record per-file change events (create, rename,
// delete, data extend, security change, ...). For forensic purposes they are
// a second source of truth about deleted files: even after the MFT entry is
// reused, the USN record that logged the deletion often survives in the
// journal's slack. We carve them structurally from raw sectors.
struct UsnRecord {
    uint64_t fileReference;      // MFT reference of the affected file
    uint64_t parentFileReference;
    uint64_t usn;                // monotonic journal sequence number
    int64_t  timestamp;          // Unix seconds (converted from FILETIME)
    uint32_t reasonFlags;        // bitmask of USN_REASON_* (what changed)
    uint32_t fileAttributes;     // FILE_ATTRIBUTE_* (directory, hidden, ...)
    std::string name;            // UTF-8 file name at the time of the event
};

// USN_REASON_* bit flags (subset). Presence of these in reasonFlags tells the
// forensic story: a CREATE followed later by a DELETE on the same MFT
// reference is a strong deleted-file signal.
constexpr uint32_t USN_REASON_FILE_CREATE          = 0x00000001;
constexpr uint32_t USN_REASON_FILE_DELETE          = 0x00000002;
constexpr uint32_t USN_REASON_DATA_OVERWRITE       = 0x00000004;
constexpr uint32_t USN_REASON_DATA_EXTEND          = 0x00000008;
constexpr uint32_t USN_REASON_DATA_TRUNCATION      = 0x00000010;
constexpr uint32_t USN_REASON_RENAME_OLD_NAME      = 0x00010000;
constexpr uint32_t USN_REASON_RENAME_NEW_NAME      = 0x00020000;

// Try to parse a USN v2/v3 record at the given byte offset within a buffer.
// Returns true and fills `out` on success; returns false if the bytes do not
// form a plausible record (bad version, impossible length, name out of bounds).
// `available` is the number of bytes from `data` to the buffer end — used for
// every bounds check so a malformed record cannot read past the buffer.
inline bool parseUsnRecord(const uint8_t* data, size_t available, UsnRecord& out) {
    if (!data || available < 60) return false; // minimum v2 record header

    uint32_t recordLength = static_cast<uint32_t>(data[0]) |
                            (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) |
                            (static_cast<uint32_t>(data[3]) << 24);
    // recordLength includes padding to align the next record (typically to 8).
    if (recordLength < 60 || recordLength > 65536) return false;
    if (recordLength > available) return false; // truncated

    uint16_t majorVersion = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    if (majorVersion != 2 && majorVersion != 3) return false;

    // Layout (v2): offsets are absolute from the record start.
    //   8  fileReferenceNumber (uint64)
    //   16 parentFileReferenceNumber (uint64)
    //   24 usn (uint64)
    //   32 timestamp (FILETIME uint64)
    //   40 reasonFlags (uint32)
    //   44 sourceInfo (uint32)
    //   48 securityId (uint32)
    //   52 fileAttributes (uint32)
    //   56 fileNameLength (uint16, bytes)
    //   58 fileNameOffset (uint16, from record start)
    //   60 fileName[] (UTF-16LE, fileNameLength bytes)
    auto readU64 = [&](size_t off) -> uint64_t {
        return static_cast<uint64_t>(data[off]) |
               (static_cast<uint64_t>(data[off + 1]) << 8) |
               (static_cast<uint64_t>(data[off + 2]) << 16) |
               (static_cast<uint64_t>(data[off + 3]) << 24) |
               (static_cast<uint64_t>(data[off + 4]) << 32) |
               (static_cast<uint64_t>(data[off + 5]) << 40) |
               (static_cast<uint64_t>(data[off + 6]) << 48) |
               (static_cast<uint64_t>(data[off + 7]) << 56);
    };
    auto readU32 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(data[off]) |
               (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) |
               (static_cast<uint32_t>(data[off + 3]) << 24);
    };
    auto readU16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(data[off]) | (static_cast<uint16_t>(data[off + 1]) << 8);
    };

    out.fileReference = readU64(8);
    out.parentFileReference = readU64(16);
    out.usn = readU64(24);
    out.timestamp = filetimeToUnix(readU64(32));
    out.reasonFlags = readU32(40);
    out.fileAttributes = readU32(52);

    uint16_t nameLen = readU16(56);
    uint16_t nameOff = readU16(58);
    if (nameLen == 0 || nameOff < 60) { out.name.clear(); return true; }
    if (static_cast<size_t>(nameOff) + nameLen > recordLength) return false;

    size_t nameUnits = nameLen / sizeof(uint16_t);
    const uint16_t* namePtr = reinterpret_cast<const uint16_t*>(data + nameOff);
    out.name = utf16leToUtf8(namePtr, nameUnits);
    return true;
}

// Score an MFT-sourced file record for the Results UI. Allocated records
// start at 100 (minus USA damage). Deleted records start lower and gain
// confidence only when recoverable payload survives (data runs or resident
// bytes). ponytail: no cross-check with USN yet — Phase 1b can boost when
// a matching USN DELETE exists for the same MFT ref.
inline int scoreMftConfidence(bool inUse, bool usaOk, bool isDirectory,
                              uint64_t sizeBytes, bool hasDataRuns,
                              bool hasResidentPayload) {
    int score;
    if (inUse) {
        score = 100;
        if (!usaOk) score -= 25;
        return score < 0 ? 0 : score;
    }
    score = 45;
    if (hasDataRuns && sizeBytes > 0) score += 35;
    else if (hasResidentPayload && sizeBytes > 0) score += 25;
    else if (sizeBytes > 0) score += 10;
    if (isDirectory) score -= 10;
    if (!usaOk) score -= 20;
    if (score < 0) return 0;
    if (score > 95) return 95;
    return score;
}

} // namespace ntfs
} // namespace wolf
