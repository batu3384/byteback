#include "wolf_fs.h"
#include "wolf_io.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace wolf;

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

std::vector<uint8_t> buildNtfsMftCarveDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = 8;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t mft = 8 * ss;
    std::memcpy(img.data() + mft, "FILE", 4);
    writeLe16(img, mft + 0x14, 0x38);
    writeLe16(img, mft + 0x16, 0x01);
    writeLe32(img, mft + 0x18, 256);
    writeLe32(img, mft + 0x1C, 1024);

    size_t attr = mft + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;

    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i) {
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    }

    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);

    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    writeLe32(img, attr + 4, 0);
    return img;
}

} // namespace

TEST(NtfsParser, CarvesMftFileRecord) {
    auto img = buildNtfsMftCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n == "doc.txt") found = true;
    }
    EXPECT_TRUE(found);
}
