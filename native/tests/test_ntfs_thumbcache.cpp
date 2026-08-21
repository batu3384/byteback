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
    EXPECT_GT(found[0].sizeBytes, 0u);
    EXPECT_FALSE(found[0].path.empty());
}
