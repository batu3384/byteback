// Native unit tests for the RAID 6 GF(2^8) arithmetic (virtual_raid.cpp).
//
// The Q-syndrome math is what makes double-disk reconstruction possible; a
// wrong constant here silently corrupts every recovered byte. We verify the
// field axioms and the exact striping/recovery algebra against hand-computed
// vectors.
//
// Run with:
//   cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target byteback_tests
//   ctest --test-dir native/build --output-on-failure
#include "fs/virtual_raid.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using namespace byteback::raid6_math;

TEST(GfMath, IdentityAndZero) {
    // Multiplicative identity: a * 1 = a
    for (int a = 0; a < 256; ++a) {
        EXPECT_EQ(gfMul(static_cast<uint8_t>(a), 1), static_cast<uint8_t>(a));
        EXPECT_EQ(gfMul(1, static_cast<uint8_t>(a)), static_cast<uint8_t>(a));
        // Absorbing zero
        EXPECT_EQ(gfMul(static_cast<uint8_t>(a), 0), 0);
    }
}

TEST(GfMath, CommutativeAndAssociative) {
    // Spot-check a sample of the 256x256 table for the axioms.
    for (int a = 1; a < 256; a += 37) {
        for (int b = 1; b < 256; b += 41) {
            uint8_t x = static_cast<uint8_t>(a), y = static_cast<uint8_t>(b);
            EXPECT_EQ(gfMul(x, y), gfMul(y, x)); // commutative
            uint8_t z = static_cast<uint8_t>((a * 7 + b * 3) & 0xFF);
            EXPECT_EQ(gfMul(gfMul(x, y), z), gfMul(x, gfMul(y, z))); // associative
        }
    }
}

TEST(GfMath, GeneratorPowersMatchExpTable) {
    // g = 2. gfPow(e) must equal 2^e in the field for a sample of exponents.
    // Cross-check a few known values: 2^8 == 0x1D under poly 0x11D
    // (x^8 = x^4+x^3+x^2+1 mod poly).
    EXPECT_EQ(gfPow(0), 1);
    EXPECT_EQ(gfPow(1), 2);
    EXPECT_EQ(gfPow(8), 0x1D);
    EXPECT_EQ(gfPow(255), 1); // full cycle: g^255 = 1
    // Periodicity: g^(e+255) = g^e
    EXPECT_EQ(gfPow(100), gfPow(100 + 255));
}

TEST(GfMath, InverseAndDivisionRoundTrip) {
    // a * a^-1 = 1 and a / b * b = a for nonzero elements.
    for (int a = 1; a < 256; a += 11) {
        for (int b = 1; b < 256; b += 13) {
            uint8_t x = static_cast<uint8_t>(a), y = static_cast<uint8_t>(b);
            uint8_t q = gfDiv(x, y);
            EXPECT_EQ(gfMul(q, y), x); // (x/y)*y = x
        }
    }
}

TEST(GfMath, DivisionByZeroThrows) {
    EXPECT_THROW(gfDiv(5, 0), std::invalid_argument);
}

TEST(GfMath, StripeRecoveryAlgebra) {
    // Reproduce the exact 2-disk recovery computation used in read_raid6:
    //   P' = X_i ^ X_j              (XOR of the two lost blocks)
    //   Q' = X_i*g^i ^ X_j*g^j      (Q-syndrome of the two lost blocks)
    //   X_i = (P'*g^j ^ Q') / (g^i ^ g^j)
    //   X_j = P' ^ X_i
    // Pick slot indices and block bytes and verify we recover the originals.
    int si = 3, sj = 7;
    uint8_t xi = 0xA7, xj = 0x5C;

    uint8_t gi = gfPow(si), gj = gfPow(sj);
    uint8_t p = xi ^ xj;
    uint8_t q = gfMul(xi, gi) ^ gfMul(xj, gj);

    uint8_t denom = gi ^ gj;
    ASSERT_NE(denom, 0);

    uint8_t num = gfMul(gj, p) ^ q;
    uint8_t recoveredXi = gfDiv(num, denom);
    uint8_t recoveredXj = p ^ recoveredXi;

    EXPECT_EQ(recoveredXi, xi);
    EXPECT_EQ(recoveredXj, xj);
}

TEST(GfMath, StripeRecoveryAcrossManyVectors) {
    // Exhaustive-ish sweep over random-ish slots and bytes: every case must
    // round-trip. This is the property that matters for real reconstructions.
    int slots[] = {0, 1, 2, 5, 9, 14, 20};
    uint8_t vals[] = {0x00, 0x01, 0xFF, 0xA7, 0x5C, 0x93, 0x33};
    for (int si : slots) {
        for (int sj : slots) {
            if (si == sj) continue;
            for (uint8_t xi : vals) {
                for (uint8_t xj : vals) {
                    uint8_t gi = gfPow(si), gj = gfPow(sj);
                    uint8_t p = xi ^ xj;
                    uint8_t q = gfMul(xi, gi) ^ gfMul(xj, gj);
                    uint8_t denom = gi ^ gj;
                    ASSERT_NE(denom, 0);
                    uint8_t num = gfMul(gj, p) ^ q;
                    EXPECT_EQ(gfDiv(num, denom), xi);
                    EXPECT_EQ(p ^ gfDiv(num, denom), xj);
                }
            }
        }
    }
}
