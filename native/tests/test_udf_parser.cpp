#include "fs/udf_parser.h"
#include "fs/partition_scanner.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include "scan_coordinator.h"
#include "test_temp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace byteback;

namespace {

constexpr uint32_t kBs = 2048;

void wrLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wrLe32(uint8_t* p, uint32_t v) {
    wrLe16(p, static_cast<uint16_t>(v));
    wrLe16(p + 2, static_cast<uint16_t>(v >> 16));
}
void wrLe64(uint8_t* p, uint64_t v) {
    wrLe32(p, static_cast<uint32_t>(v));
    wrLe32(p + 4, static_cast<uint32_t>(v >> 32));
}

void plantVrs(uint8_t* sec, const char id[5]) {
    std::memset(sec, 0, kBs);
    sec[0] = 0;
    std::memcpy(sec + 1, id, 5);
    sec[6] = 1;
}

void sealTag(uint8_t* desc, uint16_t ident, uint32_t loc, uint16_t totalLen) {
    wrLe16(desc + 0, ident);
    wrLe16(desc + 2, 2);
    desc[4] = 0;
    desc[5] = 0;
    wrLe16(desc + 6, 0);
    wrLe16(desc + 12, 0); // location lo patched below
    wrLe32(desc + 12, loc);
    const uint16_t crcLen = static_cast<uint16_t>(totalLen - 16);
    wrLe16(desc + 10, crcLen);
    wrLe16(desc + 8, udfCrc16(desc + 16, crcLen));
    uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) {
        if (i != 4) sum = static_cast<uint8_t>(sum + desc[i]);
    }
    desc[4] = sum;
}

void plantLongAd(uint8_t* p, uint32_t len, uint32_t lbn, uint16_t part) {
    wrLe32(p, len);
    wrLe32(p + 4, lbn);
    wrLe16(p + 8, part);
}

void plantFid(uint8_t* p, uint32_t loc, uint8_t chars, uint32_t icbLbn, const char* name,
              uint16_t part = 0) {
    const uint8_t nameLen = name ? static_cast<uint8_t>(std::strlen(name)) : 0;
    const uint8_t idLen = nameLen ? static_cast<uint8_t>(nameLen + 1) : 0;
    uint16_t total = static_cast<uint16_t>(38 + idLen);
    if (total % 4) total = static_cast<uint16_t>((total + 3) & ~3);
    std::memset(p, 0, total);
    wrLe16(p + 16, 1); // version
    p[18] = chars;
    p[19] = idLen;
    plantLongAd(p + 20, kBs, icbLbn, part);
    wrLe16(p + 36, 0);
    if (idLen) {
        p[38] = 8;
        std::memcpy(p + 39, name, nameLen);
    }
    sealTag(p, 257, loc, total);
}

std::vector<uint8_t> buildUdfVrs() {
    std::vector<uint8_t> img(32 * kBs, 0);
    plantVrs(img.data() + 16 * kBs, "BEA01");
    plantVrs(img.data() + 17 * kBs, "NSR02");
    plantVrs(img.data() + 18 * kBs, "TEA01");
    return img;
}

std::vector<uint8_t> buildMinimalUdf(bool shortAd = false, bool efe = false, bool longAd = false) {
    constexpr uint32_t kBlocks = 280;
    constexpr uint32_t kAvdp = 256;
    constexpr uint32_t kVds = 32;
    constexpr uint32_t kFsd = 40;
    constexpr uint32_t kRoot = 41;
    constexpr uint32_t kFile = 43;
    constexpr uint32_t kData = 44;
    const char payload[] = "hello udf\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    std::vector<uint8_t> img(kBlocks * kBs, 0);
    plantVrs(img.data() + 16 * kBs, "BEA01");
    plantVrs(img.data() + 17 * kBs, "NSR02");
    plantVrs(img.data() + 18 * kBs, "TEA01");

    uint8_t* avdp = img.data() + kAvdp * kBs;
    wrLe32(avdp + 16, 4 * kBs);
    wrLe32(avdp + 20, kVds);
    sealTag(avdp, 2, kAvdp, 512);

    uint8_t* pd = img.data() + (kVds + 1) * kBs;
    wrLe32(pd + 16, 1);
    wrLe16(pd + 22, 0);
    wrLe32(pd + 188, 0);
    wrLe32(pd + 192, kBlocks);
    sealTag(pd, 5, kVds + 1, kBs);

    uint8_t* lvd = img.data() + (kVds + 2) * kBs;
    wrLe32(lvd + 16, 2);
    wrLe32(lvd + 212, kBs);
    plantLongAd(lvd + 248, kBs, kFsd, 0);
    sealTag(lvd, 6, kVds + 2, kBs);

    uint8_t* term = img.data() + (kVds + 3) * kBs;
    sealTag(term, 8, kVds + 3, kBs);

    uint8_t* fsd = img.data() + kFsd * kBs;
    plantLongAd(fsd + 400, kBs, kRoot, 0);
    sealTag(fsd, 256, kFsd, kBs);

    uint8_t parentFid[64];
    uint8_t fileFid[64];
    plantFid(parentFid, kRoot, 8, kRoot, nullptr);
    plantFid(fileFid, kRoot, 0, kFile, "HELLO.TXT");
    constexpr uint16_t kParentFidLen = 40;
    constexpr uint16_t kFileFidLen = 48;
    uint8_t* root = img.data() + kRoot * kBs;
    root[16 + 11] = 4; // icb fileType directory
    wrLe16(root + 16 + 18, 3); // AD_IN_ICB
    wrLe16(root + 48, 1); // link count
    wrLe64(root + 56, kParentFidLen + kFileFidLen);
    wrLe32(root + 172, kParentFidLen + kFileFidLen);
    std::memcpy(root + 176, parentFid, kParentFidLen);
    std::memcpy(root + 176 + kParentFidLen, fileFid, kFileFidLen);
    sealTag(root, 261, kRoot, kBs);

    uint8_t* fe = img.data() + kFile * kBs;
    fe[16 + 11] = 5;
    wrLe16(fe + 16 + 18, longAd ? 1 : (shortAd ? 0 : 3));
    wrLe16(fe + 48, 1);
    wrLe64(fe + 56, payloadLen);
    const uint32_t adOff = efe ? 212u : 172u;
    const uint32_t bodyOff = efe ? 216u : 176u;
    if (longAd) {
        wrLe32(fe + adOff, 16);
        plantLongAd(fe + bodyOff, payloadLen, kData, 0);
        std::memcpy(img.data() + kData * kBs, payload, payloadLen);
    } else if (shortAd) {
        wrLe32(fe + adOff, 8);
        wrLe32(fe + bodyOff, payloadLen);
        wrLe32(fe + bodyOff + 4, kData);
        std::memcpy(img.data() + kData * kBs, payload, payloadLen);
    } else {
        wrLe32(fe + adOff, payloadLen);
        std::memcpy(fe + bodyOff, payload, payloadLen);
    }
    sealTag(fe, efe ? 266 : 261, kFile, kBs);
    return img;
}

std::vector<uint8_t> buildMinimalUdfWithVat() {
    auto img = buildMinimalUdf();
    constexpr uint32_t kRoot = 41;
    constexpr uint32_t kPhysRoot = 200;
    constexpr uint32_t kVatEntries = 42;
    const uint32_t last = static_cast<uint32_t>(img.size() / kBs - 1);
    std::memcpy(img.data() + kPhysRoot * kBs, img.data() + kRoot * kBs, kBs);
    std::memset(img.data() + kRoot * kBs, 0, kBs);
    uint8_t* vat = img.data() + last * kBs;
    vat[16 + 11] = 248; // ICB fileType VAT
    wrLe16(vat + 16 + 18, 3);
    wrLe16(vat + 48, 1);
    wrLe64(vat + 56, kVatEntries * 4u);
    wrLe32(vat + 172, kVatEntries * 4u);
    uint8_t* body = vat + 176;
    for (uint32_t i = 0; i < kVatEntries; ++i) wrLe32(body + i * 4, 0xFFFFFFFFu);
    wrLe32(body + 41 * 4, kPhysRoot);
    sealTag(vat, 261, last, kBs);
    return img;
}

std::vector<uint8_t> buildMinimalUdfWithMetadata() {
    auto img = buildMinimalUdf();
    constexpr uint32_t kFsd = 40;
    constexpr uint32_t kRoot = 41;
    constexpr uint32_t kFile = 43;
    constexpr uint32_t kMetaFe = 50;
    constexpr uint32_t kMetaData = 210;
    constexpr uint32_t kVds = 32;
    std::memcpy(img.data() + (kMetaData + 0) * kBs, img.data() + kFsd * kBs, kBs);
    std::memcpy(img.data() + (kMetaData + 1) * kBs, img.data() + kRoot * kBs, kBs);
    std::memcpy(img.data() + (kMetaData + 3) * kBs, img.data() + kFile * kBs, kBs);
    std::memset(img.data() + kFsd * kBs, 0, kBs);
    std::memset(img.data() + kRoot * kBs, 0, kBs);
    std::memset(img.data() + kFile * kBs, 0, kBs);

    uint8_t* fsd = img.data() + kMetaData * kBs;
    plantLongAd(fsd + 400, kBs, 1, 1);
    sealTag(fsd, 256, 0, kBs);

    uint8_t parentFid[64];
    uint8_t fileFid[64];
    plantFid(parentFid, 1, 8, 1, nullptr, 1);
    plantFid(fileFid, 1, 0, 3, "HELLO.TXT", 1);
    constexpr uint16_t kParentFidLen = 40;
    constexpr uint16_t kFileFidLen = 48;
    uint8_t* root = img.data() + (kMetaData + 1) * kBs;
    std::memcpy(root + 176, parentFid, kParentFidLen);
    std::memcpy(root + 176 + kParentFidLen, fileFid, kFileFidLen);
    sealTag(root, 261, 1, kBs);

    uint8_t* fileFe = img.data() + (kMetaData + 3) * kBs;
    sealTag(fileFe, 261, 3, kBs);

    uint8_t* meta = img.data() + kMetaFe * kBs;
    meta[16 + 11] = 250; // metadata file
    wrLe16(meta + 16 + 18, 0); // short_ad
    wrLe16(meta + 48, 1);
    wrLe64(meta + 56, 4u * kBs);
    wrLe32(meta + 172, 8);
    wrLe32(meta + 176, 4u * kBs);
    wrLe32(meta + 180, kMetaData);
    sealTag(meta, 261, kMetaFe, kBs);

    uint8_t* lvd = img.data() + (kVds + 2) * kBs;
    plantLongAd(lvd + 248, kBs, 0, 1);
    wrLe32(lvd + 264, 70);
    wrLe32(lvd + 268, 2);
    uint8_t* maps = lvd + 440;
    maps[0] = 1;
    maps[1] = 6;
    wrLe16(maps + 2, 1);
    wrLe16(maps + 4, 0);
    uint8_t* m2 = maps + 6;
    m2[0] = 2;
    m2[1] = 64;
    std::memcpy(m2 + 5, "*UDF Metadata Partition", 23);
    wrLe16(m2 + 36, 1);
    wrLe16(m2 + 38, 0);
    wrLe32(m2 + 40, kMetaFe);
    sealTag(lvd, 6, kVds + 2, kBs);
    return img;
}

std::vector<uint8_t> buildMinimalUdfWithMetadataOrphan() {
    auto img = buildMinimalUdfWithMetadata();
    constexpr uint32_t kMetaData = 210;
    constexpr uint32_t kBmpFe = 52;
    constexpr uint32_t kVds = 32;
    const char payload[] = "orphan udf\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    uint8_t* orphan = img.data() + (kMetaData + 2) * kBs;
    orphan[16 + 11] = 5;
    wrLe16(orphan + 16 + 18, 3);
    wrLe16(orphan + 48, 1);
    wrLe64(orphan + 56, payloadLen);
    wrLe32(orphan + 172, payloadLen);
    std::memcpy(orphan + 176, payload, payloadLen);
    sealTag(orphan, 261, 2, kBs);

    uint8_t* bmp = img.data() + kBmpFe * kBs;
    bmp[16 + 11] = 252;
    wrLe16(bmp + 16 + 18, 3);
    wrLe16(bmp + 48, 1);
    wrLe64(bmp + 56, 1);
    wrLe32(bmp + 172, 1);
    bmp[176] = 0x0F; // LBNs 0..3 allocated
    sealTag(bmp, 261, kBmpFe, kBs);

    uint8_t* lvd = img.data() + (kVds + 2) * kBs;
    wrLe32(lvd + 440 + 6 + 48, kBmpFe);
    sealTag(lvd, 6, kVds + 2, kBs);
    return img;
}

std::vector<uint8_t> buildMinimalUdfWithMetadataMirror() {
    auto img = buildMinimalUdfWithMetadata();
    constexpr uint32_t kMetaFe = 50;
    constexpr uint32_t kMirrorFe = 51;
    constexpr uint32_t kVds = 32;
    std::memcpy(img.data() + kMirrorFe * kBs, img.data() + kMetaFe * kBs, kBs);
    uint8_t* mir = img.data() + kMirrorFe * kBs;
    mir[16 + 11] = 251;
    sealTag(mir, 261, kMirrorFe, kBs);
    std::memset(img.data() + kMetaFe * kBs, 0, kBs);
    uint8_t* lvd = img.data() + (kVds + 2) * kBs;
    wrLe32(lvd + 440 + 6 + 44, kMirrorFe);
    sealTag(lvd, 6, kVds + 2, kBs);
    return img;
}

} // namespace

TEST(Udf, ProbeFindsBea01AtSector16) {
    auto img = buildUdfVrs();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Udf);
}

TEST(Udf, ListsHelloTxt) {
    auto img = buildMinimalUdf();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Udf);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool found = false;
    for (const auto& n : names) {
        if (n.find("HELLO") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Udf, LogicalVolumeIdentifierEmitsDiscovery) {
    auto img = buildMinimalUdf();
    constexpr uint32_t kVds = 32;
    uint8_t* lvd = img.data() + (kVds + 2) * kBs;
    lvd[84] = 8;
    std::memcpy(lvd + 85, "BYTEBACK", 8);
    lvd[84 + 127] = 9;
    sealTag(lvd, 6, kVds + 2, kBs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "udf_vol_id" && fr.name == "BYTEBACK") found = true;
    }, &running));
    EXPECT_TRUE(found) << "LVD Logical Volume Identifier dstring @+84 must emit udf_vol_id";
}

TEST(Udf, PrimaryVolumeIdentifierEmitsDiscovery) {
    auto img = buildMinimalUdf();
    constexpr uint32_t kVds = 32;
    uint8_t* pvd = img.data() + kVds * kBs;
    std::memset(pvd, 0, kBs);
    pvd[24] = 8;
    std::memcpy(pvd + 25, "BYTEBACK", 8);
    pvd[24 + 31] = 9;
    sealTag(pvd, 1, kVds, kBs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "udf_pvd_id" && fr.name == "BYTEBACK") found = true;
    }, &running));
    EXPECT_TRUE(found) << "UDF PVD Volume Identifier dstring @+24 must emit udf_pvd_id";
}

TEST(Udf, FileSetIdentifierEmitsDiscovery) {
    auto img = buildMinimalUdf();
    constexpr uint32_t kFsd = 40;
    uint8_t* fsd = img.data() + kFsd * kBs;
    fsd[304] = 8;
    std::memcpy(fsd + 305, "BYTEBACK", 8);
    fsd[304 + 31] = 9;
    sealTag(fsd, 256, kFsd, kBs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "udf_fsd_id" && fr.name == "BYTEBACK") found = true;
    }, &running));
    EXPECT_TRUE(found) << "UDF FSD File Set Identifier dstring @+304 must emit udf_fsd_id";
}

TEST(Udf, FileEntryModificationTimeSetsModifiedAt) {
    auto img = buildMinimalUdf();
    constexpr uint32_t kFile = 43;
    uint8_t* fe = img.data() + kFile * kBs;
    wrLe16(fe + 84, 0);
    wrLe16(fe + 86, 2026);
    fe[88] = 9;
    fe[89] = 18;
    fe[90] = 12;
    fe[91] = 0;
    fe[92] = 0;
    sealTag(fe, 261, kFile, kBs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    int64_t got = 0;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) got = fr.modifiedAt;
    }, &running));
    EXPECT_EQ(got, 1789732800) << "UDF File Entry Modification Date @+84 must set modifiedAt";
}

TEST(Udf, ExtendedFileEntryCreateTimeSetsCreatedAt) {
    auto img = buildMinimalUdf(false, true);
    constexpr uint32_t kFile = 43;
    uint8_t* fe = img.data() + kFile * kBs;
    wrLe16(fe + 104, 0);
    wrLe16(fe + 106, 2020);
    fe[108] = 1;
    fe[109] = 2;
    fe[110] = 0;
    fe[111] = 0;
    fe[112] = 0;
    sealTag(fe, 266, kFile, kBs);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    int64_t got = 0;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) got = fr.createdAt;
    }, &running));
    EXPECT_EQ(got, 1577923200) << "UDF EFE Create Date @+104 must set createdAt";
}

TEST(Udf, QuickScanFindsHelloTxt) {
    auto img = buildMinimalUdf();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    runQuickScan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, [&](uint64_t, uint64_t) {}, &running, nullptr);
    bool found = false;
    for (const auto& n : names) {
        if (n.find("HELLO") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Udf, RecoversHelloTxtBytes) {
    auto img = buildMinimalUdf();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_hello_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversEfeAdInIcbBytes) {
    auto img = buildMinimalUdf(false, true);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_efe_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversShortAdExtentBytes) {
    auto img = buildMinimalUdf(true);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_shortad_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversFromBackupAvdp) {
    auto img = buildMinimalUdf();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kAvdp = 256;
    const uint32_t last = static_cast<uint32_t>(img.size() / kBs - 1);
    const uint32_t backup = last - 256;
    ASSERT_GT(backup, 18u);
    std::memcpy(img.data() + backup * kBs, img.data() + kAvdp * kBs, 512);
    sealTag(img.data() + backup * kBs, 2, backup, 512);
    std::memset(img.data() + kAvdp * kBs, 0, kBs);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_avdp_bak_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversLongAdExtentBytes) {
    auto img = buildMinimalUdf(false, false, true);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_longad_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversHelloTxtViaVat) {
    auto img = buildMinimalUdfWithVat();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_vat_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversHelloTxtViaMetadataPartition) {
    auto img = buildMinimalUdfWithMetadata();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_meta_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversHelloTxtViaMetadataMirror) {
    auto img = buildMinimalUdfWithMetadataMirror();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_udf_meta_mir_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("hello udf\n"));
    std::filesystem::remove_all(dir);
}

TEST(Udf, RecoversOrphanViaMetadataBitmap) {
    auto img = buildMinimalUdfWithMetadataOrphan();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    UdfParser udf;
    ASSERT_TRUE(udf.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "udf_meta_orphan" ||
            (!fr.residentData.empty() &&
             std::string(fr.residentData.begin(), fr.residentData.end()).find("orphan") !=
                 std::string::npos))
            hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty()) << "bitmap-allocated metadata FE not in directory must emit";
    const auto dir = bytebackTestTemp("bb_udf_meta_bmp_dir");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    RecoveryEngine engine;
    auto res = engine.recoverFile(reader, hit, dir.string());
    ASSERT_TRUE(res.success) << res.error;
    std::string got;
    {
        std::ifstream in(res.destPath, std::ios::binary);
        got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(got, std::string("orphan udf\n"));
    std::filesystem::remove_all(dir);
}
