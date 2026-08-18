#include "wolf_fs.h"
#include "wolf_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace wolf;

TEST(FatParser, ListsRootFileOnFat16Superfloppy) {
    auto img = wolf::testfix::buildFat16Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected TEST.TXT in FAT16 root";
}

TEST(FatParser, ScanAtPartitionOffset) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    auto disk = wolf::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    FATParser fat;
    ASSERT_TRUE(fat.scanAt(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running, 2048ull * 512));

    EXPECT_FALSE(names.empty());
}
