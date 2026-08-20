#include "fs/refs_integrity.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace wolf;

TEST(RefsIntegrity, Crc32cKnownVector) {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(refsCrc32c(s, sizeof(s)), 0xE3069283u);
    EXPECT_EQ(refsCrc32c(nullptr, 0), 0u);
}

TEST(RefsIntegrity, Crc64EcmaKnownVector) {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(refsCrc64Ecma(s, sizeof(s)), 0x6C40DF5F0B497347ULL);
    EXPECT_EQ(refsCrc64Ecma(nullptr, 0), 0ULL);
}

TEST(RefsIntegrity, VerifyRangeExplicit) {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_TRUE(verifyRefsChecksum(RefsChecksumType::Crc64Ecma, s, sizeof(s), 0, sizeof(s),
                                   refsCrc64Ecma(s, sizeof(s))));
    EXPECT_FALSE(verifyRefsChecksum(RefsChecksumType::Crc64Ecma, s, sizeof(s), 0, sizeof(s), 1));
}

TEST(RefsIntegrity, MetadataPageSelfCheckSlot) {
    std::vector<uint8_t> page(64, 0);
    page[0] = 'S';
    page[1] = 'U';
    page[2] = 'P';
    page[3] = 'B';
    const uint64_t csum = refsCrc64Ecma(page.data(), 48);
    for (int i = 0; i < 8; ++i) page[48 + i] = static_cast<uint8_t>(csum >> (8 * i));

    int conf = 0;
    ASSERT_TRUE(tryVerifyRefsMetadataPage(page.data(), page.size(), conf));
    EXPECT_EQ(conf, 95);

    page[10] ^= 0xFF;
    ASSERT_TRUE(tryVerifyRefsMetadataPage(page.data(), page.size(), conf));
    EXPECT_EQ(conf, 35);
}
