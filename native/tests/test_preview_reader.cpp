#include "recovery/preview_reader.h"
#include "byteback_io.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

namespace {

std::vector<uint8_t> minimalJpeg() {
    std::vector<uint8_t> jpeg;
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD8);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDB);
    jpeg.push_back(0x00);
    jpeg.push_back(0x03);
    jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDA);
    jpeg.push_back(0x00);
    jpeg.push_back(0x02);
    for (int i = 0; i < 20; ++i) jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

} // namespace

TEST(PreviewReader, ResidentTextPreview) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "note.txt";
    rec.sizeBytes = 11;
    rec.residentData = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "text");
    EXPECT_EQ(preview.data.size(), 11u);
}

TEST(PreviewReader, ContiguousSectorImagePreview) {
    const auto jpeg = minimalJpeg();
    std::vector<uint8_t> img(512 * 4, 0);
    std::memcpy(img.data() + 512, jpeg.data(), jpeg.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "photo.jpg";
    rec.sizeBytes = static_cast<int64_t>(jpeg.size());
    rec.startSector = 1;

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.data.size(), jpeg.size());
}

TEST(PreviewReader, PrefixCapAt64KiB) {
    std::vector<uint8_t> payload(kPreviewMaxBytes + 1024, 0x41);
    DiskReader reader;
    FileRecord rec;
    rec.name = "big.bin";
    rec.sizeBytes = static_cast<int64_t>(payload.size());
    rec.residentData = std::move(payload);

    std::vector<uint8_t> out;
    ASSERT_TRUE(readRecordPrefix(reader, rec, out, kPreviewMaxBytes));
    EXPECT_EQ(out.size(), kPreviewMaxBytes);
}
