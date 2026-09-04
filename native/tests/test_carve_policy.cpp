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
#include <fstream>
#include <functional>
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
    EXPECT_EQ(r.records, 0)
        << (r.files.empty() ? std::string() : r.files[0].extension + " @" + std::to_string(r.files[0].startSector));
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

// CA-007: reads past the range end fail on physical/image backends, which
// used to silently skip the last partial chunk of every range. A file living
// in the tail chunk must still be carved.
TEST(CarvePolicy, TailChunkPastEndStillScanned) {
    const auto path = (std::filesystem::temp_directory_path() / "byteback_tail_chunk.raw");
    // 5 MiB + 1.5 KiB: the second 4MiB chunk is partial.
    std::vector<uint8_t> img(5u * 1024 * 1024 + 512 * 3, 0);
    const size_t off = img.size() - 512;
    static const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t iend[] = {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    std::memcpy(img.data() + off, sig, sizeof(sig));
    std::memcpy(img.data() + off + 492, iend, sizeof(iend));

    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachRawFile(path.string(), &err)) << err;

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension.find("png") != std::string::npos) found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].sizeBytes, 492u + sizeof(iend));
    reader.detachImageBackend(); // release the file lock before removal
    std::filesystem::remove(path);
}

// Matroska Segment vint bounds the file exactly — MKV/WebM were dormant
// (never emitted) before a bounded parser existed.
TEST(CarvePolicy, MkvSegmentVintBoundsTheCarve) {
    // EBML header: ID(4) + size vint 0x88 (8 bytes) + 8 bytes = 13 bytes.
    // Segment at offset 13: ID 18538067 + 2-byte vint 0x43E8 (=1000).
    std::vector<uint8_t> disk(8192, 0);
    disk[0] = 0x1A; disk[1] = 0x45; disk[2] = 0xDF; disk[3] = 0xA3;
    disk[4] = 0x88;
    for (int i = 5; i < 13; ++i) disk[i] = 0x42; // 8 bytes of header payload
    disk[13] = 0x18; disk[14] = 0x53; disk[15] = 0x80; disk[16] = 0x67;
    disk[17] = 0x43; disk[18] = 0xE8; // vint: 1000
    const uint64_t kTotal = 19ull + 1000;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension.find("mkv") != std::string::npos) found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].sizeBytes, kTotal);
    EXPECT_GE(found[0].confidence, 75);
}

TEST(CarvePolicy, OggPageWalkBoundsTheCarve) {
    // Two CRC-valid pages: page1 27+1+100 (BOS), page2 27+1+50 (EOS).
    auto oggCrc = [](const std::vector<uint8_t>& d) {
        uint32_t crc = 0;
        for (uint8_t byte : d) {
            crc ^= static_cast<uint32_t>(byte) << 24;
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04c11db7u) : (crc << 1);
            }
        }
        return crc;
    };

    std::vector<uint8_t> disk(8192, 0);
    size_t off = 0;
    auto page = [&](uint8_t flags, uint8_t lacing) {
        std::vector<uint8_t> p(27 + 1 + lacing, 0);
        p[0] = 'O'; p[1] = 'g'; p[2] = 'g'; p[3] = 'S';
        p[4] = 0;        // version
        p[5] = flags;    // 0x02 BOS / 0x04 EOS
        p[26] = 1;       // one segment
        p[27] = lacing;
        const uint32_t crc = oggCrc(p);
        for (int i = 0; i < 4; ++i) p[22 + i] = static_cast<uint8_t>((crc >> (8 * i)) & 0xFF);
        std::memcpy(disk.data() + off, p.data(), p.size());
        off += p.size();
    };
    page(0x02, 100);
    page(0x04, 50);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension.find("ogg") != std::string::npos) found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].sizeBytes, off);
}

namespace {
std::vector<uint8_t> minimalJpeg() {
    return {0xFF,0xD8,0xFF,0xDB, 0x00,0x03,0x00, 0xFF,0xDA,0x00,0x02,
            0x11,0x22,0x33,0x44,0x55, 0xFF,0xD9};
}
} // namespace

// CA-018: a 2MB mdat must carve at full size — the old probe-only walk
// clamped every footer-less mp4 to the 1MB probe window.
TEST(CarvePolicy, LargeMdatMp4CarvesFullSize) {
    const uint32_t kMdatPayload = 2u * 1024 * 1024;
    const size_t kTotal = 24 + 8 + kMdatPayload; // ftyp box + mdat box

    std::vector<uint8_t> disk(3u * 1024 * 1024, 0);
    // ftyp box: size 24, 'ftyp', brand 'isom'
    disk[0] = 0; disk[1] = 0; disk[2] = 0; disk[3] = 24;
    std::memcpy(disk.data() + 4, "ftypisom", 8);
    // mdat box at offset 24: size = payload + 8
    const uint64_t mdatSize = kMdatPayload + 8;
    disk[24] = static_cast<uint8_t>((mdatSize >> 24) & 0xFF);
    disk[25] = static_cast<uint8_t>((mdatSize >> 16) & 0xFF);
    disk[26] = static_cast<uint8_t>((mdatSize >> 8) & 0xFF);
    disk[27] = static_cast<uint8_t>(mdatSize & 0xFF);
    std::memcpy(disk.data() + 28, "mdat", 4);
    std::memset(disk.data() + 32, 0xAB, kMdatPayload);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    std::vector<FileRecord> found;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1 && fr.extension.find("mp4") != std::string::npos) found.push_back(fr);
    }, &running));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].sizeBytes, kTotal);
    EXPECT_GE(found[0].confidence, 75);
}

TEST(CarvePolicy, IsobmffBoundedWalksBeyondProbe) {
    // ftyp(24) + mdat(1MB payload) laid out in a 2MB buffer acting as "disk";
    // the probe window is only the first 4KB — the walk must fetch the mdat
    // header beyond it and bound the file at its true end.
    const uint32_t kMdatPayload = 1024u * 1024;
    std::vector<uint8_t> media(24 + 8 + kMdatPayload, 0xAB);
    media[0] = 0; media[1] = 0; media[2] = 0; media[3] = 24;
    std::memcpy(media.data() + 4, "ftypisom", 8);
    const uint64_t mdatSize = kMdatPayload + 8;
    media[24] = static_cast<uint8_t>((mdatSize >> 24) & 0xFF);
    media[25] = static_cast<uint8_t>((mdatSize >> 16) & 0xFF);
    media[26] = static_cast<uint8_t>((mdatSize >> 8) & 0xFF);
    media[27] = static_cast<uint8_t>(mdatSize & 0xFF);
    std::memcpy(media.data() + 28, "mdat", 4);

    auto readAt = [&](uint64_t off, uint32_t len, uint8_t* out) {
        if (off + len > media.size()) return false;
        std::memcpy(out, media.data() + off, len);
        return true;
    };

    auto pr = byteback::carver::parseIsobmffBounded(media.data(), 4096, 0, 1ull << 30, readAt);
    ASSERT_TRUE(pr.valid);
    EXPECT_EQ(pr.size, media.size());
    EXPECT_GE(pr.confidence, 80);
}

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
