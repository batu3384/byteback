#include "carver/structural_parsers.h"
#include <algorithm>
#include <cstring>

namespace byteback {
namespace carver {

namespace {

uint32_t readBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

uint32_t sqlitePageSize(const uint8_t* hdr) {
    uint16_t raw = readBe16(hdr + 16);
    if (raw == 1) return 65536;
    return raw;
}

bool validBtreePageType(uint8_t t) {
    return t == 0x02 || t == 0x05 || t == 0x0a || t == 0x0d || t == 0x0f || t == 0x10;
}

} // namespace

StructuralParseResult parseSqliteDb(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    static const char kMagic[] = "SQLite format 3";
    if (size < 100 || std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return r;

    uint32_t pageSize = sqlitePageSize(data);
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) != 0) return r;

    uint32_t pageCount = readBe32(data + 28);
    if (pageCount == 0 || pageCount > 0x0FFFFFFF) return r;

    uint64_t dbSize = uint64_t(pageSize) * uint64_t(pageCount);
    if (dbSize < 100 || dbSize > (1ULL << 40)) return r;

    r.valid = true;
    r.size = dbSize;
    r.extension = "sqlite";
    r.confidence = 65;

    if (size >= 101 && validBtreePageType(data[100])) r.confidence = 80;
    if (size >= dbSize) r.confidence = 90;
    return r;
}

} // namespace carver
} // namespace byteback
