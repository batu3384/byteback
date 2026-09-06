#include "scan/dedup_index.h"
#include <gtest/gtest.h>

using namespace byteback;

namespace {
FileRecord makeRec(const char* source, uint64_t start, uint64_t end) {
    FileRecord r;
    r.source = source;
    r.startSector = start;
    r.endSector = end;
    r.confidence = 80;
    r.name = "rec";
    return r;
}
} // namespace

// CA-009 regression: the old query lower_bound'ed a startSector-sorted vector
// with an endSector comparator. With a long-span entry early in the vector the
// binary search could land past it and a carve living inside its span leaked
// through as a "new" file.
TEST(DedupIndex, LongSpanEntryIsFoundByMismatchedOrderQuery) {
    DedupIndex idx;
    idx.observe(makeRec("ntfs_mft", 0, 10));
    FileRecord longSpan = makeRec("ntfs_mft", 20, 200000);
    longSpan.path = "/Users/big.mov";
    longSpan.name = "big.mov";
    idx.observe(longSpan);
    idx.observe(makeRec("ntfs_mft", 30, 40));
    idx.observe(makeRec("ntfs_mft", 50, 60));

    FileRecord carve = makeRec("carver", 100, 110); // inside the long span
    EXPECT_TRUE(idx.markDuplicate(carve));
    EXPECT_EQ(carve.source, "carver_duplicate");
    EXPECT_EQ(carve.path, "/dup_of/Users/big.mov");
}

TEST(DedupIndex, CarverDuplicateMarksOverlappingCarveOfCarve) {
    DedupIndex idx;
    FileRecord first = makeRec("carver", 20, 200000);
    EXPECT_FALSE(idx.markDuplicate(first)); // registers the span

    FileRecord second = makeRec("carver", 100, 110); // inside the first span
    EXPECT_TRUE(idx.markDuplicate(second));
    EXPECT_EQ(second.source, "carver_duplicate");
}

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

TEST(DedupIndex, MarksOverlappingCarveAgainstPriorCarve) {
    DedupIndex idx;
    FileRecord first;
    first.source = "carver";
    first.startSector = 300;
    first.endSector = 310;
    first.sizeBytes = 5120;
    first.confidence = 80;
    first.name = "carved_a.jpg";
    EXPECT_FALSE(idx.markDuplicate(first));

    FileRecord second;
    second.source = "carver";
    second.startSector = 305;
    second.endSector = 315;
    second.sizeBytes = 5120;
    second.confidence = 75;
    second.name = "carved_b.jpg";
    EXPECT_TRUE(idx.markDuplicate(second));
    EXPECT_EQ(second.source, "carver_duplicate");
}

// P0-6: identical content at disjoint sectors is a duplicate even when the
// sector-overlap heuristics see nothing.
TEST(DedupIndex, SameContentHashAtDisjointSectorsIsDuplicate) {
    DedupIndex idx;
    FileRecord first = makeRec("carver", 1000, 1010);
    first.contentHash = "d41d8cd98f00b204e9800998ecf8427e";
    first.sizeBytes = 4096;
    EXPECT_FALSE(idx.markDuplicate(first));

    FileRecord second = makeRec("carver", 5000, 5010);
    second.contentHash = "d41d8cd98f00b204e9800998ecf8427e";
    second.sizeBytes = 4096;
    EXPECT_TRUE(idx.markDuplicate(second));
    EXPECT_EQ(second.source, "carver_duplicate");
    EXPECT_EQ(second.path, "/dup_of/content");
}

// Prefix-collision guard: the hash covers first min(64KB, size) bytes, so two
// files sharing a prefix (same EXIF header, zero-padded DB pages) but with
// DIFFERENT sizes are distinct — hash alone must not demote a real file.
TEST(DedupIndex, SameHashDifferentSizeIsNotDuplicate) {
    DedupIndex idx;
    FileRecord first = makeRec("carver", 1000, 1010);
    first.contentHash = "prefixcollision";
    first.sizeBytes = 65536;
    EXPECT_FALSE(idx.markDuplicate(first));

    FileRecord second = makeRec("carver", 5000, 5010);
    second.contentHash = "prefixcollision";
    second.sizeBytes = 900000; // same 64KB prefix, different real size
    EXPECT_FALSE(idx.markDuplicate(second));
    EXPECT_EQ(second.source, "carver");
}

// Resume (loadFromRecords) must rehydrate content hashes too — an identical
// payload re-carved after resume at disjoint sectors is still a duplicate.
TEST(DedupIndex, LoadFromRecordsRehydratesContentHashes) {
    DedupIndex idx;
    FileRecord persisted = makeRec("carver", 1000, 1010);
    persisted.contentHash = "d41d8cd98f00b204e9800998ecf8427e";
    idx.loadFromRecords({persisted});

    FileRecord again = makeRec("carver", 5000, 5010);
    again.contentHash = "d41d8cd98f00b204e9800998ecf8427e";
    EXPECT_TRUE(idx.markDuplicate(again));
    EXPECT_EQ(again.source, "carver_duplicate");
}
