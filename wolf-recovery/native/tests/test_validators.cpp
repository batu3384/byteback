// Native unit tests for the Fast Object Validators (carver/file_validators.h).
//
// Each validator is fed a minimal hand-built "valid" buffer (just enough to
// satisfy the structural checks) and a few malformed buffers. The goal is to
// confirm that a structurally-sound candidate scores high while random /
// truncated / CRC-broken candidates are rejected or down-scored.
//
// Run with:
//   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target wolf_tests
//   ctest --test-dir native/build --output-on-failure
#include "carver/file_validators.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace wolf::carver;

namespace {
void appendU16Le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void appendU16Be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}
void appendU32Be(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((x >> (8 * i)) & 0xFF);
}
} // namespace

// ---------------- JPEG ----------------
TEST(JpegValidator, RejectsNonJpeg) {
    const uint8_t buf[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(validateJpeg(buf, sizeof(buf)), 0);
}

TEST(JpegValidator, ValidJpegWithEoiScoresHigh) {
    // SOI + DQT segment + SOS + EOI
    std::vector<uint8_t> b;
    b.push_back(0xFF); b.push_back(0xD8); // SOI
    // DQT marker, length 3 (minimal: length field itself + 1 byte)
    b.push_back(0xFF); b.push_back(0xDB); appendU16Be(b, 3); b.push_back(0x00);
    // SOS marker, length 2 (minimal)
    b.push_back(0xFF); b.push_back(0xDA); appendU16Be(b, 2);
    b.push_back(0x00); b.push_back(0x00); // entropy data
    b.push_back(0xFF); b.push_back(0xD9); // EOI
    EXPECT_GE(validateJpeg(b.data(), b.size()), 90);
}

TEST(JpegValidator, SoiOnlyScoresLow) {
    const uint8_t b[] = {0xFF, 0xD8, 0xFF, 0xC0};
    EXPECT_LT(validateJpeg(b, sizeof(b)), 60);
}

// ---------------- PNG ----------------
TEST(PngValidator, RejectsBadSignature) {
    const uint8_t b[] = {'P', 'N', 'G', 0, 0, 0, 0, 0};
    EXPECT_EQ(validatePng(b, sizeof(b)), 0);
}

TEST(PngValidator, ValidPngWithCrcScoresVeryHigh) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> b(SIG, SIG + 8);
    // IHDR chunk: 13 bytes of data (width/height/depth/etc)
    const char* type = "IHDR";
    uint32_t dataLen = 13;
    appendU32Be(b, dataLen);
    b.insert(b.end(), type, type + 4);
    size_t dataStart = b.size();
    b.insert(b.end(), 13, 0x00); // 13 zero bytes of IHDR data
    uint32_t crc = crc32(b.data() + dataStart - 4, 4 + 13); // type+data
    appendU32Be(b, crc);
    // IEND chunk
    appendU32Be(b, 0);
    static const uint8_t IEND_TYPE[4] = {'I', 'E', 'N', 'D'};
    b.insert(b.end(), IEND_TYPE, IEND_TYPE + 4);
    size_t iendDataStart = b.size();
    (void)iendDataStart;
    uint32_t iendCrc = crc32(b.data() + b.size() - 4, 4);
    appendU32Be(b, iendCrc);

    EXPECT_GE(validatePng(b.data(), b.size()), 95);
}

TEST(PngValidator, CrcMismatchScoresLow) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> b(SIG, SIG + 8);
    appendU32Be(b, 13);
    static const uint8_t IHDR_TYPE[4] = {'I', 'H', 'D', 'R'};
    b.insert(b.end(), IHDR_TYPE, IHDR_TYPE + 4);
    b.insert(b.end(), 13, 0x01); // data
    appendU32Be(b, 0xDEADBEEF); // deliberately wrong CRC
    EXPECT_LT(validatePng(b.data(), b.size()), 60);
}

// ---------------- ZIP ----------------
TEST(ZipValidator, RejectsNonZip) {
    const uint8_t b[] = {'Z', 'I', 'P', '!'};
    EXPECT_EQ(validateZip(b, sizeof(b)), 0);
}

TEST(ZipValidator, LocalHeaderOnlyIsWeak) {
    const uint8_t b[] = {'P', 'K', 0x03, 0x04, 0, 0};
    int score = validateZip(b, sizeof(b));
    EXPECT_GT(score, 0);
    EXPECT_LT(score, 50); // no central dir / EOCD
}

TEST(ZipValidator, FullArchiveWithEocdScoresHigh) {
    std::vector<uint8_t> b;
    static const uint8_t local[] = {'P', 'K', 0x03, 0x04};
    static const uint8_t central[] = {'P', 'K', 0x01, 0x02};
    static const uint8_t eocd[] = {'P', 'K', 0x05, 0x06};
    b.insert(b.end(), local, local + sizeof(local));
    b.insert(b.end(), central, central + sizeof(central));
    b.insert(b.end(), eocd, eocd + sizeof(eocd));
    EXPECT_GE(validateZip(b.data(), b.size()), 90);
}

// ---------------- PDF ----------------
TEST(PdfValidator, RejectsNonPdf) {
    const uint8_t b[] = {'D', 'O', 'C', '1'};
    EXPECT_EQ(validatePdf(b, sizeof(b)), 0);
}

TEST(PdfValidator, ValidPdfWithObjAndEof) {
    std::vector<uint8_t> b;
    const char* hdr = "%PDF-1.4\n";
    b.insert(b.end(), hdr, hdr + std::strlen(hdr));
    const char* obj = "1 0 obj\n<< >>\nendobj\n";
    b.insert(b.end(), obj, obj + std::strlen(obj));
    const char* eof = "%%EOF";
    b.insert(b.end(), eof, eof + std::strlen(eof));
    EXPECT_GE(validatePdf(b.data(), b.size()), 85);
}

TEST(PdfValidator, HeaderOnlyWithoutObjScoresLow) {
    const uint8_t b[] = {'%', 'P', 'D', 'F', '-', '1', '.', '4', 0, 0, 0, 0};
    EXPECT_LT(validatePdf(b, sizeof(b)), 60);
}

// ---------------- GZIP ----------------
TEST(GzipValidator, RejectsNonGzip) {
    const uint8_t b[] = {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(validateGzip(b, sizeof(b)), 0);
}

TEST(GzipValidator, MinimalDeflateHeaderScoresHigh) {
    // Minimal gzip: magic + deflate + no flags + mtime + xfl + os + body + footer
    std::vector<uint8_t> b = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xFF};
    // body (deflate stream) + 8-byte footer (crc32 + isize)
    b.insert(b.end(), 16, 0x00);
    EXPECT_GE(validateGzip(b.data(), b.size()), 80);
}

TEST(GzipValidator, BadCompressionMethodRejected) {
    std::vector<uint8_t> b = {0x1F, 0x8B, 0x09, 0x00, 0, 0, 0, 0, 0, 0xFF}; // method 9
    EXPECT_LT(validateGzip(b.data(), b.size()), 50);
}

// ---------------- CRC-32 primitive ----------------
TEST(Crc32, KnownVectors) {
    // CRC32 of "123456789" is 0xCBF43926.
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(crc32(s, sizeof(s)), 0xCBF43926u);
    // Empty input CRC is 0.
    EXPECT_EQ(crc32(nullptr, 0), 0u);
}

// ---------------- RIFF container (CA-006) ----------------
TEST(RiffValidator, SubtypeDetection) {
    // RIFF....WEBP
    std::vector<uint8_t> webp = {'R','I','F','F', 0x10,0,0,0, 'W','E','B','P'};
    EXPECT_STREQ(detectRiffSubtype(webp.data(), webp.size()), "webp");
    EXPECT_EQ(validateRiff(webp.data(), webp.size()), 90);

    std::vector<uint8_t> avi = {'R','I','F','F', 0,0,0,0, 'A','V','I',' '};
    EXPECT_STREQ(detectRiffSubtype(avi.data(), avi.size()), "avi");
    EXPECT_EQ(validateRiff(avi.data(), avi.size()), 90);

    std::vector<uint8_t> wav = {'R','I','F','F', 0,0,0,0, 'W','A','V','E'};
    EXPECT_STREQ(detectRiffSubtype(wav.data(), wav.size()), "wav");

    // Unknown subtype -> no detection, low structural score.
    std::vector<uint8_t> other = {'R','I','F','F', 0,0,0,0, 'X','X','X','X'};
    EXPECT_EQ(detectRiffSubtype(other.data(), other.size()), nullptr);
    EXPECT_EQ(validateRiff(other.data(), other.size()), 15);

    // Too short / not RIFF.
    EXPECT_EQ(detectRiffSubtype(webp.data(), 8), nullptr);
    EXPECT_EQ(validateRiff(webp.data(), 8), 0);
    EXPECT_EQ(validateRiff(other.data() + 4, 8), 0);
}

// ---------------- MPEG-TS ----------------
TEST(MpegTsValidator, RejectsShortOrMisalignedSync) {
    const uint8_t junk[] = {0x47, 0x40, 0x00, 0x01, 0x02};
    EXPECT_EQ(validateMpegTs(junk, sizeof(junk)), 0);
}

TEST(MpegTsValidator, AcceptsFiveAlignedPackets) {
    std::vector<uint8_t> buf(188 * 5, 0);
    for (size_t i = 0; i < 5; ++i) buf[i * 188] = 0x47;
    EXPECT_GE(validateMpegTs(buf.data(), buf.size()), 90);
}
