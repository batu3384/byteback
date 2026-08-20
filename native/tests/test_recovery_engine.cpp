#include "byteback_recovery.h"
#include "byteback_io.h"
#include "byteback_db.h"
#include "carver/file_validators.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace byteback;

class RecoveryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        dest_ = (std::filesystem::temp_directory_path() / "byteback_test").string();
        std::filesystem::create_directories(dest_);
    }
    void TearDown() override {
        std::filesystem::remove_all(dest_);
    }

    std::string dest_;
};

namespace {

std::vector<uint8_t> minimalJpegBytes() {
    std::vector<uint8_t> jpeg;
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD8);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDB);
    jpeg.push_back(0x00);
    jpeg.push_back(0x03);
    jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDA);
    jpeg.push_back(0x00);
    jpeg.push_back(0x02);
    for (int i = 0; i < 20; ++i) jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

} // namespace

TEST_F(RecoveryEngineTest, CarverSourceGetsValidationScore) {
    const auto jpeg = minimalJpegBytes();
    ASSERT_GE(carver::validateJpeg(jpeg.data(), jpeg.size()), 85);

    std::vector<uint8_t> img(512 * 4, 0);
    std::memcpy(img.data() + 512, jpeg.data(), jpeg.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "photo.jpg";
    rec.sizeBytes = static_cast<int64_t>(jpeg.size());
    rec.startSector = 1;
    rec.source = "carver";

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_GE(result.validationScore, 85);
}

TEST_F(RecoveryEngineTest, CarvedContiguousFile) {
    std::vector<uint8_t> img(512 * 10);
    const char payload[] = "HELLO_BYTEBACK_RECOVERY";
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
    EXPECT_EQ(content, "HELLO_BYTEBACK_RECOVERY");
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
    auto path = (std::filesystem::temp_directory_path() / "byteback_recover_lookup.db").string();
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

TEST_F(RecoveryEngineTest, PaddedReadIsNotSuccess) {
    std::vector<uint8_t> img(512, 0x41);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "past_eof.bin";
    rec.sizeBytes = 512;
    rec.runs = {{8, 1}};
    rec.source = "ntfs_mft";

    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.zeroFilled);
}

TEST_F(RecoveryEngineTest, DiscoveryOnlySourcesRefuse) {
    DiskReader reader;
    reader.attachMemoryVolume(std::vector<uint8_t>(512, 0));
    FileRecord rec;
    rec.name = "vol";
    rec.source = "apfs_file";
    rec.runs = {{1, 1}};
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, rec, dest_);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("discovery"), std::string::npos);
}

TEST(RecoveryHelpers, BindReaderRejectsVssWithoutVolume) {
    DiskReader reader;
    FileRecord rec;
    rec.source = "vss_ntfs";
    rec.path = "/VSS99/x";
    rec.runs = {{0, 1}};
    std::string err;
    EXPECT_FALSE(bindReaderForRecord(reader, rec, 0, nullptr, err));
    EXPECT_FALSE(err.empty());
}

TEST(RecoveryHelpers, ApplyBoundFvekSkipsVssCopiesPhysical) {
    DiskReader src, destVss, destPhys;
    uint8_t key[32] = {};
    key[0] = 0x11;
    ASSERT_TRUE(src.setXtsFvek128(key, 32));

    FileRecord vss;
    vss.source = "vss_ntfs";
    vss.path = "/VSS1/x";
    applyBoundFvek(destVss, src, vss);
    EXPECT_FALSE(destVss.hasXtsFvek());

    FileRecord phys;
    phys.source = "ntfs_mft";
    applyBoundFvek(destPhys, src, phys);
    EXPECT_TRUE(destPhys.hasXtsFvek());
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
