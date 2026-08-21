#include "fs/ntfs_recycle.h"
#include "byteback_db.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;
using namespace byteback::ntfs;

static std::vector<uint8_t> buildIFile(uint64_t ver, const std::u16string& path, uint64_t size, uint64_t ft) {
    std::vector<uint8_t> b(24 + (path.size() + 1) * 2, 0);
    auto w64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    w64(0, ver);
    w64(8, size);
    w64(16, ft);
    for (size_t i = 0; i < path.size(); ++i) {
        b[24 + i * 2] = static_cast<uint8_t>(path[i] & 0xFF);
        b[24 + i * 2 + 1] = static_cast<uint8_t>((path[i] >> 8) & 0xFF);
    }
    return b;
}

TEST(NtfsRecycle, ParsesVersion1Path) {
    auto raw = buildIFile(1, u"C:\\Users\\x\\doc.txt", 99, 132000000000000000ULL);
    auto info = parseRecycleIFile(raw.data(), raw.size());
    ASSERT_TRUE(info.ok);
    EXPECT_EQ(info.fileSize, 99u);
    EXPECT_NE(info.originalPath.find("doc.txt"), std::string::npos);
    EXPECT_EQ(recycleFileName(info.originalPath), "doc.txt");
}

TEST(NtfsRecycle, PairsIRRecords) {
    struct Wrap { FileRecord fr; };
    std::vector<Wrap> files(2);
    files[0].fr.name = "$Iabc123.txt";
    files[0].fr.residentData = buildIFile(2, u"C:\\Photos\\vacation.jpg", 2048, 132000000000000000ULL);
    files[0].fr.source = "ntfs_mft";
    files[1].fr.name = "$Rabc123.txt";
    files[1].fr.source = "ntfs_mft";
    files[1].fr.status = 0;
    files[1].fr.confidence = 50;
    files[1].fr.runs = {{100, 4}};

    applyRecycleBinRecords(files);
    EXPECT_EQ(files[1].fr.name, "vacation.jpg");
    EXPECT_EQ(files[1].fr.source, "ntfs_recycle");
    EXPECT_EQ(files[1].fr.sizeBytes, 2048u);
    EXPECT_EQ(files[0].fr.source, "ntfs_recycle_meta");
}

TEST(NtfsRecycle, SkipsPairingWhenIFileHasNoResidentBytes) {
    struct Wrap { FileRecord fr; };
    std::vector<Wrap> files(2);
    files[0].fr.name = "$Iabc123.txt";
    files[0].fr.source = "ntfs_mft";
    files[1].fr.name = "$Rabc123.txt";
    files[1].fr.source = "ntfs_mft";
    files[1].fr.status = 0;
    applyRecycleBinRecords(files);
    EXPECT_EQ(files[1].fr.name, "$Rabc123.txt");
    EXPECT_EQ(files[1].fr.source, "ntfs_mft");
    EXPECT_EQ(files[0].fr.source, "ntfs_mft");
}
