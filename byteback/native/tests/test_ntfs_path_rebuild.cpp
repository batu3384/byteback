#include "fs/ntfs_path_rebuild.h"
#include <gtest/gtest.h>

using namespace byteback::ntfs;

TEST(NtfsPathRebuild, RebuildsNestedPath) {
    MftIndex idx;
    idx.putFileRecord(10, MftIndex::kRootMft, "Users", true, true);
    idx.putFileRecord(11, 10, "bar.jpg", false, false);

    std::string path = idx.rebuildPath(11, "bar.jpg", 10);
    EXPECT_EQ(path, "/Users/bar.jpg");
}

TEST(NtfsPathRebuild, IndxHintFillsMissingParent) {
    MftIndex idx;
    idx.putIndxHint(10, MftIndex::kRootMft, "Projects");
    std::string path = idx.rebuildPath(12, "note.txt", 10);
    EXPECT_EQ(path, "/Projects/note.txt");
}

TEST(NtfsPathRebuild, StopsAtRoot) {
    MftIndex idx;
    idx.putFileRecord(48, MftIndex::kRootMft, "boot.ini", false, false);
    std::string path = idx.rebuildPath(48, "boot.ini", MftIndex::kRootMft);
    EXPECT_EQ(path, "/boot.ini");
}
