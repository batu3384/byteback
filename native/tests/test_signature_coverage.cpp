#include "byteback_carver.h"
#include <gtest/gtest.h>

using namespace byteback;

TEST(SignatureCoverage, LoadsAtLeastOneHundredFortySignatures) {
    // 89 embedded/ftyp + 61 resource JSON after the CA-001/CA-002 cleanup:
    // the old "400+" set was padded with text magics and duplicate anchors
    // that could never win the same-offset dedup.
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));
    EXPECT_GE(carver.signatureCount(), 140u);
}
