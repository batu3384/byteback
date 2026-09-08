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

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// Cancel a RAW run right after the first chunk (deterministic 512 KiB point).
void cancelAfterFirstChunk(DiskReader& reader, const std::string& dest) {
    DiskImager imager;
    imager.setChunkSectorsForTest(1024);
    std::atomic<bool> finished{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) finished = true;
        else imager.requestStop();
    }, ImageFormat::Raw);
    for (int i = 0; i < 400 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager.stopImaging();
    ASSERT_TRUE(finished.load());
    ASSERT_TRUE(std::filesystem::exists(dest + ".part"));
    ASSERT_TRUE(std::filesystem::exists(dest + ".part.json"));
}

// Run a RAW imaging job to completion on dest.
void runToCompletion(DiskReader& reader, const std::string& dest) {
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
}

// Test-local base64 (mirrors the sidecar encoding).
std::string b64Test(const uint8_t* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (data[i] << 16) | (i + 1 < len ? data[i + 1] << 8 : 0) |
                           (i + 2 < len ? data[i + 2] : 0);
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? T[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? T[v & 63] : '=');
    }
    return out;
}

// Serialize a forged Md5State exactly like disk_imager does (92 bytes LE):
// state[4], count, buffer[64], bufLen.
std::string forgedStateBlob(uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3,
                            uint64_t count, uint32_t bufLen) {
    std::vector<uint8_t> blob(92, 0);
    const uint32_t states[4] = {s0, s1, s2, s3};
    size_t o = 0;
    for (int i = 0; i < 4; ++i)
        for (int b = 0; b < 4; ++b) blob[o++] = static_cast<uint8_t>(states[i] >> (8 * b));
    for (int b = 0; b < 8; ++b) blob[o++] = static_cast<uint8_t>(count >> (8 * b));
    o += 64; // buffer stays zero
    for (int b = 0; b < 4; ++b) blob[o++] = static_cast<uint8_t>(bufLen >> (8 * b));
    return b64Test(blob.data(), blob.size());
}

// Forge a sidecar with verbatim field values (doneBytesVerbatim lets a test
// write negative/non-numeric junk the way a hostile file would).
void forgeSidecar(const std::string& sidePath, const std::string& sourceKey,
                  const std::string& totalBytesVerbatim,
                  const std::string& doneBytesVerbatim,
                  const std::string& md5StateB64) {
    std::ofstream s(sidePath, std::ios::binary | std::ios::trunc);
    s << "{\n  \"format\": \"raw\",\n"
      << "  \"sourceKey\": \"" << sourceKey << "\",\n"
      << "  \"totalBytes\": " << totalBytesVerbatim << ",\n"
      << "  \"doneBytes\": " << doneBytesVerbatim << ",\n"
      << "  \"sectorSize\": 512,\n"
      << "  \"md5StateVersion\": 1,\n"
      << "  \"md5StateBase64\": \"" << md5StateB64 << "\",\n"
      << "  \"appVersion\": \"byteback-native-1\"\n}\n";
    ASSERT_TRUE(s.good());
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

// Forensic rule probe: the sidecar's sourceKey for memory volumes is
// "memory:<size>" — geometry only. Imaging a DIFFERENT source of the same size
// onto the same destination must NOT resume (that would stitch source A's
// prefix onto source B's tail and report a digest over the hybrid). It must
// restart fresh and produce source B, byte-exact.
TEST(DiskImagerResume, SameSizeDifferentSourceSidecarStartsFresh) {
    constexpr size_t kSectors = 8192; // 4 MiB both — sizes collide by design
    const auto volA = makeVolume(kSectors, 0x77);
    const auto volB = makeVolume(kSectors, 0x5C); // different content, same size

    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume_swap.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    // Run 1: source A, cancelled after the first chunk.
    {
        std::vector<uint8_t> copyA = volA;
        DiskReader readerA;
        readerA.attachMemoryVolume(std::move(copyA));
        DiskImager imager;
        imager.setChunkSectorsForTest(1024);
        std::atomic<bool> finished{false};
        imager.startImagingFromReader(readerA, dest, [&](uint64_t cur, uint64_t total) {
            if (cur == total) finished = true;
            else imager.requestStop();
        }, ImageFormat::Raw);
        for (int i = 0; i < 400 && !finished; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        imager.stopImaging();
        ASSERT_TRUE(finished.load());
        ASSERT_TRUE(std::filesystem::exists(part));
        ASSERT_TRUE(std::filesystem::exists(side));
    }

    // Run 2: source B (same size, different bytes) onto the SAME destination.
    std::vector<uint8_t> copyB = volB;
    DiskReader readerB;
    readerB.attachMemoryVolume(std::move(copyB));
    DiskImager imager2;
    std::atomic<bool> done{false};
    imager2.startImagingFromReader(readerB, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Raw);
    for (int i = 0; i < 600 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager2.stopImaging();
    ASSERT_TRUE(done.load());

    std::ifstream in(dest, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    in.close();
    ASSERT_EQ(out.size(), volB.size());
    // The image must be source B in full — never a hybrid of A's prefix.
    EXPECT_EQ(out, volB);
    crypto::Md5 md5;
    md5.update(volB.data(), volB.size());
    EXPECT_EQ(imager2.lastImageMd5(), md5.finalHex());
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

// Hostile/corrupt sidecar matrix. Every variant must either resume
// byte-exact or restart FRESH — none may produce a non-source image or a
// "verified" digest over one. The .part always holds junk a broken resume
// would leak into the output.
TEST(DiskImagerResume, HostileSidecarVariantsNeverCorruptOutput) {
    constexpr size_t kSectors = 8192; // 4 MiB
    const auto vol = makeVolume(kSectors, 0xA7);
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string sourceKey = "memory:" + std::to_string(vol.size());

    struct Variant { const char* name; std::string json; };
    std::vector<Variant> variants;

    // Truncated JSON.
    variants.push_back({"truncated", "{\"format\": \"ra"});
    variants.push_back({"empty", ""});
    // doneBytes beyond the source size.
    variants.push_back({"doneOverTotal",
        "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
        "  \"totalBytes\": " + std::to_string(vol.size()) + ",\n"
        "  \"doneBytes\": " + std::to_string(vol.size() + 512 * 1024) + ",\n"
        "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
        "  \"md5StateBase64\": \"AAAA\",\n  \"appVersion\": \"x\"\n}\n"});
    // Negative doneBytes must not silently parse as positive.
    variants.push_back({"negativeDone",
        "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
        "  \"totalBytes\": " + std::to_string(vol.size()) + ",\n"
        "  \"doneBytes\": -512,\n"
        "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
        "  \"md5StateBase64\": \"AAAA\",\n  \"appVersion\": \"x\"\n}\n"});
    // Non-numeric doneBytes.
    variants.push_back({"nonNumericDone",
        "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
        "  \"totalBytes\": " + std::to_string(vol.size()) + ",\n"
        "  \"doneBytes\": \"abc\",\n"
        "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
        "  \"md5StateBase64\": \"AAAA\",\n  \"appVersion\": \"x\"\n}\n"});
    // Overflowing totalBytes.
    variants.push_back({"totalOverflows",
        "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
        "  \"totalBytes\": 99999999999999999999,\n"
        "  \"doneBytes\": 512,\n"
        "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
        "  \"md5StateBase64\": \"AAAA\",\n  \"appVersion\": \"x\"\n}\n"});
    // Wrong md5State blob length (91 bytes, not 92).
    {
        std::vector<uint8_t> short91(91, 0xAB);
        variants.push_back({"md5StateWrongLength",
            "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
            "  \"totalBytes\": " + std::to_string(vol.size()) + ",\n"
            "  \"doneBytes\": " + std::to_string(512 * 1024) + ",\n"
            "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
            "  \"md5StateBase64\": \"" + b64Test(short91.data(), short91.size()) + "\",\n"
            "  \"appVersion\": \"x\"\n}\n"});
    }
    // Crafted-but-CONSISTENT state (bufLen == count % 64) over a junk .part:
    // the crypto-level consistency check alone accepts it; the recomputed
    // prefix state must not match it.
    {
        const std::string blob = forgedStateBlob(0x67452301, 0xefcdab89, 0x98badcfe,
                                                 0x10325476, 512 * 1024, 0);
        variants.push_back({"craftedConsistentState",
            "{\n  \"format\": \"raw\",\n  \"sourceKey\": \"" + sourceKey + "\",\n"
            "  \"totalBytes\": " + std::to_string(vol.size()) + ",\n"
            "  \"doneBytes\": " + std::to_string(512 * 1024) + ",\n"
            "  \"sectorSize\": 512,\n  \"md5StateVersion\": 1,\n"
            "  \"md5StateBase64\": \"" + blob + "\",\n"
            "  \"appVersion\": \"byteback-native-1\"\n}\n"});
    }

    for (const auto& v : variants) {
        const std::string dest = (tmp / "bb_resume_hostile.raw").string();
        const std::string part = dest + ".part";
        const std::string side = dest + ".part.json";
        std::filesystem::remove(dest);
        std::filesystem::remove(part);
        std::filesystem::remove(side);

        // Junk .part the output must never inherit, plus the hostile sidecar.
        {
            std::ofstream p(part, std::ios::binary | std::ios::trunc);
            std::vector<uint8_t> junk(1024 * 1024, 0xEE);
            p.write(reinterpret_cast<const char*>(junk.data()), junk.size());
        }
        {
            std::ofstream s(side, std::ios::binary | std::ios::trunc);
            s << v.json;
            ASSERT_TRUE(s.good()) << v.name;
        }

        std::vector<uint8_t> copy = vol;
        DiskReader reader;
        reader.attachMemoryVolume(std::move(copy));
        runToCompletion(reader, dest);

        std::vector<uint8_t> out = readFile(dest);
        ASSERT_EQ(out.size(), vol.size()) << v.name;
        EXPECT_EQ(out, vol) << "variant " << v.name << " leaked junk into the image";
        EXPECT_FALSE(std::filesystem::exists(part)) << v.name;
        EXPECT_FALSE(std::filesystem::exists(side)) << v.name;
        std::filesystem::remove(dest);
    }
}

// Crash semantics: the .part is SHORTER than the sidecar's doneBytes (data
// hit disk before the sidecar advanced, or the file was truncated). Resuming
// from the phantom bytes is impossible — the run must restart fresh and still
// deliver the source byte-exact.
TEST(DiskImagerResume, PartShorterThanSidecarRestartsFresh) {
    constexpr size_t kSectors = 8192;
    const auto vol = makeVolume(kSectors, 0xB2);
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume_short.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    {
        std::vector<uint8_t> copy = vol;
        DiskReader reader;
        reader.attachMemoryVolume(std::move(copy));
        cancelAfterFirstChunk(reader, dest);
    }
    // Simulate the torn write: keep the sidecar, shred the .part below it.
    std::filesystem::resize_file(part, 100);

    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));
    runToCompletion(reader, dest);

    const std::vector<uint8_t> out = readFile(dest);
    ASSERT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol);
    EXPECT_FALSE(std::filesystem::exists(part));
    EXPECT_FALSE(std::filesystem::exists(side));
    std::filesystem::remove(dest);
}

// Opposite tear: the .part holds MORE than the sidecar's doneBytes (crash
// between the data write and the sidecar update). The tail was never hashed,
// so resume must truncate to doneBytes and continue — the junk tail may not
// survive into the image.
TEST(DiskImagerResume, PartLongerThanSidecarIsTruncatedAndResumes) {
    constexpr size_t kSectors = 8192;
    const auto vol = makeVolume(kSectors, 0xC3);
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume_long.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    {
        std::vector<uint8_t> copy = vol;
        DiskReader reader;
        reader.attachMemoryVolume(std::move(copy));
        cancelAfterFirstChunk(reader, dest);
    }
    {
        // The un-hashable tail.
        std::ofstream p(part, std::ios::binary | std::ios::app);
        std::vector<uint8_t> junk(256 * 1024, 0x5D);
        p.write(reinterpret_cast<const char*>(junk.data()), junk.size());
        ASSERT_TRUE(p.good());
    }

    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));
    runToCompletion(reader, dest);

    const std::vector<uint8_t> out = readFile(dest);
    ASSERT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol); // truncation happened; no 0x5D tail survived
    EXPECT_FALSE(std::filesystem::exists(part));
    EXPECT_FALSE(std::filesystem::exists(side));
    std::filesystem::remove(dest);
}

// Restart after a self-stop (stopImaging called from inside the progress
// callback): the cancelled run is parked and must be fully reaped before the
// second run starts — two writers on one destination is forbidden. The final
// image stays byte-exact with the source.
TEST(DiskImagerResume, RestartAfterSelfStopProducesByteExactImage) {
    constexpr size_t kSectors = 8192;
    const auto vol = makeVolume(kSectors, 0xD4);
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string dest = (tmp / "bb_resume_selfstop.raw").string();
    const std::string part = dest + ".part";
    const std::string side = dest + ".part.json";
    std::filesystem::remove(dest);
    std::filesystem::remove(part);
    std::filesystem::remove(side);

    std::vector<uint8_t> copy = vol;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(copy));

    DiskImager imager;
    imager.setChunkSectorsForTest(1024);
    std::atomic<bool> finished{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) finished = true;
        else imager.stopImaging(); // self-stop from the callback (parked run)
    }, ImageFormat::Raw);
    for (int i = 0; i < 400 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(finished.load());
    // Foreign stop: joins the parked run before anything else may start.
    imager.stopImaging();

    // Second run on the SAME imager and destination — resumes the .part.
    std::atomic<bool> done{false};
    imager.startImagingFromReader(reader, dest, [&](uint64_t cur, uint64_t total) {
        if (cur == total) done = true;
    }, ImageFormat::Raw);
    for (int i = 0; i < 600 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    imager.stopImaging();
    ASSERT_TRUE(done.load());

    const std::vector<uint8_t> out = readFile(dest);
    ASSERT_EQ(out.size(), vol.size());
    EXPECT_EQ(out, vol);
    EXPECT_FALSE(std::filesystem::exists(part));
    EXPECT_FALSE(std::filesystem::exists(side));
    std::filesystem::remove(dest);
}
