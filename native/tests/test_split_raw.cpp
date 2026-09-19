#include "io/byte_source.h"
#include "byteback_io.h"
#include "byteback_fs.h"
#include "fixtures/volume_fixtures.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void writeBytes(const std::filesystem::path& p, const uint8_t* data, size_t n) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    ASSERT_TRUE(out.good());
}

} // namespace

TEST(SplitRaw, Concatenates001And002AcrossBoundary) {
    const auto p1 = bytebackTestTemp("bb_split", ".001");
    const auto p2 = std::filesystem::path(p1).replace_extension(".002");
    std::filesystem::remove(p1);
    std::filesystem::remove(p2);
    std::vector<uint8_t> a(100, 0xAA);
    std::vector<uint8_t> b(100, 0xBB);
    writeBytes(p1, a.data(), a.size());
    writeBytes(p2, b.data(), b.size());
    std::string err;
    auto src = openFileByteSource(p1.string(), err);
    ASSERT_NE(src, nullptr) << err;
    EXPECT_EQ(src->size(), 200u);
    uint8_t buf[20] = {};
    ASSERT_TRUE(src->read(90, buf, 20));
    for (int i = 0; i < 10; ++i) EXPECT_EQ(buf[i], 0xAA);
    for (int i = 10; i < 20; ++i) EXPECT_EQ(buf[i], 0xBB);
    src.reset();
    std::filesystem::remove(p1);
    std::filesystem::remove(p2);
}

TEST(SplitRaw, GapWithout002DoesNotSkipTo003) {
    const auto p1 = bytebackTestTemp("bb_split_gap", ".001");
    const auto p3 = std::filesystem::path(p1).replace_extension(".003");
    std::filesystem::remove(p1);
    std::filesystem::remove(p3);
    std::vector<uint8_t> a(16, 0x11);
    std::vector<uint8_t> c(16, 0x33);
    writeBytes(p1, a.data(), a.size());
    writeBytes(p3, c.data(), c.size());
    std::string err;
    auto src = openFileByteSource(p1.string(), err);
    ASSERT_NE(src, nullptr) << err;
    EXPECT_EQ(src->size(), 16u);
    src.reset();
    std::filesystem::remove(p1);
    std::filesystem::remove(p3);
}

TEST(SplitRaw, Lone001IsSingleFile) {
    const auto p1 = bytebackTestTemp("bb_split_lone", ".001");
    std::filesystem::remove(p1);
    std::vector<uint8_t> a(32, 0xCC);
    writeBytes(p1, a.data(), a.size());
    std::string err;
    auto src = openFileByteSource(p1.string(), err);
    ASSERT_NE(src, nullptr) << err;
    EXPECT_EQ(src->size(), 32u);
    src.reset();
    std::filesystem::remove(p1);
}

TEST(SplitRaw, EvidenceImageScanFindsFatAcrossSplit) {
    auto fat = byteback::testfix::buildFat16Volume();
    ASSERT_GT(fat.size(), 64u);
    const size_t mid = fat.size() / 2;
    const auto p1 = bytebackTestTemp("bb_split_fat", ".001");
    const auto p2 = std::filesystem::path(p1).replace_extension(".002");
    std::filesystem::remove(p1);
    std::filesystem::remove(p2);
    writeBytes(p1, fat.data(), mid);
    writeBytes(p2, fat.data() + mid, fat.size() - mid);
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEvidenceImage(p1.string(), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool found = false;
    for (const auto& n : names) {
        if (n.find("TEST") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
    reader.detachImageBackend();
    std::filesystem::remove(p1);
    std::filesystem::remove(p2);
}
