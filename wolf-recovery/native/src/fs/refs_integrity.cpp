#include "fs/refs_integrity.h"

namespace wolf {

namespace {

uint32_t crc32cTable[256];
bool crc32cReady = false;

void initCrc32cTable() {
    if (crc32cReady) return;
    constexpr uint32_t kCrc32cPolyRef = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? (kCrc32cPolyRef ^ (crc >> 1)) : (crc >> 1);
        }
        crc32cTable[i] = crc;
    }
    crc32cReady = true;
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

} // namespace

uint32_t refsCrc32c(const uint8_t* data, size_t len) {
    initCrc32cTable();
    uint32_t crc = 0xFFFFFFFFu;
    if (data) {
        for (size_t i = 0; i < len; ++i) {
            crc = crc32cTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint64_t refsCrc64Ecma(const uint8_t* data, size_t len) {
    constexpr uint64_t kPoly = 0x42F0E1EBA9EA3693ULL;
    uint64_t crc = 0;
    if (data) {
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint64_t>(data[i]) << 56;
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x8000000000000000ULL) ? ((crc << 1) ^ kPoly) : (crc << 1);
            }
        }
    }
    return crc;
}

bool verifyRefsChecksum(RefsChecksumType type, const uint8_t* data, size_t len,
                        size_t start, size_t end, uint64_t stored) {
    if (!data || start > end || end > len) return false;
    if (type == RefsChecksumType::Crc32C) {
        const uint32_t got = refsCrc32c(data + start, end - start);
        return got == static_cast<uint32_t>(stored);
    }
    if (type == RefsChecksumType::Crc64Ecma) {
        return refsCrc64Ecma(data + start, end - start) == stored;
    }
    return false;
}

bool tryVerifyRefsMetadataPage(const uint8_t* page, size_t pageSize, int& confidenceOut) {
    constexpr size_t kSlotOff = 48;
    constexpr size_t kVerifyEnd = 48;
    if (!page || pageSize < kSlotOff + 8) return false;
    const uint64_t stored = readLe64(page + kSlotOff);
    if (stored == 0) return false;
    const bool ok = verifyRefsChecksum(RefsChecksumType::Crc64Ecma, page, pageSize,
                                       0, kVerifyEnd, stored);
    confidenceOut = ok ? 95 : 35;
    return true;
}

} // namespace wolf
