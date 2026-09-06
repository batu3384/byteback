#include "byteback_carver.h"
#include "byteback_io.h"
#include "crypto/byteback_md5.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
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
