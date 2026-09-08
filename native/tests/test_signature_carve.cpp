#include "byteback_carver.h"
#include "byteback_io.h"
#include "carver/file_validators.h"
#include "crypto/byteback_md5.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace byteback;

namespace {
std::string md5Of(const uint8_t* p, size_t n) {
    crypto::Md5 m;
    m.update(p, n);
    return m.finalHex();
}
}

// P0-6 content dedup (Lane E sweep): contentHash must exist for footer-matched
// carves of EVERY format — not only the bounded-parse probe list — and must
// cover exactly the file bytes: two identical objects at different offsets with
// different trailing junk must hash identically (no post-file padding, no
// intra-sector offset leakage).
TEST(SignatureCarve, ContentHashCoversOnlyFileBytes) {
    const auto png = byteback::testfix::buildMinimalValidPng();
    std::vector<uint8_t> disk(64 * 1024, 0);
    std::memcpy(disk.data() + 4100, png.data(), png.size()); // mid-sector start
    std::memset(disk.data() + 4100 + png.size(), 0xAA, 512);
    std::memcpy(disk.data() + 30000, png.data(), png.size());
    std::memset(disk.data() + 30000 + png.size(), 0x55, 512);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<FileRecord> pngs;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension == "png") pngs.push_back(fr);
    }, &running));

    ASSERT_EQ(pngs.size(), 2u);
    const std::string expected = md5Of(png.data(), png.size());
    for (const auto& fr : pngs) {
        EXPECT_FALSE(fr.contentHash.empty());
        EXPECT_EQ(fr.contentHash, expected);
    }
}

// Same invariant on the expire path, where the size arrives from a structural
// parser (7z StartHeader) long after the 1 MiB probe was filled with trailing
// junk: the hash must clamp to the refined actualSize.
TEST(SignatureCarve, ExpiredCarveHashClampedToRefinedSize) {
    auto build7z = [] {
        std::vector<uint8_t> sz(32 + 64 + 128, 0x00);
        static const uint8_t magic[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
        std::memcpy(sz.data(), magic, 6);
        const uint64_t nho = 64, nhs = 128;
        for (int i = 0; i < 8; ++i) sz[12 + i] = static_cast<uint8_t>(nho >> (8 * i));
        for (int i = 0; i < 8; ++i) sz[20 + i] = static_cast<uint8_t>(nhs >> (8 * i));
        return sz;
    };
    const auto sz = build7z();

    std::vector<uint8_t> disk(256 * 1024, 0);
    std::memcpy(disk.data() + 3000, sz.data(), sz.size());
    std::memset(disk.data() + 3000 + sz.size(), 0x77, 2048);
    std::memcpy(disk.data() + 100000, sz.data(), sz.size());
    std::memset(disk.data() + 100000 + sz.size(), 0x11, 2048);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<FileRecord> szs;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension == "7z") szs.push_back(fr);
    }, &running));

    ASSERT_EQ(szs.size(), 2u);
    ASSERT_EQ(szs[0].sizeBytes, sz.size());
    const std::string expected = md5Of(sz.data(), sz.size());
    for (const auto& fr : szs) EXPECT_EQ(fr.contentHash, expected);
}

TEST(SignatureCarve, FindsPngByHeaderFooter) {
    auto img = byteback::testfix::buildPngCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<std::string> exts;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) exts.push_back(fr.extension);
    }, &running));

    bool png = false;
    for (const auto& e : exts) {
        if (e.find("png") != std::string::npos) png = true;
    }
    EXPECT_TRUE(png);
}

TEST(SignatureCarve, RejectsFooterlessRiffWithoutSubtype) {
    std::vector<uint8_t> disk(4096, 0);
    disk[512] = 'R';
    disk[513] = 'I';
    disk[514] = 'F';
    disk[515] = 'F';

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<std::string> exts;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) exts.push_back(fr.extension);
    }, &running));

    for (const auto& e : exts) {
        EXPECT_NE(e, "riff");
        EXPECT_NE(e, "avi");
        EXPECT_NE(e, "webp");
        EXPECT_NE(e, "wav");
    }
}

TEST(SignatureCarve, RejectsFakeBmpAtExpire) {
    std::vector<uint8_t> disk(8192, 0);
    // Weak BM magic without valid BMP header fields — expire-path refineBmpCarve drops it.
    disk[4096] = 'B';
    disk[4097] = 'M';

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<std::string> exts;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) exts.push_back(fr.extension);
    }, &running));

    for (const auto& e : exts) {
        EXPECT_NE(e, "bmp");
    }
}

// BGC-rescued records are an emit site too: the P0-6 content-hash invariant
// applies. The hash covers the first sector-rounded chunk of the carved span
// (frag1 + gap prefix here), clamped to sizeBytes.
TEST(SignatureCarve, BgcRescuedCarveCarriesContentHash) {
    auto appendBe32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(static_cast<uint8_t>(x >> 24));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> c;
        appendBe32(c, static_cast<uint32_t>(data.size()));
        const size_t typeOff = c.size();
        c.insert(c.end(), type, type + 4);
        c.insert(c.end(), data.begin(), data.end());
        appendBe32(c, carver::crc32(c.data() + typeOff, 4 + data.size()));
        return c;
    };

    // frag1 (512B): PNG sig + IHDR + a 467-byte side-data chunk.
    std::vector<uint8_t> png;
    const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    for (const uint8_t b : chunk("IHDR", std::vector<uint8_t>(13, 0))) png.push_back(b);
    for (const uint8_t b : chunk("teST", std::vector<uint8_t>(467, 0x41))) png.push_back(b);
    ASSERT_EQ(png.size(), 512u);

    // gap (512B): a fake chunk whose CRC is wrong -> contiguous span scores 40
    // (BGC window) while the reassembly below scores 98 (rescued).
    std::vector<uint8_t> gap;
    gap.reserve(512);
    appendBe32(gap, 500);
    const uint8_t ztype[] = {'z', 'z', 'z', 'z'};
    gap.insert(gap.end(), ztype, ztype + 4);
    gap.insert(gap.end(), 500, 0xEE);
    gap.push_back(0xDE); gap.push_back(0xAD); gap.push_back(0xBE); gap.push_back(0xEF);
    ASSERT_EQ(gap.size(), 512u);

    // frag2: 4 KiB IDAT + IEND.
    std::vector<uint8_t> frag2;
    for (const uint8_t b : chunk("IDAT", std::vector<uint8_t>(4096, 0))) frag2.push_back(b);
    const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    frag2.insert(frag2.end(), iend, iend + sizeof(iend));

    const size_t spanLen = png.size() + gap.size() + frag2.size();
    ASSERT_EQ(spanLen, 5144u);

    std::vector<uint8_t> disk(64 * 1024, 0);
    constexpr size_t kOff = 8192; // sector-aligned carve start
    std::memcpy(disk.data() + kOff, png.data(), png.size());
    std::memcpy(disk.data() + kOff + png.size(), gap.data(), gap.size());
    std::memcpy(disk.data() + kOff + png.size() + gap.size(), frag2.data(), frag2.size());

    // Hash = md5 over exactly the first min(64KB, sizeBytes) carved bytes —
    // byte-exact, no sector rounding, no padding past the file end.
    const size_t hashLen = std::min<size_t>(spanLen - gap.size(), 64 * 1024);
    const std::vector<uint8_t> prefix(disk.begin() + kOff, disk.begin() + kOff + hashLen);
    const std::string expectedHash = md5Of(prefix.data(), prefix.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<FileRecord> bgc;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.source == "carver_bgc") bgc.push_back(fr);
    }, &running));

    ASSERT_EQ(bgc.size(), 1u);
    EXPECT_EQ(bgc[0].sizeBytes, spanLen - gap.size()); // reassembled size
    ASSERT_EQ(bgc[0].runs.size(), 2u);                 // frag1 + frag2 runs
    EXPECT_FALSE(bgc[0].contentHash.empty());
    EXPECT_EQ(bgc[0].contentHash, expectedHash);
}

// CA-034: OLE2 magic every sector — each header hit used to rescan the whole
// activeCarve vector, quadratic in the carve count. The indexed engine must
// complete the scan in bounded time and keep emission bounded (header-only
// OLE2 candidates without a size bound are dropped, not emitted).
TEST(SignatureCarve, HostileOle2DensityCompletesBounded) {
    constexpr size_t kSectors = 2048; // 1 MB, magic per sector = 2048 hits
    std::vector<uint8_t> disk(kSectors * 512, 0);
    static const uint8_t ole2[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    for (size_t s = 0; s < kSectors; ++s) {
        std::memcpy(disk.data() + s * 512, ole2, sizeof(ole2));
    }

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    size_t carved = 0;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "carver" || fr.source == "carver_bgc") ++carved;
    }, &running));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(ms, 60000); // generous bound: the O(n^2) scans did not finish at all
    EXPECT_LE(carved, kSectors); // duplicate suppression keeps emission bounded
}

// CA-038: Olympus ORF — TIFF-IFD structure with the "IIRO" variant magic.
// parseTiff must bound the carve via StripOffsets/StripByteCounts and the
// candidate must emit exactly once with the bounded size.
TEST(SignatureCarve, OlympusOrfRawCarvesWithBoundedSize) {
    std::vector<uint8_t> disk(64 * 1024, 0);
    const size_t off = 8; // file-relative; TIFF offsets are file-relative too
    disk[off + 0] = 'I'; disk[off + 1] = 'I'; disk[off + 2] = 'R'; disk[off + 3] = 'O';
    const auto le32 = [&](size_t p, uint32_t v) {
        disk[p] = static_cast<uint8_t>(v);
        disk[p + 1] = static_cast<uint8_t>(v >> 8);
        disk[p + 2] = static_cast<uint8_t>(v >> 16);
        disk[p + 3] = static_cast<uint8_t>(v >> 24);
    };
    const auto le16 = [&](size_t p, uint16_t v) {
        disk[p] = static_cast<uint8_t>(v);
        disk[p + 1] = static_cast<uint8_t>(v >> 8);
    };
    le32(off + 4, 8);  // IFD0 at 8
    le16(off + 8, 2);  // 2 entries
    // entry 1: StripOffsets (273), LONG (4), count 1, value 100
    le16(off + 10, 273); le16(off + 12, 4); le32(off + 14, 1); le32(off + 18, 100);
    // entry 2: StripByteCounts (279), LONG (4), count 1, value 300
    le16(off + 22, 279); le16(off + 24, 4); le32(off + 26, 1); le32(off + 30, 300);
    le32(off + 34, 0);  // no next IFD
    std::memset(disk.data() + off + 100, 0xAB, 300); // strip payload 100..400

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::vector<FileRecord> orfs;
    std::atomic<bool> running{true};
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension == "orf") orfs.push_back(fr);
    }, &running));

    // Exactly one candidate, bounded by the strip extent (100 + 300 = 400).
    ASSERT_EQ(orfs.size(), 1u);
    EXPECT_EQ(orfs[0].sizeBytes, 400u);
}
