#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wolf {
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

inline int validateSqlite(const uint8_t* data, size_t size) {
    return parseSqliteDb(data, size).confidence;
}

inline int validateMp4(const uint8_t* data, size_t size) {
    return parseMp4Mov(data, size).confidence;
}

} // namespace carver
} // namespace wolf
