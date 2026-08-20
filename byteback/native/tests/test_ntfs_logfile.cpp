#include "fs/ntfs_logfile.h"
#include "fs/ntfs_util.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) img[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

std::vector<uint8_t> buildLogfileHintDisk() {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr uint32_t mftSize = 1024;
    const size_t mftBase = spc * ss; // cluster 1
    const size_t logMft = mftBase + 2 * mftSize;

    std::vector<uint8_t> img(logMft + mftSize, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 44, 1); // mftCluster
    img[60] = static_cast<uint8_t>(0xF6); // clustersPerMftRecord = -10 => 1024 bytes
    img[510] = 0x55;
    img[511] = 0xAA;

    std::memcpy(img.data() + logMft, "FILE", 4);
    writeLe16(img, logMft + 0x14, 0x38);
    writeLe32(img, logMft + 0x18, 256);
    writeLe32(img, logMft + 0x1C, mftSize);

    // RCRD log record with UTF-16 "lost.doc" in client data.
    std::vector<uint8_t> logPayload(128, 0);
    std::memcpy(logPayload.data(), "RCRD", 4);
    const uint16_t clientOff = 32;
    const uint16_t clientLen = 26;
    writeLe16(logPayload, 0x10, clientLen);
    writeLe16(logPayload, 0x12, clientOff);
    writeLe64(logPayload, clientOff, 5); // MFT ref immediately before UTF-16 name
    const wchar_t* name = L"lost.doc";
    for (size_t i = 0; name[i] != 0; ++i)
        writeLe16(logPayload, clientOff + 8 + i * 2, static_cast<uint16_t>(name[i]));

    size_t attr = logMft + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + logPayload.size()));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(logPayload.size()));
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, logPayload.data(), logPayload.size());

    attr += 16 + 8 + logPayload.size();
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    writeLe32(img, attr + 4, 0);
    return img;
}

} // namespace

TEST(NtfsLogfile, RcrdClientDataFindsFilename) {
    auto disk = buildLogfileHintDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    scanNtfsLogFileHints(reader, 0, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running, nullptr);

    bool found = false;
    for (const auto& n : names) {
        if (n.find("lost.doc") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(NtfsLogfile, CollectorStoresHintsWithoutFileCallback) {
    auto disk = buildLogfileHintDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    NtfsLogHintCollector collector;
    int fileCallbacks = 0;
    std::atomic<bool> running{true};
    scanNtfsLogFileHints(reader, 0, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_logfile") ++fileCallbacks;
    }, &running, &collector);

    EXPECT_EQ(fileCallbacks, 0);
    EXPECT_GE(collector.size(), 1u);
    EXPECT_TRUE(collector.findByName("lost.doc"));
    uint64_t mftRef = 0;
    ASSERT_TRUE(collector.findByName("lost.doc", &mftRef));
    EXPECT_EQ(mftRef, 5u);
    std::string name;
    EXPECT_TRUE(collector.findByMftRef(mftRef, &name));
    EXPECT_EQ(name, "lost.doc");
}

TEST(NtfsLogfile, RestartPageEmitsLsn) {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr uint32_t mftSize = 1024;
    const size_t mftBase = spc * ss;
    const size_t logMft = mftBase + 2 * mftSize;

    std::vector<uint8_t> img(logMft + mftSize, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 44, 1);
    img[60] = static_cast<uint8_t>(0xF6);
    img[510] = 0x55;
    img[511] = 0xAA;

    std::memcpy(img.data() + logMft, "FILE", 4);
    writeLe16(img, logMft + 0x14, 0x38);
    writeLe32(img, logMft + 0x18, 256);
    writeLe32(img, logMft + 0x1C, mftSize);

    std::vector<uint8_t> payload(16, 0);
    std::memcpy(payload.data(), "RSTR", 4);
    writeLe64(payload, 8, 42);

    size_t attr = logMft + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + payload.size()));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(payload.size()));
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, payload.data(), payload.size());
    attr += 16 + 8 + payload.size();
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool saw = false;
    std::atomic<bool> running{true};
    scanNtfsLogFileHints(reader, 0, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_logfile_restart" && fr.name.find("42") != std::string::npos) saw = true;
    }, &running);
    EXPECT_TRUE(saw);
}

TEST(MftConfidence, DeletedWithRunsScoresHigherThanMetadataOnly) {
    EXPECT_GT(ntfs::scoreMftConfidence(false, true, false, 4096, true, false),
              ntfs::scoreMftConfidence(false, true, false, 4096, false, false));
    EXPECT_EQ(ntfs::scoreMftConfidence(true, true, false, 0, false, false), 100);
    EXPECT_LT(ntfs::scoreMftConfidence(false, false, false, 100, true, false), 80);
}
