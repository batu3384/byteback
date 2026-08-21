#include "util/utf8_sanitize.h"
#include <gtest/gtest.h>
#include <string>

TEST(Utf8Sanitize, PassesValidUtf8) {
    EXPECT_EQ(byteback::utf8ForJs("photo.jpg"), "photo.jpg");
    EXPECT_EQ(byteback::utf8ForJs("fatura_öğün.pdf"), "fatura_öğün.pdf");
}

TEST(Utf8Sanitize, ReplacesIllegalBytes) {
    std::string bad = "a";
    bad.push_back(static_cast<char>(0xFF));
    bad += "b";
    EXPECT_EQ(byteback::utf8ForJs(bad), "a?b");
}

TEST(Utf8Sanitize, TruncatedSequenceBecomesQuestion) {
    std::string bad = "x";
    bad.push_back(static_cast<char>(0xC3)); // start of 2-byte, missing continuation
    EXPECT_EQ(byteback::utf8ForJs(bad), "x?");
}
