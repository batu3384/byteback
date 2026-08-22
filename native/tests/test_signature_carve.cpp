#include "byteback_carver.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace byteback;

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
