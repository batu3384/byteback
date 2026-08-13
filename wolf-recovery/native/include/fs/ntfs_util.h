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

} // namespace ntfs
} // namespace wolf
