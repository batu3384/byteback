#include "recovery/path_util.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace wolf;

TEST(PathUtil, SafeBasenameStripsTraversal) {
    EXPECT_EQ(safeBasename("C:\\evil\\..\\secret.txt"), "secret.txt");
    EXPECT_EQ(safeBasename("../../etc/passwd"), "passwd");
    EXPECT_EQ(safeBasename(""), "recovered_file.bin");
    EXPECT_EQ(safeBasename("."), "recovered_file.bin");
    EXPECT_EQ(safeBasename("con"), "con");
    EXPECT_EQ(safeBasename("bad:name?.txt"), "bad_name_.txt");
}

TEST(PathUtil, UniqueDestPathAppendsSuffix) {
    auto dir = std::filesystem::temp_directory_path() / "wolf_path_util_test";
    std::filesystem::create_directories(dir);
    auto first = dir / "dup.bin";
    std::ofstream(first.string(), std::ios::binary).put('x');

    std::string p1 = uniqueDestPath(dir.string(), "dup.bin");
    EXPECT_EQ(p1, (dir / "dup_1.bin").string());
    std::ofstream(p1).put('x');

    std::string p2 = uniqueDestPath(dir.string(), "dup.bin");
    EXPECT_EQ(p2, (dir / "dup_2.bin").string());

    std::filesystem::remove_all(dir);
}
