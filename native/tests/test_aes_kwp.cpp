// RFC 3394 test vectors (Section 4 of the RFC).
#include "crypto/byteback_aes_kwp.h"
#include <gtest/gtest.h>
#include <vector>
#include <cstring>

using namespace byteback::crypto;

static std::vector<uint8_t> hx(const char* s) {
    std::vector<uint8_t> v;
    for (const char* p = s; p[0] && p[1]; p += 2)
        v.push_back(static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
    return v;
}

static void expectUnwrap(const char* kek, const char* wrapped, const char* plain) {
    auto k = hx(kek), w = hx(wrapped), p = hx(plain);
    std::vector<uint8_t> out(p.size());
    size_t n = 0;
    ASSERT_TRUE(aesKwpUnwrap(k.data(), k.size(), w.data(), w.size(), out.data(), out.size(), &n));
    EXPECT_EQ(n, p.size());
    EXPECT_EQ(out, p);
}

TEST(AesKwp, Rfc3394_128Kek128Data) {
    expectUnwrap("000102030405060708090a0b0c0d0e0f",
                 "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5",
                 "00112233445566778899aabbccddeeff");
}
TEST(AesKwp, Rfc3394_256Kek128Data) {
    expectUnwrap("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                 "64e8c3f9ce0f5ba263e9777905818a2a93c8191e7d6e8ae7",
                 "00112233445566778899aabbccddeeff");
}
TEST(AesKwp, Rfc3394_256Kek192Data) {
    expectUnwrap("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                 "a8f9bc1612c68b3ff6e6f4fbe30e71e4769c8b80a32cb8958cd5d17d6b254da1",
                 "00112233445566778899aabbccddeeff0001020304050607");
}
TEST(AesKwp, Rfc3394_256Kek256Data) {
    expectUnwrap("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                 "28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43bfb988b9b7a02dd21",
                 "00112233445566778899aabbccddeeff000102030405060708090a0b0c0d0e0f");
}
TEST(AesKwp, RoundTrip) {
    auto k = hx("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    auto p = hx("deadbeefcafe00112233445566778899aabbccddeeff00112233445566778899");
    std::vector<uint8_t> w(64), o(64);
    size_t wn = 0, on = 0;
    ASSERT_TRUE(aesKwpWrap(k.data(), k.size(), p.data(), p.size(), w.data(), w.size(), &wn));
    ASSERT_TRUE(aesKwpUnwrap(k.data(), k.size(), w.data(), wn, o.data(), o.size(), &on));
    EXPECT_EQ(on, p.size());
    EXPECT_TRUE(std::memcmp(o.data(), p.data(), p.size()) == 0);
}
TEST(AesKwp, WrongKekFailsIntegrity) {
    auto k = hx("000102030405060708090a0b0c0d0e0f");
    auto k2 = hx("ff0102030405060708090a0b0c0d0e0f");
    auto w = hx("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    std::vector<uint8_t> out(16);
    size_t n = 0;
    EXPECT_FALSE(aesKwpUnwrap(k2.data(), k2.size(), w.data(), w.size(), out.data(), out.size(), &n));
}
TEST(AesKwp, RejectsBadInput) {
    std::vector<uint8_t> out(16);
    size_t n = 0;
    auto k = hx("000102030405060708090a0b0c0d0e0f");
    auto w = hx("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    EXPECT_FALSE(aesKwpUnwrap(k.data(), 17, w.data(), w.size(), out.data(), out.size(), &n));
    EXPECT_FALSE(aesKwpUnwrap(k.data(), k.size(), w.data(), 20, out.data(), out.size(), &n));
}
