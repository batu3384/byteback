#include "fs/volume_identity.h"
#include "crypto/byteback_aes.h"
#include "crypto/byteback_aes_ccm.h"
#include "crypto/byteback_sha256.h"
#include "fs/bitlocker_fve.h"
#include "fs/bitlocker_unlock.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

TEST(VolumeIdentity, ParsesNtfsSerialAt0x48) {
    std::vector<uint8_t> boot(512, 0);
    std::memcpy(boot.data() + 3, "NTFS    ", 8);
    boot[0x48] = 0xEF;
    boot[0x49] = 0xBE;
    boot[0x4A] = 0xAD;
    boot[0x4B] = 0xDE;
    EXPECT_EQ(parseVolumeSerial(boot.data(), boot.size()), 0xDEADBEEFull);
}

TEST(VolumeIdentity, CollectsSerialFromMemoryNtfs) {
    std::vector<uint8_t> img(512 * 8, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x48] = 0x11;
    img[0x49] = 0x22;
    img[0x4A] = 0x33;
    img[0x4B] = 0x44;
    img[510] = 0x55;
    img[511] = 0xAA;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    auto serials = collectVolumeSerials(reader);
    EXPECT_TRUE(serials.count(0x44332211ull) == 1);
}

TEST(Aes128, Fips197EcbOneBlock) {
    // FIPS-197 C.1 AES-128: key 000102...0f, pt 001122...ff
    uint8_t key[16], pt[16], ct[16];
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<uint8_t>(i);
        pt[i] = static_cast<uint8_t>(i * 0x11);
    }
    byteback::crypto::aes128EncryptBlock(key, pt, ct);
    const uint8_t expect[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };
    EXPECT_EQ(std::memcmp(ct, expect, 16), 0);
}

TEST(XtsAes128, Ieee1619ZeroKeyZeroTweak) {
    uint8_t key[32] = {};
    uint8_t tweak[16] = {};
    uint8_t pt[16] = {};
    uint8_t ct[16] = {};
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, pt, ct, 16, true));
    const uint8_t expect[16] = {
        0x91,0x7c,0xf6,0x9e,0xbd,0x68,0xb2,0xec,0x9b,0x9f,0xe9,0xa3,0xea,0xdd,0xa6,0x92
    };
    EXPECT_EQ(std::memcmp(ct, expect, 16), 0);
}

TEST(XtsAes128, DecryptRoundTrip) {
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    uint8_t tweak[16] = {1};
    uint8_t pt[32];
    for (int i = 0; i < 32; ++i) pt[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t ct[32], back[32];
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, pt, ct, 32, true));
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, ct, back, 32, false));
    EXPECT_EQ(std::memcmp(pt, back, 32), 0);
}

namespace {
std::vector<uint8_t> fipsHex(const char* hex) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return static_cast<uint8_t>(c - 'A' + 10);
    };
    std::vector<uint8_t> out;
    for (const char* p = hex; *p && p[1]; p += 2) out.push_back(static_cast<uint8_t>(nibble(p[0]) * 16 + nibble(p[1])));
    return out;
}

// XTS per-block tweak advance (little-endian shift, 0x87 reduction) — an
// independent copy so the production loop is cross-checked, not trusted.
void gf128DoubleRef(uint8_t t[16]) {
    uint8_t carryIn = 0;
    for (int j = 0; j < 16; ++j) {
        uint8_t carryOut = static_cast<uint8_t>(t[j] >> 7);
        t[j] = static_cast<uint8_t>((t[j] << 1) | carryIn);
        carryIn = carryOut;
    }
    if (carryIn) t[0] ^= 0x87;
}
} // namespace

// AR2 vector gap: no FIPS-197 C.3 known answer existed for AES-256 (only the
// C.1 AES-128 vector), so a wrong key schedule could hide behind roundtrips.
TEST(Aes256, Fips197EcbOneBlock) {
    // FIPS-197 C.3 AES-256: key 603deb..., pt 6bc1bee2..., ct f3eed1bd...
    const auto key = fipsHex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const auto pt = fipsHex("6bc1bee22e409f96e93d7e117393172a");
    const auto expect = fipsHex("f3eed1bdb5d2a03c064b5a7e3db181f8");
    uint8_t ct[16], back[16];
    byteback::crypto::aes256EncryptBlock(key.data(), pt.data(), ct);
    EXPECT_EQ(std::memcmp(ct, expect.data(), 16), 0);
    byteback::crypto::aes256DecryptBlock(key.data(), ct, back);
    EXPECT_EQ(std::memcmp(back, pt.data(), 16), 0);
}

// AR2 vector gap: multi-block tweak chaining (GF(2^128) successive doubling
// and the K1/K2 key-half split) was only self-roundtrip tested. Recompute the
// expected ciphertext block-by-block with an independent tweak-chain copy.
TEST(XtsAes128, MultiBlockMatchesIndependentGf128Composition) {
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    uint8_t tweak[16] = {0x0F}; // nonzero data-unit tweak
    std::vector<uint8_t> pt(64);
    for (int i = 0; i < 64; ++i) pt[i] = static_cast<uint8_t>(0xC0 ^ i);
    std::vector<uint8_t> ct(64);
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, pt.data(), ct.data(), 64, true));

    // Independent composition: t = AES_K2(tweak); block i uses t advanced by
    // i doublings; C_i = AES_K1(P_i ^ t_i) ^ t_i.
    uint8_t t[16];
    byteback::crypto::aes128EncryptBlock(key + 16, tweak, t);
    for (int blk = 0; blk < 4; ++blk) {
        uint8_t x[16], c[16];
        for (int i = 0; i < 16; ++i) x[i] = static_cast<uint8_t>(pt[blk * 16 + i] ^ t[i]);
        byteback::crypto::aes128EncryptBlock(key, x, c);
        for (int i = 0; i < 16; ++i) {
            EXPECT_EQ(ct[blk * 16 + i], static_cast<uint8_t>(c[i] ^ t[i]))
                << "block " << blk << " byte " << i;
        }
        gf128DoubleRef(t);
    }

    std::vector<uint8_t> back(64);
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, ct.data(), back.data(), 64, false));
    EXPECT_EQ(back, pt);
}

TEST(DiskReaderXts, DecryptsMemorySectorWithFvek) {
    uint8_t key[32] = {};
    uint8_t tweak[16] = {};
    std::vector<uint8_t> pt(512, 0x3C);
    std::vector<uint8_t> ct(512);
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, pt.data(), ct.data(), 512, true));
    DiskReader reader;
    reader.attachMemoryVolume(ct);
    ASSERT_TRUE(reader.setXtsFvek128(key, 32));
    std::vector<uint8_t> out(512);
    auto res = reader.readSectors(0, 512, out.data());
    ASSERT_TRUE(res.success);
    EXPECT_EQ(std::memcmp(out.data(), pt.data(), 512), 0);
}

TEST(DiskReaderXts, CopyFvekDecryptsOtherReader) {
    uint8_t key[32] = {};
    uint8_t tweak[16] = {};
    std::vector<uint8_t> pt(512, 0x3C);
    std::vector<uint8_t> ct(512);
    ASSERT_TRUE(byteback::crypto::xtsAes128Crypt(key, tweak, pt.data(), ct.data(), 512, true));
    DiskReader src;
    src.attachMemoryVolume(ct);
    ASSERT_TRUE(src.setXtsFvek128(key, 32));
    DiskReader dest;
    dest.attachMemoryVolume(std::vector<uint8_t>(ct));
    dest.copyXtsFvekFrom(src);
    std::vector<uint8_t> out(512);
    auto res = dest.readSectors(0, 512, out.data());
    ASSERT_TRUE(res.success);
    EXPECT_EQ(std::memcmp(out.data(), pt.data(), 512), 0);
}

TEST(XtsAes256, DecryptRoundTrip) {
    uint8_t key[64];
    for (int i = 0; i < 64; ++i) key[i] = static_cast<uint8_t>(i);
    uint8_t tweak[16] = {2};
    uint8_t pt[32];
    for (int i = 0; i < 32; ++i) pt[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t ct[32], back[32];
    ASSERT_TRUE(byteback::crypto::xtsAes256Crypt(key, tweak, pt, ct, 32, true));
    ASSERT_TRUE(byteback::crypto::xtsAes256Crypt(key, tweak, ct, back, 32, false));
    EXPECT_EQ(std::memcmp(pt, back, 32), 0);
}

// Key-half contract for the key64 path (BitLocker FVEK / IEEE 1619): first
// 32 bytes = data key, second 32 bytes (offset 32) = tweak key. Mirrors the
// AES-128 multi-block composition test using AES-256 primitives.
TEST(XtsAes256, MultiBlockMatchesIndependentGf128Composition) {
    uint8_t key[64];
    for (int i = 0; i < 64; ++i) key[i] = static_cast<uint8_t>(i + 3);
    uint8_t tweak[16] = {0x2A};
    std::vector<uint8_t> pt(64);
    for (int i = 0; i < 64; ++i) pt[i] = static_cast<uint8_t>(0xC0 ^ i);
    std::vector<uint8_t> ct(64);
    ASSERT_TRUE(byteback::crypto::xtsAes256Crypt(key, tweak, pt.data(), ct.data(), 64, true));

    // Independent composition: t = AES_K2(tweak) with K2 = key[32..63];
    // block i uses t advanced by i doublings; C_i = AES_K1(P_i ^ t_i) ^ t_i.
    uint8_t t[16];
    byteback::crypto::aes256EncryptBlock(key + 32, tweak, t);
    for (int blk = 0; blk < 4; ++blk) {
        uint8_t x[16], c[16];
        for (int i = 0; i < 16; ++i) x[i] = static_cast<uint8_t>(pt[blk * 16 + i] ^ t[i]);
        byteback::crypto::aes256EncryptBlock(key, x, c);
        for (int i = 0; i < 16; ++i) {
            EXPECT_EQ(ct[blk * 16 + i], static_cast<uint8_t>(c[i] ^ t[i]))
                << "block " << blk << " byte " << i;
        }
        gf128DoubleRef(t);
    }

    std::vector<uint8_t> back(64);
    ASSERT_TRUE(byteback::crypto::xtsAes256Crypt(key, tweak, ct.data(), back.data(), 64, false));
    EXPECT_EQ(back, pt);
}

// XTS operates on whole 16-byte blocks only; a length that is not a multiple
// of 16 (and specifically a decrypt tail) must be rejected, not half-decrypted.
TEST(XtsLength, RejectsLengthNotMultipleOf16) {
    uint8_t key32[32] = {};
    uint8_t key64[64] = {};
    uint8_t tweak[16] = {};
    uint8_t in[33] = {};
    uint8_t out[33] = {};
    for (size_t len : {size_t(1), size_t(15), size_t(17), size_t(31), size_t(33)}) {
        EXPECT_FALSE(byteback::crypto::xtsAes128Crypt(key32, tweak, in, out, len, true)) << len;
        EXPECT_FALSE(byteback::crypto::xtsAes128Crypt(key32, tweak, in, out, len, false)) << len;
        EXPECT_FALSE(byteback::crypto::xtsAes256Crypt(key64, tweak, in, out, len, true)) << len;
        EXPECT_FALSE(byteback::crypto::xtsAes256Crypt(key64, tweak, in, out, len, false)) << len;
    }
    // len == 0 is a no-op success.
    EXPECT_TRUE(byteback::crypto::xtsAes128Crypt(key32, tweak, in, out, 0, true));
    EXPECT_TRUE(byteback::crypto::xtsAes256Crypt(key64, tweak, in, out, 0, false));
}

TEST(DiskReaderXts, Aes256FvekDecryptsSector) {
    uint8_t key[64] = {};
    uint8_t tweak[16] = {};
    std::vector<uint8_t> pt(512, 0x5A);
    std::vector<uint8_t> ct(512);
    ASSERT_TRUE(byteback::crypto::xtsAes256Crypt(key, tweak, pt.data(), ct.data(), 512, true));
    DiskReader reader;
    reader.attachMemoryVolume(ct);
    ASSERT_TRUE(reader.setXtsFvek(key, 64));
    std::vector<uint8_t> out(512);
    auto res = reader.readSectors(0, 512, out.data());
    ASSERT_TRUE(res.success);
    EXPECT_EQ(std::memcmp(out.data(), pt.data(), 512), 0);
}

TEST(BitLockerFve, ParsesMetadataMethodAndGuid) {
    std::vector<uint8_t> boot(512, 0);
    std::memcpy(boot.data() + 3, "-FVE-FS-", 8);
    boot[0xA0] = 0x00;
    boot[0xA1] = 0x10; // metadata at 0x1000

    std::vector<uint8_t> meta(128, 0);
    std::memcpy(meta.data(), "-FVE-FS-", 8);
    meta[64 + 0x10] = 0xAB;
    meta[64 + 0x24] = 6; // AES-256-XTS

    BitLockerFveInfo info;
    ASSERT_TRUE(parseBitLockerFve(boot.data(), boot.size(), meta.data(), meta.size(), info));
    EXPECT_TRUE(info.detected);
    EXPECT_EQ(info.metadataOffset, 0x1000u);
    EXPECT_EQ(info.encryptionMethod, 6u);
    EXPECT_EQ(info.encryptionName, "AES-256-XTS");
    EXPECT_EQ(info.volumeGuidHex.substr(0, 2), "ab");
}

TEST(BitLockerPassword, StretchKeyOpenwallVector) {
    const uint8_t salt[16] = {
        0x13, 0x4b, 0xd2, 0x63, 0x4b, 0xa5, 0x80, 0xad,
        0xc3, 0x75, 0x8c, 0xa5, 0xa8, 0x4d, 0x86, 0x66,
    };
    uint8_t key[32] = {};
    deriveBitLockerPasswordKey("openwall@123", salt, key);
    const uint8_t expect[32] = {
        0xa7, 0x02, 0xf4, 0x1c, 0xf1, 0x1b, 0x0b, 0x3e,
        0xb7, 0x3d, 0x52, 0xc0, 0x82, 0x4d, 0x12, 0x79,
        0xe1, 0x52, 0x61, 0x32, 0x26, 0x55, 0x13, 0xd8,
        0x03, 0xd6, 0x00, 0x4a, 0x96, 0xa8, 0x55, 0x8a,
    };
    EXPECT_EQ(std::memcmp(key, expect, 32), 0);
}

namespace {

// Reference stretch: SHA-256 over UTF-16LE digits, then 0x100000 rounds total.
void referenceStretch(const std::string& digits, uint8_t out[32]) {
    std::vector<uint8_t> wide;
    wide.reserve(digits.size() * 2);
    for (char c : digits) { wide.push_back(static_cast<uint8_t>(c)); wide.push_back(0); }
    crypto::sha256(wide.data(), wide.size(), out);
    for (uint64_t i = 1; i < 0x100000; ++i) crypto::sha256(out, 32, out);
}

} // namespace

TEST(BitLockerRecovery, StretchMatchesSpecIterationCount) {
    // Regression: the stretch loop ran 1'000'000 rounds instead of the
    // MS-BITLOCKER/libbde 0x100000; a real recovery key could never derive.
    const std::string digits = "123456234567345678456789567890678901789012890123";
    ASSERT_EQ(digits.size(), 48u);
    uint8_t got[32] = {}, expect[32] = {};
    stretchBitLockerRecoveryKey(digits, got);
    referenceStretch(digits, expect);
    EXPECT_EQ(std::memcmp(got, expect, 32), 0);
}

TEST(BitLockerRecovery, NormalizationStripsGroupSeparators) {
    const std::string grouped = "123456-234567 345678-456789 567890-678901 789012-890123";
    EXPECT_EQ(normalizeBitLockerRecoveryPassword(grouped).size(), 48u);
    uint8_t a[32] = {}, b[32] = {};
    stretchBitLockerRecoveryKey(normalizeBitLockerRecoveryPassword(grouped), a);
    stretchBitLockerRecoveryKey("123456234567345678456789567890678901789012890123", b);
    EXPECT_EQ(std::memcmp(a, b, 32), 0);
}

TEST(BitLockerRecovery, UnlockRejectsNon48DigitPasswordBeforeMetadata) {
    std::vector<uint8_t> img(512 * 64, 0); // no FVE metadata anywhere
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto shortResult = unlockBitLockerWithRecoveryPassword(reader, "1234");
    EXPECT_FALSE(shortResult.success);
    EXPECT_EQ(shortResult.error, "recovery password must be 48 digits");

    auto longResult = unlockBitLockerWithRecoveryPassword(reader,
        "123456-234567-345678-456789-567890-678901-789012-890123-123456");
    EXPECT_FALSE(longResult.success);
    EXPECT_EQ(longResult.error, "recovery password must be 48 digits");
}
