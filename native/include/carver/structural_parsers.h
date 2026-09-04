#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace byteback {
namespace carver {

struct StructuralParseResult {
    bool valid = false;
    uint64_t size = 0;
    std::string extension; // refined subtype, e.g. docx
    int confidence = 0;    // [0,100]
};

// ZIP / OOXML / JAR: EOCD scan + Office subtype sniff.
StructuralParseResult parseZipFamily(const uint8_t* data, size_t size);

// SQLite DB: header page size + page count -> bounded size.
StructuralParseResult parseSqliteDb(const uint8_t* data, size_t size);

// MP4/MOV: ftyp + atom walk; optional tail moov search.
StructuralParseResult parseMp4Mov(const uint8_t* data, size_t size);

// CA-018: ISOBMFF box walk with targeted reads. Only 8/16-byte box headers
// are needed to bound the file; headers beyond the probe window are fetched
// through readAt so a 2GB movie is no longer clamped to the 1MB probe.
// readAt(offset, len, out) must return false on failure. Sizes are relative
// to the carve start (probeAbsOffset).
using BoxHeaderReader = std::function<bool(uint64_t offset, uint32_t len, uint8_t* out)>;
StructuralParseResult parseIsobmffBounded(const uint8_t* probe, size_t probeSize,
                                          uint64_t probeAbsOffset, uint64_t maxBytes,
                                          const BoxHeaderReader& readAt);

// TIFF family (incl. CR2): IFD0 chain walk; StripOffsets+StripByteCounts
// bound the exact file end. Invalid or unbounded -> not carveable.
StructuralParseResult parseTiff(const uint8_t* data, size_t size);

// RIFF container: declared chunk size at bytes 4..7 bounds the file.
StructuralParseResult parseRiff(const uint8_t* data, size_t size);

// MPEG-TS: packet sync walk at 188/192-byte stride until sync is lost.
StructuralParseResult parseMpegTs(const uint8_t* data, size_t size);

// 7-Zip: StartHeader carries NextHeaderOffset/Size -> exact total size.
StructuralParseResult parseSevenZip(const uint8_t* data, size_t size);

// CAB: cbCabinet at offset 8 bounds the file.
StructuralParseResult parseCab(const uint8_t* data, size_t size);

inline int validateSqlite(const uint8_t* data, size_t size) {
    return parseSqliteDb(data, size).confidence;
}

inline int validateMp4(const uint8_t* data, size_t size) {
    return parseMp4Mov(data, size).confidence;
}

} // namespace carver
} // namespace byteback
