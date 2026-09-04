// CA-001 emission policy: footer-less candidates emit only when a structural
// parser bounds their size. Unbounded candidates (random data after a magic)
// are phantom records and must never reach the results.
#include "byteback_carver.h"
#include "byteback_io.h"
#include "byteback_db.h"
#include "byteback_recovery.h"
#include "carver/structural_parsers.h"
#include "crypto/byteback_md5.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace byteback;

namespace {
struct ScanResult {
    int records = 0;
    std::vector<FileRecord> files;
};

ScanResult runScan(const std::vector<uint8_t>& disk) {
    DiskReader reader;
    reader.attachMemoryVolume(std::move(const_cast<std::vector<uint8_t>&>(disk)));
    CarvingEngine carver;
    EXPECT_TRUE(carver.loadSignatures(""));
    ScanResult out;
    std::atomic<bool> running{true};
    EXPECT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) { ++out.records; out.files.push_back(fr); }
    }, &running));
    return out;
}
} // namespace

TEST(CarvePolicy, FooterlessMagicWithoutParserEmitsNothing) {
    // Matroska + OGG + FLAC + MP3(ID3) magics inside random data: no parser
    // bounds these formats, so the policy must drop all of them.
    std::vector<uint8_t> disk(1 << 20, 0x5A);
    auto put = [&](size_t off, const std::vector<uint8_t>& magic) {
        for (size_t i = 0; i < magic.size(); ++i) disk[off + i] = magic[i];
    };
    put(4096, {0x1A, 0x45, 0xDF, 0xA3});       // MKV
    put(100000, {0x4F, 0x67, 0x67, 0x53});     // OGG
    put(300000, {0x66, 0x4C, 0x61, 0x43});     // FLAC
    put(600000, {0x49, 0x44, 0x33, 0x03});     // MP3 ID3

    const ScanResult r = runScan(disk);
    EXPECT_EQ(r.records, 0);
}

TEST(CarvePolicy, RiffDeclaredSizeBoundsTheCarve) {
    std::vector<uint8_t> disk(4096, 0);
    disk[0] = 'R'; disk[1] = 'I'; disk[2] = 'F'; disk[3] = 'F';
    const uint32_t riffSize = 1000 - 8; // declared size excludes the 8-byte header
    disk[4] = riffSize & 0xFF; disk[5] = (riffSize >> 8) & 0xFF;
    disk[6] = (riffSize >> 16) & 0xFF; disk[7] = (riffSize >> 24) & 0xFF;
    disk[8] = 'W'; disk[9] = 'A'; disk[10] = 'V'; disk[11] = 'E';

    const ScanResult r = runScan(disk);
    ASSERT_EQ(r.records, 1);
    EXPECT_LE(r.files[0].sizeBytes, 1000u);
    EXPECT_GE(r.files[0].sizeBytes, 990u);
}

TEST(CarvePolicy, TiffStripExtentBoundsTheCarve) {
    // Minimal little-endian TIFF: header, IFD0 with 3 entries
    // (273 StripOffsets=2048, 279 StripByteCounts=1024, 256 width), next IFD 0.
    std::vector<uint8_t> disk(8192, 0);
    disk[0] = 'I'; disk[1] = 'I'; disk[2] = 0x2A; disk[3] = 0x00;
    const uint32_t ifd0 = 8;
    disk[4] = ifd0;
    size_t p = ifd0;
    disk[p++] = 3; disk[p++] = 0; // entry count
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        disk[p++] = tag & 0xFF; disk[p++] = (tag >> 8) & 0xFF;
        disk[p++] = type & 0xFF; disk[p++] = (type >> 8) & 0xFF;
        for (int i = 0; i < 4; ++i) disk[p++] = (count >> (8 * i)) & 0xFF;
        for (int i = 0; i < 4; ++i) disk[p++] = (value >> (8 * i)) & 0xFF;
    };
    entry(273, 4, 1, 2048); // StripOffsets -> strip at 2048
    entry(279, 4, 1, 1024); // StripByteCounts -> 1024 bytes
    entry(256, 4, 1, 640);  // ImageWidth
    disk[p++] = 0; disk[p++] = 0; disk[p++] = 0; disk[p++] = 0; // next IFD
    // Fill the strip with nonzero image data.
    for (int i = 0; i < 1024; ++i) disk[2048 + i] = 0xAB;

    const ScanResult r = runScan(disk);
    ASSERT_EQ(r.records, 1);
    EXPECT_EQ(r.files[0].sizeBytes, 2048u + 1024u);
    EXPECT_EQ(r.files[0].extension, "tiff");
    EXPECT_GE(r.files[0].confidence, 80);
}

TEST(CarvePolicy, BoundedSizeParsersUnitCheck) {
    // RIFF
    const uint32_t riffSize = 1000 - 8; // declared size excludes the 8-byte header
    uint8_t riff[16] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E'};
    for (int i = 0; i < 4; ++i) riff[4 + i] = (riffSize >> (8 * i)) & 0xFF;
    auto pr = byteback::carver::parseRiff(riff, sizeof(riff));
    ASSERT_TRUE(pr.valid);
    EXPECT_EQ(pr.size, 1000u);

    // 7-Zip: StartHeader at offset 12.
    uint8_t sz[40] = {'7','z',0xBC,0xAF,0x27,0x1C, 0,4, 0,0,0,0};
    auto nho = static_cast<uint64_t>(64), nhs = static_cast<uint64_t>(128);
    for (int i = 0; i < 8; ++i) sz[12 + i] = static_cast<uint8_t>(nho >> (8 * i));
    for (int i = 0; i < 8; ++i) sz[20 + i] = static_cast<uint8_t>(nhs >> (8 * i));
    pr = byteback::carver::parseSevenZip(sz, sizeof(sz));
    ASSERT_TRUE(pr.valid);
    EXPECT_EQ(pr.size, 32 + 64 + 128);

    // CAB
    uint8_t cab[32] = {'M','S','C','F', 0,0,0,0};
    const uint32_t cb = 512;
    for (int i = 0; i < 4; ++i) cab[8 + i] = (cb >> (8 * i)) & 0xFF;
    pr = byteback::carver::parseCab(cab, sizeof(cab));
    ASSERT_TRUE(pr.valid);
    EXPECT_EQ(pr.size, 512u);
}

namespace {
std::vector<uint8_t> minimalJpeg() {
    return {0xFF,0xD8,0xFF,0xDB, 0x00,0x03,0x00, 0xFF,0xDA,0x00,0x02,
            0x11,0x22,0x33,0x44,0x55, 0xFF,0xD9};
}
} // namespace

// CA-006: a file whose header sits one band and whose footer sits in the next
// band was truncated at the old 4-worker band edge. Sequential scanning must
// recover it whole.
TEST(CarvePolicy, FileCrossingOldBandEdgeRecoveredWhole) {
    constexpr size_t kDisk = 64u * 1024 * 1024; // old: 4 bands of 16 MiB
    const size_t off = 16u * 1024 * 1024 - 8192; // header in band 0, footer in band 1

    std::vector<uint8_t> disk(kDisk, 0);
    static const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t iend[] = {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    std::memcpy(disk.data() + off, sig, sizeof(sig));
    std::memcpy(disk.data() + off + 4096, iend, sizeof(iend));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension.find("png") != std::string::npos) found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].sizeBytes, 4096u + sizeof(iend));
}

// CA-004/CA-005: a JPEG starting mid-sector must be validated, recorded with
// its true byte offset, and recovered byte-exact (previously the recovery
// read from the sector floor and produced shifted garbage).
TEST(CarvePolicy, UnalignedCarveRecoversByteExact) {
    const auto jpeg = minimalJpeg();
    const size_t kOffset = 300; // mid-sector

    std::vector<uint8_t> disk(8192, 0x77);
    std::memcpy(disk.data() + kOffset, jpeg.data(), jpeg.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension == "jpg") found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].startByteOffset, kOffset);
    EXPECT_EQ(found[0].startSector, 0u);
    EXPECT_EQ(found[0].sizeBytes, jpeg.size());

    // Recovery must reproduce the exact source bytes.
    std::filesystem::path dest = std::filesystem::temp_directory_path() / "byteback_carve_policy";
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, found[0], dest.string(), nullptr, nullptr);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.bytesRecovered, jpeg.size());

    crypto::Md5 expected;
    expected.update(jpeg.data(), jpeg.size());
    EXPECT_EQ(result.md5Hash, expected.finalHex());
    std::filesystem::remove_all(dest);
}

// The byte offset must survive the DB roundtrip or resumed sessions would
// recover shifted files.
TEST(CarvePolicy, StartByteOffsetRoundTripsThroughDb) {
    const auto dbPath = (std::filesystem::temp_directory_path() /
                         "byteback_carve_policy.db").string();
    std::filesystem::remove(dbPath);

    MetadataStore store;
    ASSERT_TRUE(store.open(dbPath));

    FileRecord rec;
    rec.name = "mid.jpg";
    rec.sizeBytes = 1234;
    rec.startSector = 7;
    rec.startByteOffset = 300;
    rec.endSector = 10;
    rec.status = 0;
    rec.confidence = 85;
    rec.source = "carver";

    const int64_t scanId = store.createScan(0, "deep", 100);
    ASSERT_GT(scanId, 0);
    ASSERT_GT(store.insertFile(scanId, rec), 0);

    const auto loaded = store.getFileById(store.getLatestScanId(), scanId);
    ASSERT_GE(loaded.id, 0);
    EXPECT_EQ(loaded.startByteOffset, 300u);

    store.close();
    std::filesystem::remove(dbPath);
}
