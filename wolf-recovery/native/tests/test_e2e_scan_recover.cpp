// End-to-end integration: scan a FAT fixture, persist to MetadataStore,
// recover via RecoveryEngine, verify payload and MD5.
#include "scan_coordinator.h"
#include "wolf_db.h"
#include "wolf_io.h"
#include "wolf_recovery.h"
#include "fixtures/volume_fixtures.h"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wolf;

class E2EScanRecoverTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath_ = (std::filesystem::temp_directory_path() / "wolf_e2e_test.db").string();
        destDir_ = (std::filesystem::temp_directory_path() / "wolf_e2e_recover").string();
        std::filesystem::remove(dbPath_);
        std::filesystem::remove_all(destDir_);
        ASSERT_TRUE(store_.open(dbPath_));
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(dbPath_);
        std::filesystem::remove_all(destDir_);
    }

    MetadataStore store_;
    std::string dbPath_;
    std::string destDir_;
};

TEST_F(E2EScanRecoverTest, FatQuickScanPersistAndRecover) {
    auto fatVol = wolf::testfix::buildFat16Volume();
    auto disk = wolf::testfix::buildMbrDiskWithFatPartition(fatVol, 2048);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    const uint32_t sectorSize = reader.getSectorSize() ? reader.getSectorSize() : 512;
    const uint64_t totalSectors = reader.getDiskSize() / sectorSize;
    const int64_t scanId = store_.createScan(0, "quick", totalSectors);
    ASSERT_GT(scanId, 0);

    std::vector<FileRecord> scanned;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (fr.name.empty()) return;
        scanned.push_back(fr);
        store_.insertFile(scanId, fr);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);

    const FileRecord* target = nullptr;
    for (const auto& fr : scanned) {
        if (fr.name.find("TEST") != std::string::npos) {
            target = &fr;
            break;
        }
    }
    ASSERT_NE(target, nullptr) << "TEST.TXT not found by quick scan";

    auto fromDb = store_.getFiles(scanId, 0, 100);
    ASSERT_GE(fromDb.size(), 1u);

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, *target, destDir_);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.bytesRecovered, target->sizeBytes);
    EXPECT_FALSE(result.md5Hash.empty());

    std::ifstream in(result.destPath, std::ios::binary);
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "Hello FAT16");
}
