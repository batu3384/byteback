#include "wolf_recovery.h"
#include "wolf_io.h"
#include "wolf_db.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wolf;

class RecoveryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        dest_ = (std::filesystem::temp_directory_path() / "wolf_recovery_test").string();
        std::filesystem::create_directories(dest_);
    }
    void TearDown() override {
        std::filesystem::remove_all(dest_);
    }

    std::string dest_;
};

TEST_F(RecoveryEngineTest, CarvedContiguousFile) {
    std::vector<uint8_t> img(512 * 10);
    const char payload[] = "HELLO_WOLF_RECOVERY";
    std::memcpy(img.data() + 512 * 2, payload, sizeof(payload));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "hello.txt";
    rec.sizeBytes = sizeof(payload) - 1;
    rec.startSector = 2;
    rec.status = 0;

    RecoveryEngine engine;
    auto result = engine.recoverCarvedFile(reader, rec, dest_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesRecovered, rec.sizeBytes);
    ASSERT_FALSE(result.destPath.empty());
    std::ifstream in(result.destPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "HELLO_WOLF_RECOVERY");
}

TEST_F(RecoveryEngineTest, MultiRunRecovery) {
    std::vector<uint8_t> img(512 * 20, 0);
    img[512 * 1] = 'A';
    img[512 * 5] = 'B';

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "twopart.bin";
    rec.sizeBytes = 1024;
    rec.runs = {{1, 1}, {5, 1}};

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesRecovered, 1024u);

    std::ifstream in(result.destPath, std::ios::binary);
    char a = 0, b = 0;
    in.seekg(0);
    in.read(&a, 1);
    in.seekg(512);
    in.read(&b, 1);
    EXPECT_EQ(a, 'A');
    EXPECT_EQ(b, 'B');
}

TEST_F(RecoveryEngineTest, PathTraversalNeutralized) {
    std::vector<uint8_t> img(512, 0x42);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "..\\..\\evil.bin";
    rec.sizeBytes = 4;
    rec.startSector = 0;

    RecoveryEngine engine;
    auto result = engine.recoverCarvedFile(reader, rec, dest_);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.destPath.find("evil.bin") != std::string::npos);
    EXPECT_TRUE(result.destPath.find("..") == std::string::npos);
}

TEST_F(RecoveryEngineTest, FailsWhenDiskClosed) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "x.bin";
    rec.sizeBytes = 1;
    rec.startSector = 0;

    RecoveryEngine engine;
    auto result = engine.recoverCarvedFile(reader, rec, dest_);
    EXPECT_FALSE(result.success);
}

TEST_F(RecoveryEngineTest, RefusesEmptyRunsForMetadataSource) {
    std::vector<uint8_t> img(512, 0x41);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "resident.bin";
    rec.sizeBytes = 16;
    rec.startSector = 0;
    rec.source = "mft";

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("resident"), std::string::npos);
}

TEST_F(RecoveryEngineTest, CarverEmptyRunsStillRecover) {
    std::vector<uint8_t> img(512, 0x42);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "carved.bin";
    rec.sizeBytes = 4;
    rec.startSector = 0;
    rec.source = "carver";

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_TRUE(result.success);
}

TEST_F(RecoveryEngineTest, RecoversResidentPayloadWithoutRuns) {
    DiskReader reader;
    reader.attachMemoryVolume(std::vector<uint8_t>(512, 0x00));

    FileRecord rec;
    rec.name = "resident.txt";
    rec.sizeBytes = 5;
    rec.source = "ntfs_mft";
    rec.residentData = {'h', 'e', 'l', 'l', 'o'};

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.bytesRecovered, 5u);
    EXPECT_FALSE(result.zeroFilled);
    std::ifstream in(result.destPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "hello");
}

TEST_F(RecoveryEngineTest, LoadRecoverRecordRejectsMissingIds) {
    MetadataStore store;
    auto path = (std::filesystem::temp_directory_path() / "wolf_recover_lookup.db").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(store.open(path));
    FileRecord out;
    std::string err;
    EXPECT_FALSE(loadRecoverRecord(store, -1, 1, out, err));
    EXPECT_FALSE(loadRecoverRecord(store, 1, 0, out, err));
    int64_t scanId = store.createScan(0, "quick", 10);
    EXPECT_FALSE(loadRecoverRecord(store, scanId, 99999, out, err));
    FileRecord rec;
    rec.name = "a.bin";
    rec.sizeBytes = 1;
    rec.runs = {{4, 2}};
    rec.residentData = {1, 2, 3};
    int64_t id = store.insertFile(scanId, rec);
    ASSERT_GT(id, 0);
    ASSERT_TRUE(loadRecoverRecord(store, scanId, id, out, err)) << err;
    ASSERT_EQ(out.runs.size(), 1u);
    EXPECT_EQ(out.runs[0].startSector, 4u);
    ASSERT_EQ(out.residentData.size(), 3u);
    store.close();
    std::filesystem::remove(path);
}

TEST_F(RecoveryEngineTest, ZeroFilledDoesNotCountAsRecovered) {
    RecoveryResult r;
    r.success = true;
    r.zeroFilled = true;
    EXPECT_FALSE(countsAsRecovered(r));
    r.zeroFilled = false;
    EXPECT_TRUE(countsAsRecovered(r));
}

TEST_F(RecoveryEngineTest, RejectsUnsafeDestDir) {
    std::vector<uint8_t> img(512, 0x42);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord rec;
    rec.name = "x.bin";
    rec.sizeBytes = 1;
    rec.startSector = 0;
    rec.source = "carver";
    RecoveryEngine engine;
    auto result = engine.recoverCarvedFile(reader, rec, dest_ + "/../evil");
    EXPECT_FALSE(result.success);
}
