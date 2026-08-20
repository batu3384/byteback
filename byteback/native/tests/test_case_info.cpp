#include "byteback_db.h"

#include <gtest/gtest.h>
#include <cstdio>

using byteback::CaseInfo;
using byteback::MetadataStore;

class CaseInfoTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath_ = "test_case_info.db";
        std::remove(dbPath_.c_str());
        ASSERT_TRUE(store_.open(dbPath_));
    }
    void TearDown() override {
        store_.close();
        std::remove(dbPath_.c_str());
    }

    std::string dbPath_;
    MetadataStore store_;
};

TEST_F(CaseInfoTest, DefaultsEmpty) {
    CaseInfo c = store_.getCaseInfo();
    EXPECT_TRUE(c.caseNumber.empty());
    EXPECT_TRUE(c.investigator.empty());
    EXPECT_EQ(c.createdAt, 0);
}

TEST_F(CaseInfoTest, RoundTripPersists) {
    CaseInfo in;
    in.caseNumber = "2026-FORENSIC-001";
    in.investigator = "Examiner A";
    in.agency = "Cyber Unit";
    in.notes = "USB seizure";
    ASSERT_TRUE(store_.setCaseInfo(in));

    CaseInfo out = store_.getCaseInfo();
    EXPECT_EQ(out.caseNumber, in.caseNumber);
    EXPECT_EQ(out.investigator, in.investigator);
    EXPECT_EQ(out.agency, in.agency);
    EXPECT_EQ(out.notes, in.notes);
    EXPECT_GT(out.createdAt, 0);
    EXPECT_GT(out.updatedAt, 0);
    EXPECT_GE(out.updatedAt, out.createdAt);
}

TEST_F(CaseInfoTest, UpdatePreservesCreatedAt) {
    CaseInfo first;
    first.caseNumber = "CASE-A";
    ASSERT_TRUE(store_.setCaseInfo(first));
    int64_t created = store_.getCaseInfo().createdAt;

    CaseInfo second;
    second.caseNumber = "CASE-B";
    second.investigator = "B";
    ASSERT_TRUE(store_.setCaseInfo(second));

    CaseInfo out = store_.getCaseInfo();
    EXPECT_EQ(out.caseNumber, "CASE-B");
    EXPECT_EQ(out.createdAt, created);
    EXPECT_GE(out.updatedAt, created);
}
