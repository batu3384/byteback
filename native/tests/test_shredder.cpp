#include "byteback_shredder.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

using security::DataShredder;

namespace {
std::string makeTempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

TEST(Shredder, RefusesDirectory) {
    auto dir = std::filesystem::temp_directory_path() / "byteback_shred_dir_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec));
    DataShredder s;
    // A directory must never be "shredded" (an empty one would just be removed).
    EXPECT_FALSE(s.shred_file(dir.string()));
    EXPECT_TRUE(std::filesystem::exists(dir));
    std::filesystem::remove_all(dir, ec);
}

TEST(Shredder, ShredFileWipesAndRemoves) {
    auto p = makeTempPath("byteback_shred_file_test.bin");
    {
        std::ofstream f(p, std::ios::binary);
        std::string data(128 * 1024, 'X');
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    DataShredder s;
    EXPECT_TRUE(s.shred_file(p));
    EXPECT_FALSE(std::filesystem::exists(p));
}

TEST(Shredder, RefusesReadOnlyFile) {
    auto p = makeTempPath("byteback_shred_ro_test.bin");
    {
        std::ofstream f(p, std::ios::binary);
        f << "keep me";
    }
#ifdef _WIN32
    ASSERT_TRUE(SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_READONLY));
    DataShredder s;
    EXPECT_FALSE(s.shred_file(p));
    EXPECT_TRUE(std::filesystem::exists(p));
    // cleanup: clear the attribute so the file can be deleted
    SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(Shredder, FreeSpaceFillerIsRemoved) {
    auto dir = std::filesystem::temp_directory_path() / "byteback_wipe_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec));
    DataShredder s;
    ASSERT_TRUE(s.shred_free_space(dir.string(), 4096));
    EXPECT_FALSE(std::filesystem::exists(dir / ".byteback_freespace_wipe.tmp"));
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
