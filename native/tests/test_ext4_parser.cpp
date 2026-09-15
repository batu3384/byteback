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

TEST(Ext4Parser, UnreadDirIsSentinelNotEmptyDirectory) {
    auto img = byteback::testfix::buildExt4Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Directory lives at block 3, 1 KiB, 512-byte sectors → sectors 6-7
    // (inode table is blocks 5-6; do not fault that range or the walk never starts).
    reader.setMemoryFaultRange(6, 2);

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawNote = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "note.txt") sawNote = true;
    }
    for (const auto& s : sources) {
        if (s == "ext4_dir_unread") sawUnread = true;
    }
    EXPECT_FALSE(sawNote) << "unread ext4 dir must not parse zeros as an empty directory of note.txt";
    EXPECT_TRUE(sawUnread);
}

TEST(Ext4Parser, UnreadInodeTableIsSentinelNotEmptyVolume) {
    auto img = byteback::testfix::buildExt4Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Inode table starts at block 5, 2 KiB → sectors 10-13.
    reader.setMemoryFaultRange(10, 4);

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawNote = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "note.txt") sawNote = true;
    }
    for (const auto& s : sources) {
        if (s == "ext4_dir_unread") sawUnread = true;
    }
    EXPECT_FALSE(sawNote) << "unread inode table must not look like an empty ext4 volume";
    EXPECT_TRUE(sawUnread);
}

TEST(Ext4Parser, UnreadGdtIsSentinelNotEmptyVolume) {
    auto img = byteback::testfix::buildExt4Volume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // GDT lives at block 2 (1 KiB blocks) → sector 4.
    reader.setMemoryFaultRange(4, 1);

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawNote = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "note.txt") sawNote = true;
    }
    for (const auto& s : sources) {
        if (s == "ext4_dir_unread") sawUnread = true;
    }
    EXPECT_FALSE(sawNote) << "unread GDT must not look like an empty ext4 volume";
    EXPECT_TRUE(sawUnread);
}

TEST(Ext4Parser, PaddedGdtIsSentinelNotEmptyVolume) {
    auto img = byteback::testfix::buildExt4Volume();
    img.resize(2048); // superblock fits; GDT at byte 2048 past EOF zero-pads
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawNote = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "note.txt") sawNote = true;
    }
    for (const auto& s : sources) {
        if (s == "ext4_dir_unread") sawUnread = true;
    }
    EXPECT_FALSE(sawNote) << "padded GDT must not look like an empty ext4 volume";
    EXPECT_TRUE(sawUnread);
}

TEST(Ext4Parser, UnreadExtentChildIsSentinelNotEmptyDirectory) {
    auto img = byteback::testfix::buildExt4Volume();
    constexpr uint32_t bs = 1024;
    const size_t ino12 = 5 * bs + 11 * 128; // directory inode 12
    writeLe32Local(img, ino12 + 0x20, 0x80000); // EXT4_EXTENTS_FL
    writeLe16Local(img, ino12 + 0x28, 0xF30A);  // eh_magic
    writeLe16Local(img, ino12 + 0x2A, 1);       // eh_entries
    writeLe16Local(img, ino12 + 0x2C, 4);       // eh_max
    writeLe16Local(img, ino12 + 0x2E, 1);       // eh_depth = internal
    writeLe32Local(img, ino12 + 0x34, 0);       // ei_block
    writeLe32Local(img, ino12 + 0x38, 8);       // ei_leaf_lo → block 8
    // Leaf at block 8: one extent covering directory block 3.
    const size_t leaf = 8 * bs;
    writeLe16Local(img, leaf + 0x00, 0xF30A);
    writeLe16Local(img, leaf + 0x02, 1);
    writeLe16Local(img, leaf + 0x04, 4);
    writeLe16Local(img, leaf + 0x06, 0); // depth leaf
    writeLe32Local(img, leaf + 0x0C, 0); // ee_block
    writeLe16Local(img, leaf + 0x10, 1); // ee_len
    writeLe16Local(img, leaf + 0x12, 0); // ee_start_hi
    writeLe32Local(img, leaf + 0x14, 3); // ee_start_lo

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Extent child at block 8 → sectors 16–17. Superblock, GDT, inode table stay readable.
    reader.setMemoryFaultRange(16, 2);

    std::vector<std::string> names;
    std::vector<std::string> sources;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
        if (!fr.source.empty()) sources.push_back(fr.source);
    }, &running));

    bool sawNote = false;
    bool sawUnread = false;
    for (const auto& n : names) {
        if (n == "note.txt") sawNote = true;
    }
    for (const auto& s : sources) {
        if (s == "ext4_dir_unread") sawUnread = true;
    }
    EXPECT_FALSE(sawNote) << "unread extent child must not parse as an empty successful directory";
    EXPECT_TRUE(sawUnread);
}
