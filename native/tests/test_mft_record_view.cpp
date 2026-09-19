#include "fs/mft_record_view.h"
#include "fixtures/volume_fixtures.h"
#include "byteback_io.h"
#include <gtest/gtest.h>

using namespace byteback;

TEST(MftRecordView, Record0FileInUseNonresidentData) {
    auto img = testfix::buildNtfsIndexAllocationVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool unread = false;
    auto view = getMftRecordView(reader, 0, 0, &unread);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->signature, "FILE");
    EXPECT_EQ(view->mftRef, 0u);
    EXPECT_EQ(view->byteOffset, 4096u);
    EXPECT_NE(view->flags & 0x01, 0);
    EXPECT_FALSE(unread);
    bool sawNonresidentData = false;
    for (const auto& a : view->attrs) {
        if (a.type == 0x80) {
            EXPECT_FALSE(a.resident);
            sawNonresidentData = true;
        }
    }
    EXPECT_TRUE(sawNonresidentData);
}

TEST(MftRecordView, UnreadMftDoesNotInventFile) {
    auto img = testfix::buildNtfsIndexAllocationVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(8, 2);

    bool unread = false;
    auto view = getMftRecordView(reader, 0, 0, &unread);
    EXPECT_TRUE(unread);
    EXPECT_FALSE(view.has_value()) << "unread MFT bytes must not parse as FILE";
}

TEST(MftRecordView, FollowsMftDataRunsForNonzeroRef) {
    using byteback::testfix::writeLe16;
    using byteback::testfix::writeLe32;
    using byteback::testfix::writeLe64;
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr size_t clusterBytes = ss * spc;
    std::vector<uint8_t> img(8 * clusterBytes, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = clusterBytes;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x02;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = 2 * clusterBytes + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "view.dat";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool unread = false;
    auto view = getMftRecordView(reader, 1, 0, &unread);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->signature, "FILE");
    EXPECT_EQ(view->mftRef, 1u);
    EXPECT_EQ(view->byteOffset, rec1);
    EXPECT_FALSE(unread);
    bool sawFn = false;
    for (const auto& a : view->attrs) {
        if (a.type == 0x30) sawFn = true;
    }
    EXPECT_TRUE(sawFn) << "MFT view must map ref 1 through $MFT data runs";
}
