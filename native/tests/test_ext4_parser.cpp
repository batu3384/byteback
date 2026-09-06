#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

using namespace byteback;

TEST(Ext4Parser, FindsNoteTxt) {
    auto img = byteback::testfix::buildExt4Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n == "note.txt") found = true;
    }
    EXPECT_TRUE(found);
}

namespace {

void writeLe32Local(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeLe16Local(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>(v >> 8);
}

} // namespace

TEST(Ext4Parser, HugeExtentEntryCountIsClamped) {
    // Regression: eh_entries was trusted as-is; a crafted header read past the
    // inode (root) / block buffer (children). Must clamp, not crash.
    auto img = byteback::testfix::buildExt4Volume();
    constexpr uint32_t bs = 1024;
    const size_t ino13 = 5 * bs + 12 * 128; // inode table block 5, inode 13
    writeLe32Local(img, ino13 + 0x20, 0x80000);      // EXT4_EXTENTS_FL
    writeLe16Local(img, ino13 + 0x28, 0xF30A);       // eh_magic
    writeLe16Local(img, ino13 + 0x2A, 0xFFFF);       // eh_entries: absurd
    writeLe16Local(img, ino13 + 0x2C, 4);            // eh_max
    writeLe16Local(img, ino13 + 0x2E, 0);            // eh_depth = leaf

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    EXPECT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool found = false;
    for (const auto& n : names) if (n == "note.txt") found = true;
    EXPECT_TRUE(found);
}

TEST(Ext4Parser, BogusLogBlockSizeRejected) {
    // s_log_block_size > 6 is invalid (shift UB / absurd block size).
    auto img = byteback::testfix::buildExt4Volume();
    constexpr uint32_t bs = 1024;
    writeLe32Local(img, bs + 0x18, 99); // s_log_block_size
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    EXPECT_FALSE(ext4.scan(reader, [](const FileRecord&) {}, &running));
}
