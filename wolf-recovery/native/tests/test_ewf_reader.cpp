#include "imager/ewf_reader.h"
#include "imager/ewf_writer.h"
#include "wolf_io.h"
#include "crypto/wolf_md5.h"

#include <gtest/gtest.h>
#include <cstring>
#include <fstream>
#include <vector>

using namespace wolf;

TEST(EwfReader, RoundTripsWriterImageViaDiskReader) {
    const char* path = "reader_roundtrip.E01";
    ::remove(path);

    const uint64_t kSectors = 32;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(path, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), img.size());
    EXPECT_EQ(reader.getSectorSize(), 512u);

    std::vector<uint8_t> out(img.size());
    auto rr = reader.readSectors(0, static_cast<uint32_t>(out.size()), out.data());
    ASSERT_TRUE(rr.success) << rr.error;
    EXPECT_EQ(std::memcmp(out.data(), img.data(), img.size()), 0);

    EwfReader direct;
    ASSERT_TRUE(direct.open(path, err));
    EXPECT_EQ(direct.md5Hex(), w.md5Hex());

    ::remove(path);
}

TEST(EwfReader, MultiSegmentLinearRead) {
    const char* p1 = "reader_seg.E01";
    const char* p2 = "reader_seg.E02";
    ::remove(p1);
    ::remove(p2);

    EwfOptions opts;
    opts.sectorsPerChunk = 1;
    EwfWriter w;
    w.setMaxSectorsSectionBytes(8 * 512);
    const uint64_t kSectors = 16;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    ASSERT_TRUE(w.open(p1, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());
    EXPECT_EQ(w.segmentCount(), 2);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(p1, &err)) << err;
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(reader.readSectors(0, static_cast<uint32_t>(out.size()), out.data()).success);
    EXPECT_EQ(std::memcmp(out.data(), img.data(), img.size()), 0);

    ::remove(p1);
    ::remove(p2);
}

TEST(DiskReader, RawFileBackend) {
    const char* path = "reader_raw.dd";
    std::vector<uint8_t> img(2048, 0xAB);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachRawFile(path, &err)) << err;
    std::vector<uint8_t> out(512);
    ASSERT_TRUE(reader.readSectors(512, 512, out.data()).success);
    for (uint8_t b : out) EXPECT_EQ(b, 0xAB);

    ::remove(path);
}
