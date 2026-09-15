#include "byteback_smart.h"
#include <gtest/gtest.h>

using byteback::ataHealthFromDefects;
using byteback::smartNeedsTrimAck;
using byteback::SmartStatus;

TEST(AtaHealth, AcsDefectTriage) {
    EXPECT_STREQ(ataHealthFromDefects(0, 0), "Good");
    EXPECT_STREQ(ataHealthFromDefects(1, 0), "Warning");
    EXPECT_STREQ(ataHealthFromDefects(0, 1), "Warning");
    EXPECT_STREQ(ataHealthFromDefects(4, 2), "Bad");
    EXPECT_STREQ(ataHealthFromDefects(-1, -8), "Good");
}

TEST(AtaHealth, SmartNeedsTrimAckTreatsUnreadAsRisk) {
    SmartStatus unread;
    EXPECT_TRUE(smartNeedsTrimAck(unread));

    SmartStatus hdd;
    hdd.isValid = true;
    hdd.seekPenaltyKnown = true;
    hdd.isSsd = false;
    EXPECT_FALSE(smartNeedsTrimAck(hdd));

    SmartStatus ssd = hdd;
    ssd.isSsd = true;
    EXPECT_TRUE(smartNeedsTrimAck(ssd));

    SmartStatus noSeek = hdd;
    noSeek.seekPenaltyKnown = false;
    EXPECT_TRUE(smartNeedsTrimAck(noSeek));

    SmartStatus healthFail;
    healthFail.isValid = false;
    healthFail.seekPenaltyKnown = true;
    healthFail.isSsd = true;
    EXPECT_TRUE(smartNeedsTrimAck(healthFail));
}
