#include "fs/volume_identity.h"
#include "crypto/byteback_aes.h"
#include "crypto/byteback_aes_ccm.h"
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
