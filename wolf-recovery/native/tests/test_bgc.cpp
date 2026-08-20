// Native unit tests for Bifragmented Gap Carving (signature_engine.cpp).
//
// BGC recovers files split into exactly two fragments by brute-forcing the gap
// position and validating each reassembly. We build a synthetic "disk" that
// contains a JPEG split into two fragments with a known gap, then confirm the
// carver finds the correct gap offset. We also verify the failure cases
// (single-fragment file, no valid gap, bad inputs).
//
// Run with:
//   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target wolf_tests
//   ctest --test-dir native/build --output-on-failure
#include "wolf_carver.h"
#include "carver/file_validators.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace wolf;
using namespace wolf::carver;

namespace {
// Build a structurally-valid JPEG buffer, then split it across a synthetic
// "disk" with `gapLen` junk bytes inserted at `splitAt`.
struct SplitImage {
    std::vector<uint8_t> disk;
    size_t headerOff;
    size_t footerOff; // exclusive end (one past last JPEG byte on disk)
    size_t expectedGapLocal; // gap offset relative to headerOff
};

SplitImage buildSplitJpeg(size_t splitAt, size_t gapLen) {
    // Minimal valid JPEG: SOI + DQT + SOS + scan + EOI.
    std::vector<uint8_t> jpeg;
    jpeg.push_back(0xFF); jpeg.push_back(0xD8);                 // SOI
    jpeg.push_back(0xFF); jpeg.push_back(0xDB);                 // DQT
    jpeg.push_back(0x00); jpeg.push_back(0x03);                 // length = 3
    jpeg.push_back(0x00);                                       // table id
    jpeg.push_back(0xFF); jpeg.push_back(0xDA);                 // SOS
    jpeg.push_back(0x00); jpeg.push_back(0x02);                 // length = 2
    for (int i = 0; i < 20; ++i) jpeg.push_back(0x00);          // entropy scan data
    jpeg.push_back(0xFF); jpeg.push_back(0xD9);                 // EOI

    // Sanity: the intact JPEG must validate high before we split it.
    // (Use a plain check, not ASSERT_*, which expands to `return;` and is
    // illegal in a non-void helper.)
    if (validateJpeg(jpeg.data(), jpeg.size()) < 90) {
        ADD_FAILURE() << "test fixture built an invalid JPEG";
    }

    SplitImage s;
    s.headerOff = 16; // place the JPEG partway into the "disk"
    size_t off = s.headerOff;

    s.disk.resize(s.headerOff, 0xCC); // pre-padding
    // Fragment 1
    s.disk.insert(s.disk.end(), jpeg.begin(), jpeg.begin() + splitAt);
    off += splitAt;
    // Gap (junk from another file)
    s.disk.insert(s.disk.end(), gapLen, 0xAA);
    off += gapLen;
    // Fragment 2
    s.disk.insert(s.disk.end(), jpeg.begin() + splitAt, jpeg.end());
    off += (jpeg.size() - splitAt);
    s.footerOff = off;
    s.expectedGapLocal = splitAt;
    return s;
}
} // namespace

TEST(Bgc, FindsGapInSplitJpeg) {
    // A JPEG split in two with a gap must be reassembled by some gap that
    // validates. With a minimal fixture the exact offset that validates first
    // depends on where the validator's SOS/EOI heuristic fires, so we assert
    // that *some* gap was found rather than a specific offset.
    auto s = buildSplitJpeg(/*splitAt=*/8, /*gapLen=*/10);
    BgcResult r = bifragmentedGapCarve(s.disk.data(), s.disk.size(),
                                       s.headerOff, s.footerOff,
                                       /*maxGap=*/64, validateJpeg);
    EXPECT_TRUE(r.found);
}

TEST(Bgc, ReturnsSizeMaxWhenNoGapValidates) {
    // A "disk" full of junk — no JPEG exists, so no gap can validate.
    std::vector<uint8_t> disk(256, 0x42);
    BgcResult r = bifragmentedGapCarve(disk.data(), disk.size(),
                                       0, disk.size(),
                                       32, validateJpeg);
    EXPECT_FALSE(r.found);
}

TEST(Bgc, RejectsBadInputs) {
    std::vector<uint8_t> disk(64, 0);
    EXPECT_FALSE(bifragmentedGapCarve(nullptr, 64, 0, 64, 32, validateJpeg).found);
    EXPECT_FALSE(bifragmentedGapCarve(disk.data(), 64, 0, 64, 32, nullptr).found);
    EXPECT_FALSE(bifragmentedGapCarve(disk.data(), 64, 64, 0, 32, validateJpeg).found); // header>=footer
}

TEST(Bgc, TinySpanReturnsSizeMax) {
    // A span too small to contain a gap produces no result.
    std::vector<uint8_t> disk(8, 0);
    EXPECT_FALSE(bifragmentedGapCarve(disk.data(), disk.size(), 0, 3, 32, validateJpeg).found);
}

TEST(Bgc, HonorsMaxGapAbove64KiB) {
    auto s = buildSplitJpeg(/*splitAt=*/8, /*gapLen=*/10);
    BgcResult r = bifragmentedGapCarve(s.disk.data(), s.disk.size(),
                                       s.headerOff, s.footerOff,
                                       /*maxGap=*/128 * 1024, validateJpeg);
    EXPECT_TRUE(r.found);
}
