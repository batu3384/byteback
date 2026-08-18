#include "wolf_fs.h"
#include "wolf_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace wolf;

TEST(Ext4Parser, FindsNoteTxt) {
    auto img = wolf::testfix::buildExt4Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n == "note.txt") found = true;
    }
    EXPECT_TRUE(found);
}
