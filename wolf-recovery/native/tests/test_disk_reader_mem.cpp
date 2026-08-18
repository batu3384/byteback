#include "wolf_io.h"
#include <gtest/gtest.h>
#include <vector>
#include <cstring>

using namespace wolf;

TEST(DiskReaderMem, ReadAlignedSlice) {
    std::vector<uint8_t> img(512 * 4);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    DiskReader reader;
    reader.attachMemoryVolume(img, 512);
    EXPECT_TRUE(reader.isOpen());
    EXPECT_EQ(reader.getDiskSize(), 512u * 4);
    EXPECT_EQ(reader.getSectorSize(), 512u);

    uint8_t buf[512] = {};
    auto res = reader.readSectors(512, 512, buf);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.bytesRead, 512u);
    EXPECT_EQ(buf[0], static_cast<uint8_t>(512 & 0xFF));
}

TEST(DiskReaderMem, ShortReadZeroFillsAndRecordsBadSector) {
    std::vector<uint8_t> img(512);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    uint8_t buf[1024] = {0xFF};
    auto res = reader.readSectors(0, 1024, buf);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(buf[0], 0);
    EXPECT_EQ(buf[512], 0);
    EXPECT_GT(reader.getBadSectorReads(), 0u);
}

TEST(DiskReaderMem, RejectsUnalignedRead) {
    DiskReader reader;
    reader.attachMemoryVolume(std::vector<uint8_t>(1024));
    uint8_t buf[512];
    auto res = reader.readSectors(1, 512, buf);
    EXPECT_FALSE(res.success);
}
