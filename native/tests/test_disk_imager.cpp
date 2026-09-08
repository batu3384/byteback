// DiskImager over a memory volume: RAW byte-for-byte output + MD5, EWF
// with digest section, bad-read zero-fill accounting, and stop semantics.
#include "byteback_imager.h"
#include "byteback_io.h"
#include "crypto/byteback_md5.h"
#include "imager/ewf_reader.h"
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
    in.close();
    EXPECT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol);

    crypto::Md5 md5;
    md5.update(vol.data(), vol.size());
    EXPECT_EQ(imager.lastImageMd5(), md5.finalHex());
    std::filesystem::remove(dest);
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
    back.detachImageBackend();
}

// Cancelled acquisition: the partial E01 must stay readable (tables written)
// but carry NO digest and NO MD5 — a truncated copy can never claim to be a
// verified image. requestStop from the first progress callback deterministically
// stops the run after chunk 1 (the loop re-checks the stop flag before chunk 2).
TEST(DiskImagerTest, CancelledEwfIsReadableButUnverified) {
    constexpr size_t kSectors = 131072; // 64 MiB = 4 x 16 MiB chunks
    const auto vol = makeVolume(kSectors, 0x33);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    const std::string dest = (std::filesystem::temp_directory_path() / "bb_img_cancel.E01").string();
    DiskImager imager;
    std::atomic<bool> finished{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) finished = true;
        else imager.requestStop(); // first chunk done -> cancel
    }, ImageFormat::Ewf);
    // Wait for the worker's completion tick before joining: a stopImaging()
    // from here could otherwise land before chunk 1 even starts.
    for (int i = 0; i < 400 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager.stopImaging();
    ASSERT_TRUE(finished.load());

    EXPECT_TRUE(imager.lastImageMd5().empty());

    {
        DiskReader back;
        std::string err;
        ASSERT_TRUE(back.attachEwfImage(dest, &err)) << err;
        constexpr size_t kChunkBytes = 16u * 1024u * 1024u;
        std::vector<uint8_t> out(kChunkBytes);
        auto res = back.readSectors(0, kChunkBytes, out.data());
        ASSERT_TRUE(res.success);
        EXPECT_EQ(out, std::vector<uint8_t>(vol.begin(), vol.begin() + kChunkBytes));
        // The image ends where the cancel landed — no phantom bytes.
        EXPECT_FALSE(back.readSectors(kChunkBytes, 512, out.data()).success);
    }
    std::filesystem::remove(dest);
}

// CA-037: one bad sector used to zero-fill the entire 16MB chunk silently.
// The halving retry must zero only the actually-failed sectors and count
// each of them in badSectorReads().
TEST(DiskImagerTest, BadRegionZeroedGranularly) {
    constexpr size_t kSectors = 256;
    const auto vol = makeVolume(kSectors, 0x11);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));
    reader.setMemoryFaultRange(100, 4); // sectors 100..103 fail

    const std::string dest = (std::filesystem::temp_directory_path() / "bb_img_faults.raw").string();
    DiskImager imager;
    std::atomic<bool> done{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Raw);
    for (int i = 0; i < 400 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    imager.stopImaging();
    ASSERT_TRUE(done.load());

    // Granular telemetry: exactly the failed sectors, not the whole chunk.
    EXPECT_EQ(imager.badSectorReads(), 4u);

    std::ifstream in(dest, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    ASSERT_EQ(out.size(), vol.size());

    // Bytes outside the fault range match the source.
    EXPECT_EQ(std::memcmp(out.data(), vol.data(), 100 * 512), 0);
    EXPECT_EQ(std::memcmp(out.data() + 104 * 512, vol.data() + 104 * 512, (kSectors - 104) * 512), 0);
    // The fault range itself is zero-filled in the image.
    EXPECT_EQ(out, [&] {
        std::vector<uint8_t> expected = vol;
        std::memset(expected.data() + 100 * 512, 0, 4 * 512);
        return expected;
    }());
    std::filesystem::remove(dest);
}

// D3: sectors that failed during acquisition are carried into the E01's
// acquiry-error section and round-trip through EwfReader::acquiryErrors().
TEST(DiskImagerAcquiryErrors, FaultRangeLandsInEwfErrorSection) {
    constexpr size_t kSectors = 256;
    const auto vol = makeVolume(kSectors, 0x21);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));
    reader.setMemoryFaultRange(100, 4); // sectors 100..103 fail

    const std::string dest = (std::filesystem::temp_directory_path() / "bb_img_faults.E01").string();
    std::filesystem::remove(dest);
    DiskImager imager;
    std::atomic<bool> done{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Ewf);
    for (int i = 0; i < 400 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    imager.stopImaging();
    ASSERT_TRUE(done.load());

    EwfReader back;
    std::string err;
    ASSERT_TRUE(back.open(dest, err)) << err;
    const auto& errs = back.acquiryErrors();
    ASSERT_EQ(errs.size(), 1u); // adjacent single-sector failures merge
    EXPECT_EQ(errs[0].first, 100u);
    EXPECT_EQ(errs[0].second, 4u);
    // The digest still covers the zero-filled image content.
    EXPECT_FALSE(back.md5Hex().empty());
    back.close(); // release the file handle before the Windows remove
    std::filesystem::remove(dest);
}

// ---------------------------------------------------------------------------
// B2/B3: RAW imaging resume via <dest>.part + <dest>.part.json.
// ---------------------------------------------------------------------------

// Cancel mid-image, then resume: the final file must be byte-exact with the
// source and the resumed run's MD5 must equal the one-shot digest. The
// .part/.sidecar pair is the resume point after cancel and must be gone after
// success.
TEST(DiskImagerResume, CancelThenResumeProducesByteExactImage) {
    constexpr size_t kSectors = 8192; // 4 MiB source
    const auto vol = makeVolume(kSectors, 0x77);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    DiskImager imager;
    imager.setChunkSectorsForTest(1024); // 512 KiB chunks: deterministic cancel point
    std::atomic<bool> finished{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) finished = true;
        else imager.requestStop(); // first chunk done -> cancel after ~512 KiB
    }, ImageFormat::Raw);
    for (int i = 0; i < 400 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager.stopImaging();
    ASSERT_TRUE(finished.load());
    EXPECT_TRUE(imager.lastImageMd5().empty()); // cancelled run is unverified

    // Resume point kept: .part holds the first chunk, sidecar present, the
    // destination itself does not exist yet.
    ASSERT_TRUE(std::filesystem::exists(part));
    ASSERT_TRUE(std::filesystem::exists(side));
    EXPECT_FALSE(std::filesystem::exists(dest));
    EXPECT_EQ(std::filesystem::file_size(part), 1024ull * 512ull);

    // Second run resumes from the sidecar and completes.
    DiskImager imager2;
    imager2.setChunkSectorsForTest(1024);
    std::atomic<bool> done{false};
    imager2.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Raw);
    for (int i = 0; i < 600 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager2.stopImaging();
    ASSERT_TRUE(done.load());

    std::ifstream in(dest, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    ASSERT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol); // byte-exact across the resume seam
    crypto::Md5 md5;
    md5.update(vol.data(), vol.size());
    EXPECT_EQ(imager2.lastImageMd5(), md5.finalHex());

    // Success cleans up the resume bookkeeping.
    EXPECT_FALSE(std::filesystem::exists(part));
    EXPECT_FALSE(std::filesystem::exists(side));
    std::filesystem::remove(dest);
}

// A sidecar that does not match the current source (wrong sourceKey) means a
// FRESH restart: the stale .part is discarded, the output is written from
// zero, and both artifacts are gone on success.
TEST(DiskImagerResume, StaleSidecarStartsFresh) {
    constexpr size_t kSectors = 8192; // 4 MiB
    const auto vol = makeVolume(kSectors, 0x91);
    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume_stale.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    // Forge a stale resume point from a different source.
    {
        std::ofstream p(part, std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> junk(1024 * 512, 0xEE);
        p.write(reinterpret_cast<const char*>(junk.data()), junk.size());
        std::ofstream s(side, std::ios::binary | std::ios::trunc);
        s << "{\n  \"format\": \"raw\",\n"
          << "  \"sourceKey\": \"drive:99:999999999\",\n"
          << "  \"totalBytes\": " << vol.size() << ",\n"
          << "  \"doneBytes\": " << (1024ull * 512ull) << ",\n"
          << "  \"sectorSize\": 512,\n"
          << "  \"md5StateVersion\": 1,\n"
          << "  \"md5StateBase64\": \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\n"
          << "  \"appVersion\": \"byteback-native-1\"\n}\n";
    }

    DiskImager imager;
    std::atomic<bool> done{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Raw);
    for (int i = 0; i < 600 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager.stopImaging();
    ASSERT_TRUE(done.load());

    std::ifstream in(dest, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    ASSERT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol); // fresh full write, none of the junk survived
    EXPECT_FALSE(std::filesystem::exists(part));
    EXPECT_FALSE(std::filesystem::exists(side));
    std::filesystem::remove(dest);
}
