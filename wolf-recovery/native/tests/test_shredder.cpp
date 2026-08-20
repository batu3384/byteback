#include "wolf_shredder.h"
#include <gtest/gtest.h>
#include <filesystem>

using security::DataShredder;

TEST(Shredder, FreeSpaceFillerIsRemoved) {
    auto dir = std::filesystem::temp_directory_path() / "wolf_wipe_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec));
    DataShredder s;
    ASSERT_TRUE(s.shred_free_space(dir.string(), 4096));
    EXPECT_FALSE(std::filesystem::exists(dir / ".wolf_freespace_wipe.tmp"));
    std::filesystem::remove_all(dir, ec);
}

TEST(Shredder, RefusesDevicePath) {
    DataShredder s;
    EXPECT_FALSE(s.shred_free_space("\\\\.\\PhysicalDrive0", 4096));
}

TEST(Shredder, WipeSerialMustMatch) {
    EXPECT_FALSE(DataShredder::wipeSerialMatches("", "ABC"));
    EXPECT_FALSE(DataShredder::wipeSerialMatches("ABC", ""));
    EXPECT_FALSE(DataShredder::wipeSerialMatches("AAA", "BBB"));
    EXPECT_TRUE(DataShredder::wipeSerialMatches(" ab c ", "ABC"));
    DataShredder s;
    EXPECT_FALSE(s.shred_physical_drive(0, "NOPE", "REAL", 1024));
}
