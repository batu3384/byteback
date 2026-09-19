#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <string>

using namespace byteback;

TEST(Jbd2, JournalNameNotInLiveDirent) {
    auto img = testfix::buildExt4JournalVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool sawNote = false;
    bool sawGone = false;
    std::string goneSource;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "note.txt") sawNote = true;
        if (fr.name == "gone.txt") {
            sawGone = true;
            goneSource = fr.source;
        }
    }, &running));
    EXPECT_TRUE(sawNote);
    EXPECT_TRUE(sawGone);
    EXPECT_EQ(goneSource, "ext4_journal");
}

TEST(Jbd2, UnreadJournalDoesNotInventGoneTxt) {
    auto img = testfix::buildExt4JournalVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // Journal data at blocks 8-10, 1024 B, 512 B sectors → sectors 16-21.
    reader.setMemoryFaultRange(16, 6);

    bool sawGone = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.txt") sawGone = true;
        if (fr.source == "ext4_journal_unread") sawUnread = true;
    }, &running));
    EXPECT_FALSE(sawGone) << "unread journal zeros must not parse as gone.txt";
    EXPECT_TRUE(sawUnread);
}

TEST(Jbd2, ReplayCommittedFileHasRunsAndPayload) {
    auto img = testfix::buildExt4JournalReplayVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord gone;
    bool sawPhantom = false;
    std::atomic<bool> running{true};
    Ext4Parser ext4;
    ASSERT_TRUE(ext4.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.dat") gone = fr;
        if (fr.name == "phantom.bin") sawPhantom = true;
    }, &running));
    EXPECT_EQ(gone.source, "ext4_journal_replay");
    ASSERT_FALSE(gone.runs.empty());
    EXPECT_EQ(gone.sizeBytes, 8u);
    EXPECT_EQ(gone.status, 0);
    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;
    std::vector<uint8_t> buf(8, 0);
    auto res = reader.readBytes(gone.runs[0].startSector * static_cast<uint64_t>(ss), 8, buf.data());
    ASSERT_TRUE(readComplete(res, 8));
    EXPECT_EQ(std::string(buf.begin(), buf.end()), "JBD2DATA");
    EXPECT_FALSE(sawPhantom) << "uncommitted descriptor must not emit phantom.bin";
}
