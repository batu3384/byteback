#include "imager/ewf_reader.h"
#include "imager/ewf_writer.h"
#include "byteback_io.h"
#include "crypto/byteback_md5.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <vector>

using namespace byteback;
using byteback::crypto::md5Hex;

TEST(EwfReader, RoundTripsWriterImageViaDiskReader) {
    const char* path = "reader_roundtrip.E01";
    ::remove(path);

    const uint64_t kSectors = 32;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(path, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), img.size());
    EXPECT_EQ(reader.getSectorSize(), 512u);

    std::vector<uint8_t> out(img.size());
    auto rr = reader.readSectors(0, static_cast<uint32_t>(out.size()), out.data());
    ASSERT_TRUE(rr.success) << rr.error;
    EXPECT_EQ(std::memcmp(out.data(), img.data(), img.size()), 0);

    EwfReader direct;
    ASSERT_TRUE(direct.open(path, err));
    EXPECT_EQ(direct.md5Hex(), w.md5Hex());

    ::remove(path);
}

TEST(EwfReader, MultiSegmentLinearRead) {
    const char* p1 = "reader_seg.E01";
    const char* p2 = "reader_seg.E02";
    ::remove(p1);
    ::remove(p2);

    EwfOptions opts;
    opts.sectorsPerChunk = 1;
    EwfWriter w;
    w.setMaxSectorsSectionBytes(8 * 512);
    const uint64_t kSectors = 16;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    ASSERT_TRUE(w.open(p1, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());
    EXPECT_EQ(w.segmentCount(), 2);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(p1, &err)) << err;
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(reader.readSectors(0, static_cast<uint32_t>(out.size()), out.data()).success);
    EXPECT_EQ(std::memcmp(out.data(), img.data(), img.size()), 0);

    ::remove(p1);
    ::remove(p2);
}

namespace {
// Writes `sectors` sectors of a counter pattern, then aborts mid-stream and
// checks every WRITTEN segment stays readable (no digest/done, headers patched).
void abortAndVerifyReadable(int expectedSegments, uint64_t writtenSectors) {
    const char* p1 = "abort_ewf.E01";
    const char* p2 = "abort_ewf.E02";
    ::remove(p1);
    ::remove(p2);

    EwfOptions opts;
    opts.sectorsPerChunk = 1;
    EwfWriter w;
    w.setMaxSectorsSectionBytes(8 * 512); // 8 sectors per segment
    const uint64_t kSectors = 16;         // declared (full) image size
    ASSERT_TRUE(w.open(p1, kSectors, 512, opts));

    std::vector<uint8_t> img(static_cast<size_t>(writtenSectors) * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.abort());
    EXPECT_EQ(w.segmentCount(), expectedSegments);
    // An aborted acquisition must never present a verification digest.
    EXPECT_TRUE(w.md5Hex().empty());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(p1, err)) << err;
    EXPECT_TRUE(r.md5Hex().empty());
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_EQ(std::memcmp(out.data(), img.data(), img.size()), 0);
    // Reads past the written data must fail, not hand back garbage.
    std::vector<uint8_t> tail(512);
    EXPECT_FALSE(r.read(img.size(), tail.data(), tail.size(), err));

    ::remove(p1);
    ::remove(p2);
}
} // namespace

TEST(EwfAbort, SingleSegmentStaysReadable) {
    abortAndVerifyReadable(1, 3);
}

// 2+ segment rotation + abort: the second segment must be patched, present,
// and readable together with the first (regression for the abort() path).
TEST(EwfAbort, MultiSegmentStaysReadable) {
    abortAndVerifyReadable(2, 12);
}

TEST(DiskReader, RawFileBackend) {
    const char* path = "reader_raw.dd";
    std::vector<uint8_t> img(2048, 0xAB);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachRawFile(path, &err)) << err;
    std::vector<uint8_t> out(512);
    ASSERT_TRUE(reader.readSectors(512, 512, out.data()).success);
    for (uint8_t b : out) EXPECT_EQ(b, 0xAB);

    ::remove(path);
}

// CA-039: the stored MD5 was parsed but never checked. A complete round-trip
// image must verify; a single flipped byte in the container must fail the
// digest check.
TEST(EwfReader, VerifyDigestTrueOnRoundTripFalseOnCorruption) {
    const char* path = "reader_verify.E01";
    ::remove(path);

    const uint64_t kSectors = 32;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);
    // Unique marker inside the image data, used to locate the bytes in the
    // container file for corruption.
    static const uint8_t marker[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44,
                                       0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    std::memcpy(img.data() + 64, marker, sizeof(marker));

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    {
        EwfReader r;
        std::string err;
        ASSERT_TRUE(r.open(path, err)) << err;
        EXPECT_FALSE(r.md5Hex().empty());
        EXPECT_TRUE(r.verifyDigest());
    }

    // Flip one byte of the image data inside the container.
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        std::vector<char> content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        in.close();
        ASSERT_FALSE(content.empty());
        // MSVC char is signed: compare as unsigned bytes or values >= 0x80
        // never match.
        const auto pos = std::search(content.begin(), content.end(),
                                     std::begin(marker), std::end(marker),
                                     [](char a, uint8_t b) {
                                         return static_cast<uint8_t>(a) == b;
                                     });
        ASSERT_NE(pos, content.end());
        *pos = static_cast<char>((*pos) ^ 0xFF);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
    }

    {
        EwfReader r;
        std::string err;
        ASSERT_TRUE(r.open(path, err)) << err;
        EXPECT_FALSE(r.verifyDigest());
    }

    ::remove(path);
}

// An aborted acquisition stores no digest; verification must report failure
// rather than pretending an unchecked image is verified.
TEST(EwfReader, VerifyDigestFalseWithoutDigestSection) {
    const char* p1 = "reader_verify_abort.E01";
    const char* p2 = "reader_verify_abort.E02";
    ::remove(p1);
    ::remove(p2);

    EwfOptions opts;
    opts.sectorsPerChunk = 1;
    EwfWriter w;
    w.setMaxSectorsSectionBytes(8 * 512);
    const uint64_t kSectors = 16;
    ASSERT_TRUE(w.open(p1, kSectors, 512, opts));
    std::vector<uint8_t> img(kSectors * 512, 0x5A);
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.abort());
    ASSERT_TRUE(w.md5Hex().empty());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(p1, err)) << err;
    EXPECT_FALSE(r.verifyDigest());

    ::remove(p1);
    ::remove(p2);
}

// ---------------------------------------------------------------------------
// C3/C4: compressed-chunk images.
// ---------------------------------------------------------------------------

namespace {
// Deterministic pseudo-random image (incompressible-ish) for round trips.
std::vector<uint8_t> pseudoRandomImage(size_t bytes, uint32_t seed) {
    std::vector<uint8_t> img(bytes);
    for (auto& b : img) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    return img;
}

// Mirror of the writer's segment naming (EwfWriter::segmentPathFor) so tests
// can clean up every rotated file.
std::string segmentPath(const std::string& base, int number) {
    std::string b = base;
    const size_t slash = b.find_last_of("\\/");
    const size_t dot = b.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        b = b.substr(0, dot);
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), ".E%02d", number);
    return b + buf;
}
std::string segmentPath(int number) { return segmentPath("reader_comp_seg.E01", number); }
} // namespace

// Byte-exact round trip through both EwfReader and DiskReader (attachEwfImage)
// at levels 1 and 6, with unaligned reads crossing chunk boundaries.
TEST(EwfReaderCompressed, RoundTripsByteExactLevels1And6) {
    const uint64_t kSectors = 200; // 100 KiB; 128-sector chunks -> partial final chunk
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 0xC0FFEE);
    // Sprinkle a compressible region so level deltas are visible too.
    std::memset(img.data() + 4096, 0x11, 32 * 1024);

    for (uint32_t level : {1u, 6u, 9u}) {
        const std::string path = "reader_comp_l" + std::to_string(level) + ".E01";
        ::remove(path.c_str());
        EwfOptions opts;
        opts.compression = level;
        EwfWriter w;
        ASSERT_TRUE(w.open(path, kSectors, 512, opts)) << level;
        // Feed in odd sector-multiples to exercise the chunk staging.
        ASSERT_TRUE(w.write(img.data(), 33 * 512)) << level;
        ASSERT_TRUE(w.write(img.data() + 33 * 512, img.size() - 33 * 512)) << level;
        ASSERT_TRUE(w.finish()) << level;
        EXPECT_EQ(w.md5Hex(), md5Hex(img.data(), img.size())) << level;

        EwfReader r;
        std::string err;
        ASSERT_TRUE(r.open(path, err)) << err;
        EXPECT_TRUE(r.verifyDigest()) << level;

        // Full read.
        std::vector<uint8_t> out(img.size());
        ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
        EXPECT_EQ(out, img) << level;

        // Unaligned slice crossing the 64 KiB chunk boundary.
        constexpr uint64_t kChunk = 128u * 512u;
        std::vector<uint8_t> slice(4096);
        ASSERT_TRUE(r.read(kChunk - 100, slice.data(), slice.size(), err)) << err;
        EXPECT_EQ(0, std::memcmp(slice.data(), img.data() + kChunk - 100, slice.size())) << level;

        // Read past the end must still fail.
        EXPECT_FALSE(r.read(img.size() - 1, slice.data(), 2, err)) << level;
        ::remove(path.c_str());
    }
}

// Compressed images must also read through DiskReader (attachEwfImage) — the
// production scan path.
TEST(EwfReaderCompressed, ReadsThroughDiskReader) {
    const char* path = "reader_comp_diskreader.E01";
    ::remove(path);
    const uint64_t kSectors = 64;
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 42);

    EwfOptions opts;
    opts.compression = 1;
    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(path, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), img.size());
    std::vector<uint8_t> out(img.size());
    auto res = reader.readSectors(0, static_cast<uint32_t>(out.size()), out.data());
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_EQ(out, img);
    reader.detachImageBackend();
    ::remove(path);
}

// Old uncompressed files must keep reading unchanged (interop with images
// written before the compression support).
TEST(EwfReaderCompressed, UncompressedFileStillReadsLegacy) {
    const char* path = "reader_legacy.E01";
    ::remove(path);
    const uint64_t kSectors = 32;
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512)); // default options: compression 0
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    EXPECT_TRUE(r.verifyDigest());
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_EQ(out, img);
    ::remove(path);
}

// Compressed multi-segment image: chunk tables per segment, linear read over
// the segment sequence. Random data keeps the compressed extents near the
// plaintext chunk size, so the small rotation ceiling actually triggers.
TEST(EwfReaderCompressed, MultiSegmentCompressedRoundTrip) {
    const std::string p1 = "reader_comp_seg.E01";
    for (int i = 1; i <= 9; ++i) ::remove(segmentPath(i).c_str());

    EwfOptions opts;
    opts.compression = 6;
    opts.sectorsPerChunk = 8;
    EwfWriter w;
    w.setMaxSectorsSectionBytes(12 * 512); // rotate after the first chunk
    const uint64_t kSectors = 40;
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 7);

    ASSERT_TRUE(w.open(p1, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());
    // Random data compresses to roughly its own size, so the small rotation
    // ceiling must produce multiple segments (>= 2; exact count depends on
    // the deflate output size near the ceiling).
    EXPECT_GE(w.segmentCount(), 2);
    EXPECT_EQ(w.md5Hex(), md5Hex(img.data(), img.size()));

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(p1, err)) << err;
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_EQ(out, img);
    EXPECT_TRUE(r.verifyDigest());
    for (int i = 1; i <= w.segmentCount(); ++i) {
        ::remove(segmentPath(i).c_str());
    }
}

// A1: EWF-backed readers clone by re-parsing the container — the clone reads
// the same image bytes through its own EwfReader/file handle while the
// original stays open.
TEST(EwfReaderCompressed, CloneReopensEwfImageIndependently) {
    const char* path = "reader_clone_ewf.E01";
    ::remove(path);
    const uint64_t kSectors = 64;
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 99);

    EwfOptions opts;
    opts.compression = 1;
    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEwfImage(path, &err)) << err;
    auto clone = reader.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_TRUE(reader.supportsParallelScan());

    std::vector<uint8_t> a(16 * 512), b(16 * 512);
    ASSERT_TRUE(reader.readSectors(0, 16 * 512, a.data()).success);
    ASSERT_TRUE(clone->readSectors(48 * 512, 16 * 512, b.data()).success);
    EXPECT_EQ(0, std::memcmp(a.data(), img.data(), a.size()));
    EXPECT_EQ(0, std::memcmp(b.data(), img.data() + 48 * 512, b.size()));
    reader.detachImageBackend();
    ::remove(path);
}

// ---------------------------------------------------------------------------
// D2/D3: the "error" section round-trips into acquiryErrors().
// ---------------------------------------------------------------------------
TEST(EwfReaderAcquiryErrors, ParsesErrorSectionEntries) {
    const char* path = "reader_err.E01";
    ::remove(path);
    const uint64_t kSectors = 8;
    std::vector<uint8_t> img(kSectors * 512, 0x44);

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    w.setAcquiryErrors({{1000, 4}, {2000, 1}});
    ASSERT_TRUE(w.finish());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    const auto& errs = r.acquiryErrors();
    ASSERT_EQ(errs.size(), 2u);
    EXPECT_EQ(errs[0].first, 1000u);
    EXPECT_EQ(errs[0].second, 4u);
    EXPECT_EQ(errs[1].first, 2000u);
    EXPECT_EQ(errs[1].second, 1u);
    ::remove(path);
}

// Images without an error section report an empty list.
TEST(EwfReaderAcquiryErrors, EmptyWhenNoErrorSection) {
    const char* path = "reader_noerr.E01";
    ::remove(path);
    EwfWriter w;
    ASSERT_TRUE(w.open(path, 8, 512));
    std::vector<uint8_t> img(8 * 512, 0);
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    EXPECT_TRUE(r.acquiryErrors().empty());
    ::remove(path);
}

namespace {
uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
// Locate a section by type in a single-segment container; returns the payload
// offset (after the 40-byte header) and payload size.
bool findSection(const std::vector<uint8_t>& f, const char* type,
                 uint64_t& dataOff, uint64_t& dataLen) {
    uint64_t off = 76;
    while (off + 40 <= f.size()) {
        char t[17] = {0};
        std::memcpy(t, f.data() + off, 16);
        const uint64_t size = rdU64(f.data() + off + 24);
        if (std::string(t) == type) {
            dataOff = off + 40;
            dataLen = size - 40;
            return true;
        }
        if (size < 40) return false;
        off += size;
    }
    return false;
}
} // namespace

// Hostile container: a compressed chunk whose deflate stream is garbage must
// fail the read cleanly (non-empty error, no crash, no hang) — and it must
// never present itself as verified.
TEST(EwfReaderHostile, CorruptDeflateChunkFailsCleanly) {
    const char* path = "hostile_badchunk.E01";
    ::remove(path);
    const uint64_t kSectors = 64;
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 31337);

    EwfOptions opts;
    opts.compression = 6;
    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    // Overwrite the first chunk extent with 0xFF bytes: block type 3 is
    // invalid per RFC 1951, so inflate must reject it outright.
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();
        uint64_t dataOff = 0, dataLen = 0;
        ASSERT_TRUE(findSection(f, "sectors", dataOff, dataLen));
        std::memset(f.data() + dataOff, 0xFF, 64);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
        ASSERT_TRUE(out.good());
    }

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err; // structure still parses
    std::vector<uint8_t> out(img.size());
    EXPECT_FALSE(r.read(0, out.data(), out.size(), err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(r.md5Hex().empty() == false); // container still carries a digest claim...
    EXPECT_FALSE(r.verifyDigest());           // ...but verification must refuse
    ::remove(path);
}

// Hostile error section: count field claims 0xFFFFFFFF entries while the
// payload holds one. The parser must bound by the payload size (no loop blow-
// up, no allocation driven by the count) and keep the image readable.
TEST(EwfReaderHostile, HugeErrorCountIsBoundedByPayload) {
    const char* path = "hostile_errcount.E01";
    ::remove(path);
    EwfWriter w;
    ASSERT_TRUE(w.open(path, 8, 512));
    std::vector<uint8_t> img(8 * 512, 0x11);
    ASSERT_TRUE(w.write(img.data(), img.size()));
    w.setAcquiryErrors({{5, 1}});
    ASSERT_TRUE(w.finish());

    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();
        uint64_t dataOff = 0, dataLen = 0;
        ASSERT_TRUE(findSection(f, "error", dataOff, dataLen));
        ASSERT_EQ(dataLen, 20u); // u32 count + one {u64,u64} entry
        f[dataOff + 0] = 0xFF;
        f[dataOff + 1] = 0xFF;
        f[dataOff + 2] = 0xFF;
        f[dataOff + 3] = 0xFF;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
        ASSERT_TRUE(out.good());
    }

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    EXPECT_TRUE(r.acquiryErrors().empty()); // count never trusted over payload
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_EQ(out, img);
    EXPECT_TRUE(r.verifyDigest());
    ::remove(path);
}

// A count=0 error section parses to an empty list and leaves the image intact.
TEST(EwfReaderHostile, ZeroCountErrorSectionParsesEmpty) {
    const char* path = "hostile_errzero.E01";
    ::remove(path);
    EwfWriter w;
    ASSERT_TRUE(w.open(path, 8, 512));
    std::vector<uint8_t> img(8 * 512, 0x22);
    ASSERT_TRUE(w.write(img.data(), img.size()));
    w.setAcquiryErrors({{1, 1}});
    ASSERT_TRUE(w.finish());

    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();
        uint64_t dataOff = 0, dataLen = 0;
        ASSERT_TRUE(findSection(f, "error", dataOff, dataLen));
        f[dataOff + 0] = 0; // count := 0
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
        ASSERT_TRUE(out.good());
    }

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    EXPECT_TRUE(r.acquiryErrors().empty());
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_TRUE(r.verifyDigest());
    ::remove(path);
}

// Compression and the acquiry-error section must coexist in one container:
// error entries round-trip, the digest stays over the plaintext, and the
// compressed chunks still verify.
TEST(EwfReaderHostile, ErrorSectionAndCompressionTogether) {
    const char* path = "hostile_err_comp.E01";
    ::remove(path);
    const uint64_t kSectors = 64;
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 2024);
    std::memset(img.data() + 8192, 0x00, 16384); // compressible region

    EwfOptions opts;
    opts.compression = 6;
    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    w.setAcquiryErrors({{3, 2}, {10, 1}});
    ASSERT_TRUE(w.finish());
    EXPECT_EQ(w.md5Hex(), md5Hex(img.data(), img.size()));

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    const auto& errs = r.acquiryErrors();
    ASSERT_EQ(errs.size(), 2u);
    EXPECT_EQ(errs[0].first, 3u);
    EXPECT_EQ(errs[0].second, 2u);
    EXPECT_EQ(errs[1].first, 10u);
    EXPECT_EQ(errs[1].second, 1u);
    std::vector<uint8_t> out(img.size());
    ASSERT_TRUE(r.read(0, out.data(), out.size(), err)) << err;
    EXPECT_EQ(out, img);
    EXPECT_TRUE(r.verifyDigest());
    ::remove(path);
}

// Backward seek must invalidate the single-chunk plaintext cache: reading a
// late chunk, then an early one, then a middle one must return exact bytes
// (stale-cache regression guard for the compressed path).
TEST(EwfReaderCompressed, BackwardSeekDoesNotServeStaleChunk) {
    const char* path = "reader_comp_seek.E01";
    ::remove(path);
    const uint64_t kSectors = 512; // 4 full 64 KiB chunks
    std::vector<uint8_t> img = pseudoRandomImage(static_cast<size_t>(kSectors) * 512, 777);

    EwfOptions opts;
    opts.compression = 9;
    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());

    EwfReader r;
    std::string err;
    ASSERT_TRUE(r.open(path, err)) << err;
    constexpr uint64_t kChunk = 128u * 512u;
    std::vector<uint8_t> a(kChunk), b(kChunk), c(kChunk - 123);
    ASSERT_TRUE(r.read(2 * kChunk, a.data(), a.size(), err)) << err; // chunk 2
    ASSERT_TRUE(r.read(0, b.data(), b.size(), err)) << err;          // backward to chunk 0
    // Unaligned read ending exactly at the image end (last chunk, stale cache
    // from the chunk-2 read must not be served).
    ASSERT_TRUE(r.read(3 * kChunk + 123, c.data(), c.size(), err)) << err;
    EXPECT_EQ(0, std::memcmp(a.data(), img.data() + 2 * kChunk, a.size()));
    EXPECT_EQ(0, std::memcmp(b.data(), img.data(), b.size()));
    EXPECT_EQ(0, std::memcmp(c.data(), img.data() + 3 * kChunk + 123, c.size()));
    EXPECT_TRUE(r.verifyDigest());
    ::remove(path);
}
