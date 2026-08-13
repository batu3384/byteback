// Native unit tests for the entropy calculator.
//
// These mirror src/shared/entropy.test.ts on the TypeScript side so the two
// implementations of the same algorithm (renderer and native engine) are kept
// in lockstep. Run with:
//   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target wolf_tests
//   ctest --test-dir native/build --output-on-failure
#include "math/entropy_calculator.h"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

namespace {
double uniformEntropy() {
    // 100 occurrences of each of the 256 byte values => exactly 8.0 bits.
    std::vector<uint8_t> buf;
    buf.reserve(100 * 256);
    for (int rep = 0; rep < 100; ++rep)
        for (int v = 0; v < 256; ++v) buf.push_back(static_cast<uint8_t>(v));
    return wolf::math::calculateEntropy(buf.data(), buf.size());
}
} // namespace

TEST(Entropy, EmptyBufferReturnsZero) {
    EXPECT_DOUBLE_EQ(wolf::math::calculateEntropy(nullptr, 0), 0.0);
    std::vector<uint8_t> empty;
    EXPECT_DOUBLE_EQ(wolf::math::calculateEntropy(empty.data(), 0), 0.0);
}

TEST(Entropy, ConstantBufferReturnsZero) {
    std::vector<uint8_t> zeros(1024, 0);
    EXPECT_DOUBLE_EQ(wolf::math::calculateEntropy(zeros.data(), zeros.size()), 0.0);

    std::vector<uint8_t> fixed(512, 0x42);
    EXPECT_DOUBLE_EQ(wolf::math::calculateEntropy(fixed.data(), fixed.size()), 0.0);
}

TEST(Entropy, TwoSymbolUniformIsOneBit) {
    std::vector<uint8_t> buf;
    buf.reserve(1000);
    for (int i = 0; i < 1000; ++i) buf.push_back(i % 2 == 0 ? 0x00 : 0xff);
    EXPECT_NEAR(wolf::math::calculateEntropy(buf.data(), buf.size()), 1.0, 1e-6);
}

TEST(Entropy, UniformAcross256ValuesIsEightBits) {
    EXPECT_NEAR(uniformEntropy(), 8.0, 1e-6);
}

TEST(Entropy, IsBoundedInZeroEight) {
    // Pseudo-random but deterministic stream — should land high but never >8.
    std::vector<uint8_t> pseudo(4096);
    for (size_t i = 0; i < pseudo.size(); ++i) {
        pseudo[i] = static_cast<uint8_t>((i * 1103515245u + 12345u) & 0xff);
    }
    double e = wolf::math::calculateEntropy(pseudo.data(), pseudo.size());
    EXPECT_GE(e, 0.0);
    EXPECT_LE(e, 8.0);
}
