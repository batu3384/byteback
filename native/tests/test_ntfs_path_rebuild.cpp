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

TEST(NtfsPathRebuild, IndxOnlyHintsAreWalkable) {
    MftIndex idx;
    idx.putFileRecord(10, MftIndex::kRootMft, "Users", true, true);
    idx.putIndxHint(99, 10, "gone.docx");
    int n = 0;
    std::string name;
    idx.forEachIndxOnly([&](uint64_t mft, const MftDirEntry& e) {
        ++n;
        name = e.name;
        EXPECT_EQ(mft, 99u);
    });
    EXPECT_EQ(n, 1);
    EXPECT_EQ(name, "gone.docx");
}

TEST(NtfsPathRebuild, StopsAtRoot) {
    MftIndex idx;
    idx.putFileRecord(48, MftIndex::kRootMft, "boot.ini", false, false);
    std::string path = idx.rebuildPath(48, "boot.ini", MftIndex::kRootMft);
    EXPECT_EQ(path, "/boot.ini");
}
