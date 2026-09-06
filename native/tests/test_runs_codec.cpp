#include "db/runs_codec.h"
#include <gtest/gtest.h>
#include <limits>

using namespace byteback;

TEST(RunsCodec, EmptyRoundTrip) {
    EXPECT_EQ(serializeRuns({}), "");
    EXPECT_TRUE(deserializeRuns("").empty());
}

TEST(RunsCodec, SingleRunRoundTrip) {
    std::vector<FileRecord::DataRun> runs = {{100, 8}};
    auto json = serializeRuns(runs);
    EXPECT_EQ(json, "[[100,8]]");
    auto out = deserializeRuns(json);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].startSector, 100u);
    EXPECT_EQ(out[0].sectorCount, 8u);
}

TEST(RunsCodec, MultipleRunsRoundTrip) {
    std::vector<FileRecord::DataRun> runs = {{0, 1}, {4096, 16}, {UINT64_MAX / 4, 2}};
    auto json = serializeRuns(runs);
    auto out = deserializeRuns(json);
    ASSERT_EQ(out.size(), runs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
        EXPECT_EQ(out[i].startSector, runs[i].startSector);
        EXPECT_EQ(out[i].sectorCount, runs[i].sectorCount);
    }
}

TEST(RunsCodec, MalformedInputNeverThrows) {
    EXPECT_TRUE(deserializeRuns("not json").empty());
    EXPECT_TRUE(deserializeRuns("[[1,2").empty());
    EXPECT_TRUE(deserializeRuns("[[abc,2]]").empty());
    EXPECT_TRUE(deserializeRuns("[[1,]]").empty());
    EXPECT_TRUE(deserializeRuns("[[1,0]]").empty()); // zero-length run rejected
    EXPECT_TRUE(deserializeRuns("[[1,2],oops").empty());
}

// AR2 vector gap: the sparse sentinel is UINT64_MAX itself, not /4.
TEST(RunsCodec, SparseSentinelLiteralRoundTrip) {
    std::vector<FileRecord::DataRun> runs = {{18446744073709551615ull, 8}, {0, 1}};
    auto json = serializeRuns(runs);
    auto out = deserializeRuns(json);
    ASSERT_EQ(out.size(), runs.size());
    EXPECT_EQ(out[0].startSector, 18446744073709551615ull);
    EXPECT_EQ(out[0].sectorCount, 8u);
    EXPECT_EQ(out[1].startSector, 0u);
}

TEST(RunsCodec, OverflowRejected) {
    std::string huge = "[[999999999999999999999999999999,1]]";
    EXPECT_TRUE(deserializeRuns(huge).empty());
}

// Negative numbers are not u64 — minus must be rejected, not wrapped.
TEST(RunsCodec, NegativeNumbersRejected) {
    EXPECT_TRUE(deserializeRuns("[[-5,3]]").empty());
    EXPECT_TRUE(deserializeRuns("[[1,-3]]").empty());
    EXPECT_TRUE(deserializeRuns("[[-1,-1]]").empty());
}
