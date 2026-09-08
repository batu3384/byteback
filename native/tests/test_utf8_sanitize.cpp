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

// ---------------------------------------------------------------------------
// CA-055: utf8SanitizeLenientPreserving — byte-length-preserving strict
// validity for snippet offsets. Every invalid BYTE becomes exactly one '?'
// (no dropped sequences, no shrunk tails), and every kept multi-byte sequence
// is a real scalar-value encoding, so the renderer's UTF-8 -> UTF-16
// conversion cannot shift byte offsets.
// ---------------------------------------------------------------------------

namespace {

// Strict UTF-8 validator (scalar values only: no overlong, no surrogates,
// nothing above U+10FFFF) used to pin the output-validity invariant.
bool isValidStrictUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        size_t need = 0;
        unsigned char min2 = 0x80, max2 = 0xBF;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) min2 = 0xA0;
            else if (c == 0xED) max2 = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) min2 = 0x90;
            else if (c == 0xF4) max2 = 0x8F;
        } else {
            return false;
        }
        if (n - i < need + 1) return false;
        if (p[i + 1] < min2 || p[i + 1] > max2) return false;
        for (size_t k = 2; k <= need; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

} // namespace

TEST(Utf8SanitizeLenient, KeepsValidUtf8Verbatim) {
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving("photo.jpg"), "photo.jpg");
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving("fatura_\xC3\xB6\xC4\x9Fun.pdf"),
              "fatura_\xC3\xB6\xC4\x9Fun.pdf");
    // U+10FFFF — the highest scalar value, valid 4-byte encoding.
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving("\xF4\x8F\xBF\xBF"), "\xF4\x8F\xBF\xBF");
}

TEST(Utf8SanitizeLenient, ReplacesEveryInvalidByteWithExactlyOneQuestion) {
    std::string overlong2 = "\xC0\x81";     // overlong encoding of 'A'
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(overlong2), "??");
    std::string overlong3 = "\xE0\x80\x80"; // overlong 3-byte
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(overlong3), "???");
    std::string surrogate = "\xED\xA0\x80"; // UTF-16 surrogate U+D800
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(surrogate), "???");
    std::string beyond = "\xF4\x90\x80\x80"; // > U+10FFFF
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(beyond), "????");
    std::string loneCont;
    loneCont.push_back(static_cast<char>(0x81)); // stray continuation byte
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(loneCont), "?");
}

TEST(Utf8SanitizeLenient, TruncatedTailKeepsLength) {
    // utf8ForJs collapsed each of these tails into ONE '?' (breaking early),
    // shrinking the string — the length shift that moved snippet offsets.
    std::string a = "x\xC3"; // 2-byte lead, missing continuation
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(a), "x?");
    std::string b = "x\xE4\xB8"; // 3-byte lead + 1 continuation, cut
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(b), "x??");
    std::string c = "\xF0\x9F\x92"; // 4-byte lead + 2 continuations, cut
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(c), "???");
}

TEST(Utf8SanitizeLenient, ResyncsAfterInvalidLead) {
    // 'a', 0xE4 lead with a non-continuation 2nd byte, then ASCII "(", "b":
    // the failed sequence costs exactly one '?' and parsing resyncs.
    std::string s = "a\xE4(b";
    EXPECT_EQ(byteback::utf8SanitizeLenientPreserving(s), "a?(b");
}

TEST(Utf8SanitizeLenient, LengthPreservedAndValidOverEveryByteValue) {
    std::string all;
    for (int b = 0; b < 256; ++b) all.push_back(static_cast<char>(b));
    const std::string out = byteback::utf8SanitizeLenientPreserving(all);
    // Invariant 1: 1:1 byte length, whatever the input.
    ASSERT_EQ(out.size(), all.size());
    // Invariant 2: output is strictly valid UTF-8 — a JS-side conversion
    // produces no U+FFFD substitutions and moves no offsets.
    EXPECT_TRUE(isValidStrictUtf8(out));
}
