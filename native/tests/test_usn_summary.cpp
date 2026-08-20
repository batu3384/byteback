#include "byteback_db.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace byteback;

TEST(MetadataStore, ScanSummaryIncludesUsnCounts) {
    MetadataStore store;
    std::string path = (std::filesystem::temp_directory_path() / "byteback_usn_summary.db").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(store.open(path));

    int64_t scanId = store.createScan(0, "quick", 1000);
    TimelineEvent createEv;
    createEv.eventType = "create";
    createEv.fileName = "a.txt";
    createEv.timestamp = 100;
  TimelineEvent deleteEv;
    deleteEv.eventType = "delete";
    deleteEv.fileName = "a.txt";
    deleteEv.timestamp = 200;
    TimelineEvent renameEv;
    renameEv.eventType = "rename_new";
    renameEv.fileName = "b.txt";
    renameEv.timestamp = 300;

    store.insertTimelineEvent(scanId, createEv);
    store.insertTimelineEvent(scanId, deleteEv);
    store.insertTimelineEvent(scanId, renameEv);

    auto summary = store.getScanSummary(scanId);
    EXPECT_EQ(summary.timelineEvents, 3);
    EXPECT_EQ(summary.usnCreates, 1);
    EXPECT_EQ(summary.usnDeletes, 1);
    EXPECT_EQ(summary.usnRenames, 1);

    store.close();
    std::filesystem::remove(path);
}
