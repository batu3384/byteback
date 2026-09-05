#include "recovery/path_util.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace byteback;

TEST(PathUtil, SafeBasenameStripsTraversal) {
    EXPECT_EQ(safeBasename("C:\\evil\\..\\secret.txt"), "secret.txt");
    EXPECT_EQ(safeBasename("../../etc/passwd"), "passwd");
    EXPECT_EQ(safeBasename(""), "recovered_file.bin");
    EXPECT_EQ(safeBasename("."), "recovered_file.bin");
    EXPECT_EQ(safeBasename("con"), "con");
    EXPECT_EQ(safeBasename("bad:name?.txt"), "bad_name_.txt");
}

TEST(PathUtil, UniqueDestPathAppendsSuffix) {
    auto dir = std::filesystem::temp_directory_path() / "byteback_path_util_test";
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

TEST(PathUtil, DestDirRejectsDotDot) {
    EXPECT_FALSE(destDirIsSafe("C:\\out\\..\\Windows"));
    EXPECT_FALSE(destDirIsSafe("../evil"));
    EXPECT_TRUE(destDirIsSafe("C:\\cases\\byteback"));
    EXPECT_TRUE(destDirIsSafe("D:\\case..lab\\out"));
    EXPECT_FALSE(destDirIsSafe("C:\\Windows\\Temp"));
}

// P0-1: folder-tree recovery needs a traversal-proof relative dir extractor.
TEST(PathUtil, SafeRelativeDirStripsTraversalAndDrivePrefixes) {
    EXPECT_EQ(safeRelativeDir("/Users/bat/Documents"), "Users/bat/Documents");
    EXPECT_EQ(safeRelativeDir("C:\\Users\\bat\\Docs"), "Users/bat/Docs");
    EXPECT_EQ(safeRelativeDir("/a/../../b/c"), "a/b/c");
    EXPECT_EQ(safeRelativeDir(""), "");
    EXPECT_EQ(safeRelativeDir("/"), "");
    EXPECT_EQ(safeRelativeDir("/CON/x"), "CON_dir/x"); // reserved device name
    EXPECT_EQ(safeRelativeDir("/a<b/c:d"), "a_b/c_d"); // per-segment sanitize
    EXPECT_EQ(joinDestDir("D:\\out", "a/b"), (std::filesystem::path("D:\\out") / "a" / "b").lexically_normal().string());
    EXPECT_EQ(joinDestDir("D:\\out", ""), "D:\\out");
}
