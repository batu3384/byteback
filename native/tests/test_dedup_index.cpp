#include "scan/dedup_index.h"
#include <gtest/gtest.h>

using namespace byteback;

TEST(DedupIndex, MarksOverlappingCarveAsDuplicate) {
    DedupIndex idx;
    FileRecord mft;
    mft.source = "ntfs_mft";
    mft.startSector = 100;
    mft.endSector = 108;
    mft.sizeBytes = 4096;
    mft.confidence = 90;
    mft.path = "/Users/photo.jpg";
    mft.name = "photo.jpg";
    idx.observe(mft);

    FileRecord carve;
    carve.source = "carver";
    carve.startSector = 102;
    carve.endSector = 110;
    carve.sizeBytes = 4096;
    carve.confidence = 80;
    carve.name = "carved_0_102.jpg";

    EXPECT_TRUE(idx.markDuplicate(carve));
    EXPECT_EQ(carve.source, "carver_duplicate");
    EXPECT_EQ(carve.path, "/dup_of/Users/photo.jpg");
}

TEST(DedupIndex, KeepsDistinctCarveWhenNoOverlap) {
    DedupIndex idx;
    FileRecord mft;
    mft.source = "fat";
    mft.startSector = 100;
    mft.endSector = 108;
    mft.confidence = 80;
    idx.observe(mft);

    FileRecord carve;
    carve.source = "carver";
    carve.startSector = 500;
    carve.endSector = 520;
    carve.confidence = 70;
    EXPECT_FALSE(idx.markDuplicate(carve));
    EXPECT_EQ(carve.source, "carver");
}

TEST(DedupIndex, HighConfidenceCarveStillDupOfMetadata) {
    DedupIndex idx;
    FileRecord mft;
    mft.source = "ntfs_mft";
    mft.startSector = 100;
    mft.endSector = 108;
    mft.sizeBytes = 4096;
    mft.confidence = 70;
    mft.path = "/Users/photo.jpg";
    mft.name = "photo.jpg";
    idx.observe(mft);

    FileRecord carve;
    carve.source = "carver";
    carve.startSector = 100;
    carve.endSector = 108;
    carve.sizeBytes = 4096;
    carve.confidence = 98; // inflated — must not beat metadata
    carve.name = "carved_0_100.jpg";

    EXPECT_TRUE(idx.markDuplicate(carve));
    EXPECT_EQ(carve.source, "carver_duplicate");
    EXPECT_LE(carve.confidence, 35);
}

TEST(DedupIndex, LoadFromRecordsHydratesResumeDedup) {
    DedupIndex idx;
    FileRecord mft;
    mft.source = "ntfs_mft";
    mft.startSector = 200;
    mft.endSector = 210;
    mft.sizeBytes = 4096;
    mft.confidence = 90;
    mft.path = "/Users/photo.jpg";
    mft.name = "photo.jpg";
    idx.loadFromRecords({mft});

    FileRecord carve;
    carve.source = "carver";
    carve.startSector = 205;
    carve.endSector = 212;
    carve.sizeBytes = 4096;
    carve.confidence = 75;
    EXPECT_TRUE(idx.markDuplicate(carve));
    EXPECT_EQ(carve.source, "carver_duplicate");
}
