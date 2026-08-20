// Native unit tests for the LZNT1 decompressor (ntfs_util.h).
//
// LZNT1 is the compression NTFS uses for compressed files. We verify the
// three structural cases an on-disk stream can present — uncompressed chunk,
// end-of-stream marker, and a compressed chunk with literal + back-reference
// tokens — plus malformed-input rejection. The back-reference case uses a
// hand-built compressed stream so we can assert exact output without depending
// on a Windows-produced fixture.
//
// Run with:
//   cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target byteback_tests
//   ctest --test-dir native/build --output-on-failure
#include "fs/ntfs_util.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

using byteback::ntfs::lznt1Decompress;

TEST(Lznt1, EmptyStreamProducesEmptyOutput) {
    uint8_t dst[16];
    // A stream that is just the end marker (0x0000) yields 0 bytes.
    const uint8_t end[] = {0x00, 0x00};
    EXPECT_EQ(lznt1Decompress(end, sizeof(end), dst, sizeof(dst)), 0);
}

TEST(Lznt1, NullBuffersRejected) {
    EXPECT_EQ(lznt1Decompress(nullptr, 4, nullptr, 4), -1);
}

TEST(Lznt1, UncompressedChunkIsLiteralCopy) {
    // Header: bit 15 clear (uncompressed), size field = 4 -> 4+1 = 5 bytes.
    // Payload: "HELLO"
    const uint8_t stream[] = {
        0x04, 0x00, // header: uncompressed, chunk size = 5
        'H', 'E', 'L', 'L', 'O',
        0x00, 0x00, // end marker
    };
    uint8_t dst[16] = {};
    int n = lznt1Decompress(stream, sizeof(stream), dst, sizeof(dst));
    ASSERT_EQ(n, 5);
    EXPECT_EQ(std::memcmp(dst, "HELLO", 5), 0);
}

TEST(Lznt1, CompressedChunkWithLiteralThenBackref) {
    // Goal: build a stream whose compressed chunk decompresses to "ABCABCABC"
    // (9 bytes). We emit one literal byte ('A'), then a back-reference token
    // that copies the rest.
    //
    // Chunk layout (bit 15 set = compressed):
    //   flag byte 0x01 -> token 0 is literal, token 1 is back-ref
    //   literal: 'A'
    //   back-ref token: we want to copy starting at displacement 1 (the 'A'),
    //     for length 8 (to produce "A" + "BCABCABC"... actually we'll produce
    //     a repeating pattern). To keep the math verifiable we instead aim for
    //     output "AAAAAAAA" (8 A's) using one literal + one back-ref of length 7
    //     at displacement 1.
    //
    // After emitting 1 literal byte, posInChunk = 1. The displacement field
    // width is determined by the smallest power of two strictly greater than
    // posInChunk (>= 16): u = 16, dispBits = 4, lenBits = 12.
    //   token layout: [displacement:4 bits high][length:12 bits low]
    //   displacement = 1 -> high nibble = displacement - 1 = 0
    //   length = 7 + 3 = 10 -> low 12 bits = 7
    //   token = (0 << 12) | 7 = 0x0007  (LE: 07 00)
    // Total decompressed: 1 (literal) + 10 (copy) = 11 bytes of 'A'.
    std::vector<uint8_t> stream;
    // Compressed chunk header: bit 15 set, size = payload length.
    // Payload = flag(1) + literal(1) + token(2) = 4 bytes.
    const uint8_t payload[] = {
        0x02,              // flag byte: bit0 CLEAR -> token0 literal; bit1 set -> token1 backref
        'A',               // literal
        0x07, 0x00,        // back-ref token LE: displacement=1, length=10
    };
    uint16_t header = 0x8000 | (static_cast<uint16_t>(sizeof(payload)) - 1);
    stream.push_back(header & 0xFF);
    stream.push_back(header >> 8);
    stream.insert(stream.end(), payload, payload + sizeof(payload));
    stream.push_back(0x00);
    stream.push_back(0x00); // end marker

    uint8_t dst[32] = {};
    int n = lznt1Decompress(stream.data(), stream.size(), dst, sizeof(dst));
    ASSERT_EQ(n, 11); // 1 literal + 10 copied
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(dst[i], 'A') << "byte " << i;
    }
}

TEST(Lznt1, TruncatedChunkRejected) {
    // Header claims 5 bytes of payload but only 2 follow.
    const uint8_t stream[] = {
        0x80 | 4, 0x00, // compressed chunk, size = 5
        'A', 'B',
    };
    uint8_t dst[16];
    EXPECT_EQ(lznt1Decompress(stream, sizeof(stream), dst, sizeof(dst)), -1);
}

TEST(Lznt1, BackrefBeforeChunkStartRejected) {
    // A back-reference whose displacement points before the chunk start is
    // invalid and must be rejected (guards against crafted streams that would
    // read unrelated memory).
    std::vector<uint8_t> stream;
    const uint8_t payload[] = {
        0x02,             // flag: token0 is back-ref
        0xFF, 0xFF,       // token: huge displacement + length
    };
    uint16_t header = 0x8000 | (static_cast<uint16_t>(sizeof(payload)) - 1);
    stream.push_back(header & 0xFF);
    stream.push_back(header >> 8);
    stream.insert(stream.end(), payload, payload + sizeof(payload));
    stream.push_back(0x00);
    stream.push_back(0x00);

    uint8_t dst[16];
    EXPECT_EQ(lznt1Decompress(stream.data(), stream.size(), dst, sizeof(dst)), -1);
}

TEST(Lznt1, OutputCappedAtCapacity) {
    // An uncompressed chunk larger than the destination must not overflow;
    // the decompressor returns the capped count (structural success).
    const uint8_t stream[] = {
        0x0F, 0x00, // uncompressed chunk, size = 16
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    };
    uint8_t dst[8] = {};
    int n = lznt1Decompress(stream, sizeof(stream), dst, sizeof(dst));
    // Position advances by the full chunk (16) but only 8 bytes were writable.
    EXPECT_EQ(n, 16);
    EXPECT_EQ(std::memcmp(dst, "ABCDEFGH", 8), 0);
}
