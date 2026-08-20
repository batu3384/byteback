#include "wolf_smart.h"
#include <gtest/gtest.h>

using wolf::ataHealthFromDefects;

TEST(AtaHealth, AcsDefectTriage) {
    EXPECT_STREQ(ataHealthFromDefects(0, 0), "Good");
    EXPECT_STREQ(ataHealthFromDefects(1, 0), "Warning");
    EXPECT_STREQ(ataHealthFromDefects(0, 1), "Warning");
    EXPECT_STREQ(ataHealthFromDefects(4, 2), "Bad");
    EXPECT_STREQ(ataHealthFromDefects(-1, -8), "Good");
}
