#include "fs/ntfs_thumbcache.h"
#include <gtest/gtest.h>
#include <vector>

using namespace byteback;
using namespace byteback::ntfs;

TEST(ThumbcacheUtil, RecognizesThumbcacheDbName) {
    EXPECT_TRUE(isThumbcacheDbName("thumbcache_256.db"));
    EXPECT_TRUE(isThumbcacheDbName("Thumbcache_idx.db"));
    EXPECT_FALSE(isThumbcacheDbName("thumbs.db"));
    EXPECT_FALSE(isThumbcacheDbName("index.db"));
}

TEST(ThumbcacheUtil, EmitsEmbeddedJpegBlobs) {
    const uint8_t prefix[] = {'V', 'e', 'r', '5', 'F', 'i', 'l'};
    const uint8_t jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0xFF, 0xD9,
    };
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), prefix, prefix + sizeof(prefix));
    buf.insert(buf.end(), jpeg, jpeg + sizeof(jpeg));

    FileRecord parent{};
    parent.id = 42;
    parent.name = "thumbcache_32.db";
    parent.path = "C:\\Users\\x\\Explorer";

    int64_t nextId = 1;
    std::vector<FileRecord> found;
    emitEmbeddedJpegs(buf.data(), buf.size(), parent, nextId, [&](const FileRecord& fr) {
        found.push_back(fr);
    });

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].source, "ntfs_thumbcache");
    EXPECT_EQ(found[0].category, "Image");
    EXPECT_EQ(found[0].extension, ".jpg");
    EXPECT_EQ(found[0].sizeBytes, sizeof(jpeg));
    ASSERT_EQ(found[0].residentData.size(), sizeof(jpeg));
    EXPECT_EQ(found[0].residentData[0], 0xFF);
    EXPECT_EQ(found[0].residentData[1], 0xD8);
    EXPECT_FALSE(found[0].path.empty());
}

TEST(ThumbcacheUtil, SkipsJpegWithoutEoi) {
    const uint8_t soi[] = {0xFF, 0xD8, 0xFF, 0xE0};
    std::vector<uint8_t> buf(soi, soi + sizeof(soi));
    buf.insert(buf.end(), 64, 0x00);
    FileRecord parent{};
    parent.name = "thumbcache_32.db";
    int64_t nextId = 1;
    std::vector<FileRecord> found;
    emitEmbeddedJpegs(buf.data(), buf.size(), parent, nextId, [&](const FileRecord& fr) {
        found.push_back(fr);
    });
    EXPECT_TRUE(found.empty());
}

TEST(ThumbcacheUtil, CapsEmbeddedJpegCount) {
    std::vector<uint8_t> buf;
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0xFF, 0xD9};
    for (int n = 0; n < 40; ++n) {
        buf.insert(buf.end(), jpeg, jpeg + sizeof(jpeg));
    }
    FileRecord parent{};
    parent.name = "thumbcache_32.db";
    int64_t nextId = 1;
    std::vector<FileRecord> found;
    emitEmbeddedJpegs(buf.data(), buf.size(), parent, nextId, [&](const FileRecord& fr) {
        found.push_back(fr);
    }, 24);
    EXPECT_EQ(found.size(), 24u);
}
