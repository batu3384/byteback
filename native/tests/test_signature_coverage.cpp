#include "byteback_carver.h"
#include "test_temp_path.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace byteback;

TEST(SignatureCoverage, LoadsAtLeastOneHundredFortySignatures) {
    // 89 embedded/ftyp + 61 resource JSON after the CA-001/CA-002 cleanup:
    // the old "400+" set was padded with text magics and duplicate anchors
    // that could never win the same-offset dedup.
    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));
    EXPECT_GE(carver.signatureCount(), 140u);
}

TEST(SignatureCoverage, OverlayLoadsFromNonAsciiUtf8Path) {
    const auto dir = std::filesystem::temp_directory_path() /
                     std::filesystem::u8path("byteback_sig_ş_" + std::to_string(bytebackTestPid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "overlay.json";
    {
        std::ofstream out(file);
        out << R"([{"format":"Test","extension":".tst","category":"Archive",)"
            << R"("header":"0011223344556677","footer":"","max_size":4096}])";
    }

    EXPECT_TRUE(CarvingEngine::setSignatureOverlay(file.u8string()));
    CarvingEngine::setSignatureOverlay(""); // process-wide: leave no overlay behind
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
