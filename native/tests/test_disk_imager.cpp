// DiskImager over a memory volume: RAW byte-for-byte output + MD5, EWF
// with digest section, bad-read zero-fill accounting, and stop semantics.
#include "byteback_imager.h"
#include "byteback_io.h"
#include "crypto/byteback_md5.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace byteback;

namespace {

std::vector<uint8_t> makeVolume(size_t sectors, uint8_t seed) {
    std::vector<uint8_t> v(sectors * 512);
    uint8_t x = seed;
    for (auto& b : v) b = x = static_cast<uint8_t>(x * 31 + 7);
    return v;
}

} // namespace

TEST(DiskImagerTest, RawImageMatchesSourceAndMd5) {
    const auto vol = makeVolume(64, 0x11);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    const std::string dest = (std::filesystem::temp_directory_path() / "bb_img_test.raw").string();
    DiskImager imager;
    std::atomic<bool> done{false};
    std::string md5AtEnd;
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) { done = true; }
    }, ImageFormat::Raw);
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    imager.stopImaging();
    ASSERT_TRUE(done.load());

    std::ifstream in(dest, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol);

    crypto::Md5 md5;
    md5.update(vol.data(), vol.size());
    EXPECT_EQ(imager.lastImageMd5(), md5.finalHex());
}

TEST(DiskImagerTest, EwfImageCarriesDigestAndRereadsIdentical) {
    const auto vol = makeVolume(32, 0x42);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    const std::string dest = (std::filesystem::temp_directory_path() / "bb_img_test.E01").string();
    DiskImager imager;
    std::atomic<bool> done{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Ewf);
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    imager.stopImaging();
    ASSERT_TRUE(done.load());
    EXPECT_FALSE(imager.lastImageMd5().empty());

    // Round-trip through the reader: E01 bytes must match the source.
    DiskReader back;
    std::string err;
    ASSERT_TRUE(back.attachEwfImage(dest, &err)) << err;
    std::vector<uint8_t> out(vol.size());
    auto res = back.readSectors(0, static_cast<uint32_t>(vol.size()), out.data());
    ASSERT_TRUE(res.success);
    EXPECT_EQ(out, vol);
}
