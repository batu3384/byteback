#include "carver/embedded_metadata.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>

using namespace byteback::carver;

namespace {

std::vector<uint8_t> minimalJpegWithExif(const std::string& dateStr) {
    const uint32_t ifdOffset = 8;
    const uint32_t valueOffset = ifdOffset + 2 + 12 + 4;
    const size_t tiffSize = valueOffset + dateStr.size();
    const size_t exifPayload = 6 + tiffSize;
    const uint16_t segLen = static_cast<uint16_t>(exifPayload + 2);

    std::vector<uint8_t> buf = {0xFF, 0xD8, 0xFF, 0xE1,
                                static_cast<uint8_t>(segLen >> 8),
                                static_cast<uint8_t>(segLen & 0xFF),
                                0x45, 0x78, 0x69, 0x66, 0x00, 0x00,
                                0x49, 0x49, 0x2A, 0x00,
                                static_cast<uint8_t>(ifdOffset),
                                static_cast<uint8_t>(ifdOffset >> 8),
                                0x00, 0x00,
                                0x01, 0x00,
                                0x03, 0x90, 0x02, 0x00,
                                static_cast<uint8_t>(dateStr.size()),
                                0x00, 0x00, 0x00,
                                static_cast<uint8_t>(valueOffset),
                                static_cast<uint8_t>(valueOffset >> 8),
                                0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00};
    for (char c : dateStr) buf.push_back(static_cast<uint8_t>(c));
    buf.push_back(0xFF);
    buf.push_back(0xD9);
    return buf;
}

} // namespace

TEST(EmbeddedMetadata, ExtractsJpegExifDateTimeOriginal) {
    const auto jpeg = minimalJpegWithExif("2024:06:15 12:30:45\0");
    const int64_t unix = extractJpegExifUnix(jpeg.data(), jpeg.size());
    EXPECT_GT(unix, 0);
    // 2024-06-15 12:30:45 UTC
    EXPECT_EQ(unix, 1718454645);
}

TEST(EmbeddedMetadata, ReturnsZeroForNonJpeg) {
    const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47};
    EXPECT_EQ(extractJpegExifUnix(png, sizeof(png)), 0);
}
