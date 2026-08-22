#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace byteback {
namespace carver {

namespace detail {

inline uint16_t readBe16(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }
inline uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
inline uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool parseExifDateTime(const char* s, size_t n, int64_t& outUnix) {
    if (n < 19) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
#if defined(_WIN32)
    outUnix = static_cast<int64_t>(_mkgmtime(&tm));
#else
    outUnix = static_cast<int64_t>(timegm(&tm));
#endif
    return outUnix > 0;
}

inline int64_t ifdTagUnix(const uint8_t* tiff, size_t tiffSize, uint32_t ifdOff, bool le, uint16_t tagWanted) {
    if (ifdOff + 2 > tiffSize) return 0;
    const uint8_t* base = tiff;
    uint16_t count = le ? readLe16(base + ifdOff) : readBe16(base + ifdOff);
    size_t pos = ifdOff + 2;
    for (uint16_t i = 0; i < count && pos + 12 <= tiffSize; ++i, pos += 12) {
        uint16_t tag = le ? readLe16(base + pos) : readBe16(base + pos);
        uint16_t type = le ? readLe16(base + pos + 2) : readBe16(base + pos + 2);
        uint32_t cnt = le ? readLe32(base + pos + 4) : readBe32(base + pos + 4);
        if (tag != tagWanted || type != 2 || cnt < 19) continue;
        uint32_t valOff = le ? readLe32(base + pos + 8) : readBe32(base + pos + 8);
        if (cnt > 4) {
            if (valOff + cnt > tiffSize) continue;
            int64_t unix = 0;
            if (parseExifDateTime(reinterpret_cast<const char*>(base + valOff), cnt, unix)) return unix;
        } else if (pos + 8 + cnt <= tiffSize) {
            int64_t unix = 0;
            if (parseExifDateTime(reinterpret_cast<const char*>(base + pos + 8), cnt, unix)) return unix;
        }
    }
    return 0;
}

} // namespace detail

/** JPEG EXIF DateTimeOriginal (0x9003) or DateTime (0x0132). 0 if absent/invalid. */
inline int64_t extractJpegExifUnix(const uint8_t* data, size_t size) {
    if (!data || size < 22) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0;
    size_t i = 2;
    while (i + 4 < size) {
        if (data[i] != 0xFF) {
            ++i;
            continue;
        }
        uint8_t marker = data[i + 1];
        if (marker == 0xDA || marker == 0xD9) break;
        if (i + 4 > size) break;
        uint16_t segLen = detail::readBe16(data + i + 2);
        if (segLen < 2 || i + 2 + segLen > size) break;
        if (marker == 0xE1 && segLen >= 8) {
            const uint8_t* exif = data + i + 4;
            size_t exifLen = segLen - 2;
            if (exifLen >= 6 && std::memcmp(exif, "Exif\0\0", 6) == 0) {
                const uint8_t* tiff = exif + 6;
                size_t tiffSize = exifLen - 6;
                if (tiffSize < 8) break;
                bool le = tiff[0] == 'I' && tiff[1] == 'I';
                bool be = tiff[0] == 'M' && tiff[1] == 'M';
                if (!le && !be) break;
                uint32_t ifd0 = le ? detail::readLe32(tiff + 4) : detail::readBe32(tiff + 4);
                int64_t t = detail::ifdTagUnix(tiff, tiffSize, ifd0, le, 0x9003);
                if (t > 0) return t;
                t = detail::ifdTagUnix(tiff, tiffSize, ifd0, le, 0x0132);
                if (t > 0) return t;
            }
        }
        i += 2 + segLen;
    }
    return 0;
}

} // namespace carver
} // namespace byteback
