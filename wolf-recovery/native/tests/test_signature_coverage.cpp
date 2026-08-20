#include "wolf_carver.h"
#include <gtest/gtest.h>

using namespace wolf;

TEST(SignatureCoverage, LoadsAtLeastTwoHundredSignatures) {
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));
    EXPECT_GE(carver.signatureCount(), 200u);
}
