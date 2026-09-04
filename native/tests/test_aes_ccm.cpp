// AR2 math-audit regression: the previous AES-CCM used a nonsense flags byte
// (0x0D) and CBC-MAC'd the CIPHERTEXT — BitLocker VMK/FVEK unwrap could never
// succeed on real disks. These tests pin the RFC 3610 wire format and the
// decrypt-side MAC-over-plaintext semantics.
#include "crypto/byteback_aes_ccm.h"
#include "crypto/byteback_aes.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;
using namespace byteback::crypto;

namespace {

// BitLocker shape: AES-256 key, 12-byte nonce, 16-byte trailing tag.
const uint8_t kKey[32] = {
    0x10, 0x01, 0x12, 0x02, 0x14, 0x03, 0x16, 0x04, 0x18, 0x05, 0x1A, 0x06, 0x1C, 0x07, 0x1E, 0x08,
    0x20, 0x09, 0x22, 0x0A, 0x24, 0x0B, 0x26, 0x0C, 0x28, 0x0D, 0x2A, 0x0E, 0x2C, 0x0F, 0x2E, 0x00,
};
const uint8_t kNonce[12] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB};

} // namespace

TEST(AesCcm, B0FlagsFollowRfc3610ForBitLockerShape) {
    // L = 15 - 12 = 3; M = 16 -> B0 flags = ((16-2)/2)<<3 | (L-1) = 0x38|0x02 = 0x3A.
    uint8_t b0[16];
    ccmFormatB0(b0, kNonce, 12, 16, 0x123456);
    EXPECT_EQ(b0[0], 0x3A);
    EXPECT_EQ(std::memcmp(b0 + 1, kNonce, 12), 0);
    // 3-byte big-endian length in the trailing bytes (1 flag + 12 nonce + 3 len = 16).
    EXPECT_EQ(b0[13], 0x12);
    EXPECT_EQ(b0[14], 0x34);
    EXPECT_EQ(b0[15], 0x56);
}

TEST(AesCcm, CounterFlagsAreLMinus1AndStartAtOne) {
    uint8_t ctr[16];
    ccmFormatCounter(ctr, kNonce, 12, 1);
    EXPECT_EQ(ctr[0], 0x02); // L-1 = 2
    EXPECT_EQ(std::memcmp(ctr + 1, kNonce, 12), 0);
    EXPECT_EQ(ctr[13], 0);
    EXPECT_EQ(ctr[14], 0);
    EXPECT_EQ(ctr[15], 1);
    ccmFormatCounter(ctr, kNonce, 12, 0x010203);
    EXPECT_EQ(ctr[13], 0x01);
    EXPECT_EQ(ctr[14], 0x02);
    EXPECT_EQ(ctr[15], 0x03);
}

TEST(AesCcm, EncryptDecryptRoundTrip) {
    const std::vector<uint8_t> pt = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                     0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                     0x11, 0x12, 0x13};
    std::vector<uint8_t> ct(pt.size() + 16, 0);
    ASSERT_TRUE(aesCcmEncrypt(kKey, kNonce, pt.data(), pt.size(), ct.data(), ct.size()));

    std::vector<uint8_t> back(pt.size(), 0);
    ASSERT_TRUE(aesCcmDecrypt(kKey, kNonce, ct.data(), ct.size(), back.data(), back.size()));
    EXPECT_EQ(back, pt);

    // Tamper with the tag (last 16 bytes): authentication must fail.
    ct[ct.size() - 1] ^= 0x01;
    EXPECT_FALSE(aesCcmDecrypt(kKey, kNonce, ct.data(), ct.size(), back.data(), back.size()));
    ct[ct.size() - 1] ^= 0x01;

    // Tamper with the payload: MAC-over-plaintext must reject.
    ct[0] ^= 0x80;
    EXPECT_FALSE(aesCcmDecrypt(kKey, kNonce, ct.data(), ct.size(), back.data(), back.size()));
    ct[0] ^= 0x80;

    // Restore fully: decrypts clean again.
    EXPECT_TRUE(aesCcmDecrypt(kKey, kNonce, ct.data(), ct.size(), back.data(), back.size()));
    EXPECT_EQ(back, pt);

    // Ciphertext differs from plaintext (real keystream, not a pass-through).
    EXPECT_NE(ct[0], pt[0]);
}

TEST(AesCcm, RejectsMalformedInputs) {
    uint8_t pt[8] = {}, ct[24] = {};
    EXPECT_FALSE(aesCcmDecrypt(kKey, kNonce, ct, 23, pt, 8));  // ct < 16+8
    EXPECT_FALSE(aesCcmDecrypt(kKey, kNonce, ct, 24, pt, 9));  // ptLen+16 != ctLen
    EXPECT_FALSE(aesCcmDecrypt(nullptr, kNonce, ct, 24, pt, 8));
}
