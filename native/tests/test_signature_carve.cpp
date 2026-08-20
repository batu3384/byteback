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
