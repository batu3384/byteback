#include "byteback_io.h"
#include "recovery/path_util.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>

using namespace byteback;

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
    EXPECT_TRUE(res.paddedZeros);
    EXPECT_GT(reader.getBadSectorReads(), 0u);
}

TEST(DiskReaderMem, RejectsUnalignedRead) {
    DiskReader reader;
    reader.attachMemoryVolume(std::vector<uint8_t>(1024));
    uint8_t buf[512];
    auto res = reader.readSectors(1, 512, buf);
    EXPECT_FALSE(res.success);
}

TEST(DiskReaderMem, AttachEvidenceImageRejectsHttpAndDevice) {
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachEvidenceImage("http://host/disk.img", &err));
    EXPECT_FALSE(reader.attachEvidenceImage("\\\\.\\PhysicalDrive0", &err));
    EXPECT_FALSE(reader.attachEvidenceImage("\\\\.\\C:", &err));
}

TEST(DiskReaderMem, AttachEvidenceImageUtf8Path) {
    std::vector<uint8_t> img(512, 0x5A);
    auto dir = std::filesystem::temp_directory_path() /
               std::filesystem::u8path(u8"byteback_\u00fcye");
    dir += "_";
    dir += std::to_string(bytebackTestPid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto p = dir / std::filesystem::u8path(u8"kan\u0131t.img");
    {
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }
    {
        DiskReader reader;
        std::string err;
        ASSERT_TRUE(reader.attachEvidenceImage(pathToUtf8(p), &err)) << err;
        EXPECT_EQ(reader.getDiskSize(), img.size());
        uint8_t buf[512] = {};
        auto res = reader.readSectors(0, 512, buf);
        EXPECT_TRUE(res.success);
        EXPECT_EQ(buf[0], 0x5A);
    }
    std::filesystem::remove_all(dir);
}
