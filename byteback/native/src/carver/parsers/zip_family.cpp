#include "carver/structural_parsers.h"
#include "carver/file_validators.h"
#include <algorithm>
#include <cstring>

namespace byteback {
namespace carver {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

const uint8_t* findEocd(const uint8_t* data, size_t size) {
    if (size < 22) return nullptr;
    size_t scanStart = size > 65557 ? size - 65557 : 0;
    for (size_t i = size - 22; i >= scanStart; --i) {
        if (data[i] == 'P' && data[i + 1] == 'K' && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
            return data + i;
        }
        if (i == 0) break;
    }
    return nullptr;
}

std::string sniffOfficeSubtype(const uint8_t* data, size_t size) {
    const char* kWord = "word/";
    const char* kXl = "xl/";
    const char* kPpt = "ppt/";
    const size_t probe = std::min<size_t>(size, 256 * 1024);
    for (size_t i = 0; i + 5 < probe; ++i) {
        if (std::memcmp(data + i, kWord, 5) == 0) return "docx";
        if (std::memcmp(data + i, kXl, 3) == 0) return "xlsx";
        if (std::memcmp(data + i, kPpt, 4) == 0) return "pptx";
    }
    return {};
}

} // namespace

StructuralParseResult parseZipFamily(const uint8_t* data, size_t size) {
    StructuralParseResult r;
    if (size < 4 || data[0] != 'P' || data[1] != 'K' || data[2] != 0x03 || data[3] != 0x04) return r;

    int base = validateZip(data, size);
    if (base <= 0) return r;

    r.valid = true;
    r.confidence = base;
    r.size = size;

    if (const uint8_t* eocd = findEocd(data, size)) {
        uint16_t commentLen = static_cast<uint16_t>(eocd[20]) | (static_cast<uint16_t>(eocd[21]) << 8);
        uint64_t end = static_cast<uint64_t>((eocd - data) + 22 + commentLen);
        if (end <= size && end > 0) {
            r.size = end;
            r.confidence = std::max(r.confidence, 92);
        }
    }

    std::string sub = sniffOfficeSubtype(data, size);
    if (!sub.empty()) {
        r.extension = sub;
        r.confidence = std::max(r.confidence, 88);
    }
    return r;
}

} // namespace carver
} // namespace byteback
