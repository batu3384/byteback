#include "wolf_recovery.h"
#include "wolf_io.h"
#include <gtest/gtest.h>
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
