#include "search/hex_search.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <vector>

using namespace byteback;

TEST(HexSearch, FindsNeedleAtKnownOffset) {
    std::vector<uint8_t> img(4096, 0);
    const uint8_t needle[] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::memcpy(img.data() + 1000, needle, sizeof(needle));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);

    bool unread = false;
    std::atomic<bool> running{true};
    auto hits = searchRawBytes(reader, needle, sizeof(needle), 16, &running, &unread);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].byteOffset, 1000u);
    EXPECT_FALSE(unread);
}

TEST(HexSearch, UnreadWindowDoesNotInventZeroHit) {
    std::vector<uint8_t> img(4096, 0xAA);
    const uint8_t needle[] = {0, 0, 0, 0};
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    reader.setMemoryFaultRange(0, 2);

    bool unread = false;
    std::atomic<bool> running{true};
    auto hits = searchRawBytes(reader, needle, sizeof(needle), 16, &running, &unread);
    EXPECT_TRUE(unread);
    EXPECT_TRUE(hits.empty()) << "unread zeros must not look like a 00 00 00 00 match";
}
