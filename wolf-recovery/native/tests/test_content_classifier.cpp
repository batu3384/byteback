#include "carver/content_classifier.h"
#include "math/entropy_calculator.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

using namespace wolf;

TEST(ContentClassifier, KeepsKnownCategory) {
    const uint8_t junk[] = {0, 1, 2, 3, 4, 5};
    EXPECT_EQ(refineCarveCategory(junk, sizeof(junk), "jpg", "Image"), "Image");
}

TEST(ContentClassifier, LowEntropyPrintableIsText) {
    std::string text(200, 'A');
    text += " quick brown fox\n";
    EXPECT_EQ(refineCarveCategory(reinterpret_cast<const uint8_t*>(text.data()), text.size(), "",
                                  "Unknown"),
              "Text");
}

TEST(ContentClassifier, HighEntropyMarkedEncrypted) {
    std::vector<uint8_t> rnd(512);
    for (size_t i = 0; i < rnd.size(); ++i) rnd[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFF);
    ASSERT_GE(math::calculateEntropy(rnd.data(), rnd.size()), 7.6);
    EXPECT_EQ(refineCarveCategory(rnd.data(), rnd.size(), "bin", "Unknown"), "Encrypted");
}

TEST(ContentClassifier, ExtensionFallbackDocument) {
    const char payload[] = "not really a pdf but ext hints";
    EXPECT_EQ(refineCarveCategory(reinterpret_cast<const uint8_t*>(payload), sizeof(payload) - 1,
                                  "pdf", "Unknown"),
              "Document");
}
