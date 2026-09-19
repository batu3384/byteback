#include "fs/iso9660_parser.h"
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

void wrBoth16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void wrBoth32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
    p[4] = static_cast<uint8_t>(v >> 24);
    p[5] = static_cast<uint8_t>(v >> 16);
    p[6] = static_cast<uint8_t>(v >> 8);
    p[7] = static_cast<uint8_t>(v);
}

// ECMA-119 directory record. idLen 1 + id[0]==0/1 is . / ..
size_t plantDirRecord(uint8_t* p, uint32_t lba, uint32_t dataLen, uint8_t flags,
                      const uint8_t* id, uint8_t idLen, const char* rrNm = nullptr,
                      const char* rrSl = nullptr, const uint8_t* rrTf7 = nullptr) {
    size_t recLen = static_cast<size_t>(33) + idLen;
    if ((recLen & 1u) != 0) ++recLen;
    const uint8_t nmLen = rrNm ? static_cast<uint8_t>(5 + std::strlen(rrNm)) : 0;
    const uint8_t slComp = rrSl ? static_cast<uint8_t>(std::strlen(rrSl)) : 0;
    const uint8_t slLen = rrSl ? static_cast<uint8_t>(5 + 2 + slComp) : 0;
    const uint8_t tfLen = rrTf7 ? 12 : 0;
    if (nmLen) recLen += nmLen;
    if (slLen) recLen += slLen;
    if (tfLen) recLen += tfLen;
    if ((recLen & 1u) != 0) ++recLen;
    p[0] = static_cast<uint8_t>(recLen);
    p[1] = 0;
    wrBoth32(p + 2, lba);
    wrBoth32(p + 10, dataLen);
    p[25] = flags;
    wrBoth16(p + 28, 1);
    p[32] = idLen;
    if (idLen) std::memcpy(p + 33, id, idLen);
    size_t sua = 33u + idLen;
    if ((sua & 1u) != 0) ++sua;
    if (nmLen) {
        p[sua] = 'N';
        p[sua + 1] = 'M';
        p[sua + 2] = nmLen;
        p[sua + 3] = 1;
        p[sua + 4] = 0;
        std::memcpy(p + sua + 5, rrNm, nmLen - 5);
        sua += nmLen;
    }
    if (slLen) {
        p[sua] = 'S';
        p[sua + 1] = 'L';
        p[sua + 2] = slLen;
        p[sua + 3] = 1;
        p[sua + 4] = 0;
        p[sua + 5] = 0;
        p[sua + 6] = slComp;
        std::memcpy(p + sua + 7, rrSl, slComp);
        sua += slLen;
    }
    if (tfLen) {
        p[sua] = 'T';
        p[sua + 1] = 'F';
        p[sua + 2] = tfLen;
        p[sua + 3] = 1;
        p[sua + 4] = 0x02;
        std::memcpy(p + sua + 5, rrTf7, 7);
    }
    return recLen;
}

std::vector<uint8_t> buildMinimalIso9660() {
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kBlocks = 24;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kFileLba = 21;
    const char payload[] = "hello iso\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    std::vector<uint8_t> img(kBlocks * kBs, 0);

    uint8_t* pvd = img.data() + 16 * kBs;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memcpy(pvd + 40, "TESTISO", 7);
    wrBoth32(pvd + 80, kBlocks);
    wrBoth16(pvd + 128, static_cast<uint16_t>(kBs));

    uint8_t* root = img.data() + kRootLba * kBs;
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    off += plantDirRecord(root + off, kFileLba, payloadLen, 0, fid, static_cast<uint8_t>(sizeof(fid)));
    (void)off;

    plantDirRecord(pvd + 156, kRootLba, kBs, 2, &dot, 1);
    std::memcpy(img.data() + kFileLba * kBs, payload, payloadLen);
    return img;
}

std::vector<uint8_t> buildMultiExtentIso() {
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kBlocks = 24;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kExt1 = 21;
    constexpr uint32_t kExt2 = 23;
    const char a[] = "hello";
    const char b[] = " iso\n";
    std::vector<uint8_t> img(kBlocks * kBs, 0);
    uint8_t* pvd = img.data() + 16 * kBs;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memcpy(pvd + 40, "TESTISO", 7);
    wrBoth32(pvd + 80, kBlocks);
    wrBoth16(pvd + 128, static_cast<uint16_t>(kBs));
    uint8_t* root = img.data() + kRootLba * kBs;
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    off += plantDirRecord(root + off, kExt1, 5, 0x80, fid, static_cast<uint8_t>(sizeof(fid)));
    off += plantDirRecord(root + off, kExt2, 5, 0, fid, static_cast<uint8_t>(sizeof(fid)));
    (void)off;
    plantDirRecord(pvd + 156, kRootLba, kBs, 2, &dot, 1);
    std::memcpy(img.data() + kExt1 * kBs, a, 5);
    std::memcpy(img.data() + kExt2 * kBs, b, 5);
    return img;
}

// First extent 100 B (not a 512-sector multiple) + second >1 MiB. Forces
// per-run byteCount: sector runs would swallow first-extent padding.
std::vector<uint8_t> buildLargeMultiExtentIso() {
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kLen1 = 100;
    constexpr uint32_t kLen2 = 1024u * 1024u + 16u;
    constexpr uint32_t kExt1 = 21;
    constexpr uint32_t kExt2 = 22;
    constexpr uint32_t kExt2Blocks = (kLen2 + kBs - 1) / kBs;
    constexpr uint32_t kRootLba = 20;
    const uint32_t kBlocks = kExt2 + kExt2Blocks;
    std::vector<uint8_t> img(static_cast<size_t>(kBlocks) * kBs, 0);
    uint8_t* pvd = img.data() + 16 * kBs;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memcpy(pvd + 40, "TESTISO", 7);
    wrBoth32(pvd + 80, kBlocks);
    wrBoth16(pvd + 128, static_cast<uint16_t>(kBs));
    uint8_t* root = img.data() + kRootLba * kBs;
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'B', 'I', 'G', '.', 'B', 'I', 'N', ';', '1'};
    off += plantDirRecord(root + off, kExt1, kLen1, 0x80, fid, static_cast<uint8_t>(sizeof(fid)));
    off += plantDirRecord(root + off, kExt2, kLen2, 0, fid, static_cast<uint8_t>(sizeof(fid)));
    (void)off;
    plantDirRecord(pvd + 156, kRootLba, kBs, 2, &dot, 1);
    std::memset(img.data() + kExt1 * kBs, 'A', kLen1);
    std::memset(img.data() + kExt2 * kBs, 'B', kLen2);
    return img;
}

std::vector<uint8_t> buildJolietIso() {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kJolietRoot = 22;
    constexpr uint32_t kFileLba = 21;
    const char payload[] = "hello iso\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    if (img.size() < (kJolietRoot + 1) * kBs) img.resize((kJolietRoot + 1) * kBs, 0);
    uint8_t* svd = img.data() + 17 * kBs;
    svd[0] = 2;
    std::memcpy(svd + 1, "CD001", 5);
    svd[6] = 1;
    svd[88] = 0x25;
    svd[89] = 0x2F;
    svd[90] = 0x45;
    wrBoth16(svd + 128, static_cast<uint16_t>(kBs));
    uint8_t* jroot = img.data() + kJolietRoot * kBs;
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(jroot + off, kJolietRoot, kBs, 2, &dot, 1);
    off += plantDirRecord(jroot + off, kJolietRoot, kBs, 2, &dotdot, 1);
    const uint8_t jid[] = {0x00, 'R', 0x00, 'e', 0x00, 'a', 0x00, 'd', 0x00, 'M',
                           0x00, 'e', 0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't'};
    off += plantDirRecord(jroot + off, kFileLba, payloadLen, 0, jid, static_cast<uint8_t>(sizeof(jid)));
    (void)off;
    plantDirRecord(svd + 156, kJolietRoot, kBs, 2, &dot, 1);
    wrBoth32(img.data() + 80 + 16 * kBs, static_cast<uint32_t>(img.size() / kBs));
    return img;
}

std::vector<uint8_t> buildElToritoIso() {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kCatalog = 18;
    constexpr uint32_t kBoot = 22;
    const char payload[] = "el torito\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    uint8_t* br = img.data() + 17 * kBs;
    br[0] = 0;
    std::memcpy(br + 1, "CD001", 5);
    br[6] = 1;
    std::memcpy(br + 7, "EL TORITO SPECIFICATION", 23);
    br[71] = static_cast<uint8_t>(kCatalog);
    br[72] = 0;
    br[73] = 0;
    br[74] = 0;
    uint8_t* cat = img.data() + kCatalog * kBs;
    cat[0] = 0x01;
    cat[0x1E] = 0x55;
    cat[0x1F] = 0xAA;
    uint16_t sum = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 14) continue;
        sum = static_cast<uint16_t>(sum + (cat[2 * i] | (static_cast<uint16_t>(cat[2 * i + 1]) << 8)));
    }
    const uint16_t chk = static_cast<uint16_t>(0u - sum);
    cat[0x1C] = static_cast<uint8_t>(chk);
    cat[0x1D] = static_cast<uint8_t>(chk >> 8);
    cat[32] = 0x88;
    cat[38] = 1;
    cat[39] = 0;
    cat[40] = static_cast<uint8_t>(kBoot);
    cat[41] = 0;
    cat[42] = 0;
    cat[43] = 0;
    std::memcpy(img.data() + kBoot * kBs, payload, payloadLen);
    return img;
}

std::vector<uint8_t> buildElToritoSectionIso() {
    auto img = buildElToritoIso();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kCatalog = 18;
    constexpr uint32_t kEfi = 23;
    if (img.size() < (kEfi + 1) * kBs) img.resize((kEfi + 1) * kBs, 0);
    uint8_t* cat = img.data() + kCatalog * kBs;
    cat[40] = 0;
    cat[41] = 0;
    cat[42] = 0;
    cat[43] = 0;
    cat[64] = 0x91;
    cat[66] = 1;
    cat[67] = 0;
    cat[96] = 0x88;
    cat[102] = 1;
    cat[103] = 0;
    cat[104] = static_cast<uint8_t>(kEfi);
    cat[105] = 0;
    cat[106] = 0;
    cat[107] = 0;
    const char efi[] = "efi boot\n";
    std::memcpy(img.data() + kEfi * kBs, efi, std::strlen(efi));
    return img;
}

std::vector<uint8_t> buildRockRidgeIso() {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kFileLba = 21;
    const char payload[] = "hello iso\n";
    const uint32_t payloadLen = static_cast<uint32_t>(std::strlen(payload));
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    off += plantDirRecord(root + off, kFileLba, payloadLen, 0, fid, static_cast<uint8_t>(sizeof(fid)),
                          "readme.txt");
    (void)off;
    return img;
}

} // namespace

TEST(Iso9660, ProbeFindsCd001AtSector16) {
    auto img = buildMinimalIso9660();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    EXPECT_EQ(probeVolumeAt(reader, 0, 512), VolumeFsKind::Iso9660);
}

TEST(Iso9660, ListsHelloTxt) {
    auto img = buildMinimalIso9660();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool found = false;
    for (const auto& n : names) {
        if (n.find("HELLO") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Iso9660, VolumeIdEmitsDiscovery) {
    auto img = buildMinimalIso9660();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.source == "iso9660_vol_id" && fr.name == "TESTISO") found = true;
    }, &running));
    EXPECT_TRUE(found) << "PVD Volume Identifier at +40 must emit iso9660_vol_id";
}

TEST(Iso9660, QuickScanFindsHelloTxt) {
    auto img = buildMinimalIso9660();
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

TEST(Iso9660, RecoversHelloTxtBytes) {
    auto img = buildMinimalIso9660();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_iso_hello_dir");
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
    EXPECT_EQ(got, std::string("hello iso\n"));
    std::filesystem::remove_all(dir);
}

TEST(Iso9660, RecoversMultiExtentHelloTxtBytes) {
    auto img = buildMultiExtentIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_iso_multiext_dir");
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
    EXPECT_EQ(got, std::string("hello iso\n"));
    std::filesystem::remove_all(dir);
}

TEST(Iso9660, RecoversMultiExtentOver1MibUnaligned) {
    auto img = buildLargeMultiExtentIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("BIG") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const uint64_t want = 100u + 1024u * 1024u + 16u;
    EXPECT_EQ(hit.sizeBytes, want);
    EXPECT_GE(hit.runs.size(), 2u);
    const auto dir = bytebackTestTemp("bb_iso_multiext_1m_dir");
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
    ASSERT_EQ(got.size(), static_cast<size_t>(want));
    EXPECT_EQ(got.substr(0, 100), std::string(100, 'A'));
    EXPECT_EQ(got.substr(100, 16), std::string(16, 'B'));
    EXPECT_EQ(got.back(), 'B');
    std::filesystem::remove_all(dir);
}

TEST(Iso9660, PrefersJolietName) {
    auto img = buildJolietIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool joliet = false;
    bool isoName = false;
    for (const auto& n : names) {
        if (n.find("ReadMe") != std::string::npos) joliet = true;
        if (n.find("HELLO") != std::string::npos) isoName = true;
    }
    EXPECT_TRUE(joliet);
    EXPECT_FALSE(isoName);
}

TEST(Iso9660, RockRidgeSlEmitsSymlinkTarget) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'L', 'I', 'N', 'K', ';', '1'};
    off += plantDirRecord(root + off, 21, 0, 0, fid, static_cast<uint8_t>(sizeof(fid)), "link",
                          "t.txt");
    (void)off;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "link" && fr.source == "iso9660_rr_sl" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "t.txt", 5) == 0) {
            found = true;
        }
    }, &running));
    EXPECT_TRUE(found) << "Rock Ridge SL must emit symlink print path as resident";
}

TEST(Iso9660, RecoversElToritoBootImg) {
    auto img = buildElToritoIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("BOOT") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_iso_eltorito_dir");
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
    ASSERT_GE(got.size(), 10u);
    EXPECT_EQ(got.substr(0, 10), std::string("el torito\n"));
    EXPECT_EQ(got.size(), 512u);
    std::filesystem::remove_all(dir);
}

TEST(Iso9660, RecoversElToritoSectionBootImg) {
    auto img = buildElToritoSectionIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    FileRecord hit;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("BOOT") != std::string::npos) hit = fr;
    }, &running));
    ASSERT_FALSE(hit.name.empty());
    const auto dir = bytebackTestTemp("bb_iso_eltorito_sec_dir");
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
    ASSERT_GE(got.size(), 9u);
    EXPECT_EQ(got.substr(0, 9), std::string("efi boot\n"));
    std::filesystem::remove_all(dir);
}

TEST(Iso9660, PrefersRockRidgeName) {
    auto img = buildRockRidgeIso();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (!fr.name.empty()) names.push_back(fr.name);
    }, &running));
    bool rr = false;
    bool isoName = false;
    for (const auto& n : names) {
        if (n.find("readme") != std::string::npos) rr = true;
        if (n.find("HELLO") != std::string::npos) isoName = true;
    }
    EXPECT_TRUE(rr);
    EXPECT_FALSE(isoName);
}

TEST(Iso9660, RecordingDateSetsModifiedAt) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    uint8_t* root = img.data() + kRootLba * kBs;
    size_t i = 0;
    while (i + 34 < kBs && root[i] != 0) {
        const uint8_t recLen = root[i];
        const uint8_t idLen = root[i + 32];
        if (idLen >= 9 && std::memcmp(root + i + 33, "HELLO.TXT", 9) == 0) {
            root[i + 18] = 126;
            root[i + 19] = 9;
            root[i + 20] = 18;
            root[i + 21] = 12;
            root[i + 22] = 0;
            root[i + 23] = 0;
            root[i + 24] = 0;
            break;
        }
        i += recLen ? recLen : 1;
    }

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    int64_t got = 0;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) got = fr.modifiedAt;
    }, &running));
    EXPECT_EQ(got, 1789732800) << "ISO recording date 2026-09-18 12:00:00 GMT must set modifiedAt";
}

TEST(Iso9660, RockRidgeTfOverridesRecordingDate) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    const uint8_t tf7[] = {120, 1, 2, 0, 0, 0, 0};
    uint8_t* rec = root + off;
    off += plantDirRecord(rec, 21, 10, 0, fid, static_cast<uint8_t>(sizeof(fid)), nullptr, nullptr,
                          tf7);
    rec[18] = 126;
    rec[19] = 9;
    rec[20] = 18;
    rec[21] = 12;
    rec[22] = 0;
    rec[23] = 0;
    rec[24] = 0;
    (void)off;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    int64_t got = 0;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) got = fr.modifiedAt;
    }, &running));
    EXPECT_EQ(got, 1577923200) << "Rock Ridge TF MODIFY 2020-01-02 must override ISO recording date";
}

TEST(Iso9660, RockRidgeCeContinuesName) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kCeLba = 22;
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    uint8_t* rec = root + off;
    size_t recLen = 33u + sizeof(fid);
    if (recLen & 1u) ++recLen;
    recLen += 28;
    rec[0] = static_cast<uint8_t>(recLen);
    wrBoth32(rec + 2, 21);
    wrBoth32(rec + 10, 10);
    rec[32] = static_cast<uint8_t>(sizeof(fid));
    std::memcpy(rec + 33, fid, sizeof(fid));
    size_t sua = 33u + sizeof(fid);
    if (sua & 1u) ++sua;
    rec[sua] = 'C';
    rec[sua + 1] = 'E';
    rec[sua + 2] = 28;
    rec[sua + 3] = 1;
    wrBoth32(rec + sua + 4, kCeLba);
    wrBoth32(rec + sua + 12, 0);
    wrBoth32(rec + sua + 20, 11);
    uint8_t* ce = img.data() + kCeLba * kBs;
    ce[0] = 'N';
    ce[1] = 'M';
    ce[2] = 11;
    ce[3] = 1;
    ce[4] = 0;
    std::memcpy(ce + 5, "readme", 6);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    bool isoName = false;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "readme") found = true;
        if (fr.name.find("HELLO") != std::string::npos) isoName = true;
    }, &running));
    EXPECT_TRUE(found) << "Rock Ridge CE continuation NM must be used as the file name";
    EXPECT_FALSE(isoName);
}

TEST(Iso9660, RockRidgeCeContinuesSymlink) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kCeLba = 22;
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'L', 'I', 'N', 'K', ';', '1'};
    uint8_t* rec = root + off;
    size_t recLen = 33u + sizeof(fid);
    if (recLen & 1u) ++recLen;
    recLen += 28;
    rec[0] = static_cast<uint8_t>(recLen);
    wrBoth32(rec + 2, 21);
    wrBoth32(rec + 10, 0);
    rec[32] = static_cast<uint8_t>(sizeof(fid));
    std::memcpy(rec + 33, fid, sizeof(fid));
    size_t sua = 33u + sizeof(fid);
    if (sua & 1u) ++sua;
    rec[sua] = 'C';
    rec[sua + 1] = 'E';
    rec[sua + 2] = 28;
    rec[sua + 3] = 1;
    wrBoth32(rec + sua + 4, kCeLba);
    wrBoth32(rec + sua + 12, 0);
    wrBoth32(rec + sua + 20, 12);
    uint8_t* ce = img.data() + kCeLba * kBs;
    ce[0] = 'S';
    ce[1] = 'L';
    ce[2] = 12;
    ce[3] = 1;
    ce[4] = 0;
    ce[5] = 0;
    ce[6] = 5;
    std::memcpy(ce + 7, "t.txt", 5);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    bool found = false;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("LINK") != std::string::npos && fr.source == "iso9660_rr_sl" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "t.txt", 5) == 0) {
            found = true;
        }
    }, &running));
    EXPECT_TRUE(found) << "Rock Ridge CE continuation SL must emit symlink print path";
}

TEST(Iso9660, RockRidgeCeContinuesTf) {
    auto img = buildMinimalIso9660();
    constexpr uint32_t kBs = 2048;
    constexpr uint32_t kRootLba = 20;
    constexpr uint32_t kCeLba = 22;
    uint8_t* root = img.data() + kRootLba * kBs;
    std::memset(root, 0, kBs);
    uint8_t dot = 0, dotdot = 1;
    size_t off = 0;
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dot, 1);
    off += plantDirRecord(root + off, kRootLba, kBs, 2, &dotdot, 1);
    const uint8_t fid[] = {'H', 'E', 'L', 'L', 'O', '.', 'T', 'X', 'T', ';', '1'};
    uint8_t* rec = root + off;
    size_t recLen = 33u + sizeof(fid);
    if (recLen & 1u) ++recLen;
    recLen += 28;
    rec[0] = static_cast<uint8_t>(recLen);
    wrBoth32(rec + 2, 21);
    wrBoth32(rec + 10, 10);
    rec[18] = 126;
    rec[19] = 9;
    rec[20] = 18;
    rec[21] = 12;
    rec[22] = 0;
    rec[23] = 0;
    rec[24] = 0;
    rec[32] = static_cast<uint8_t>(sizeof(fid));
    std::memcpy(rec + 33, fid, sizeof(fid));
    size_t sua = 33u + sizeof(fid);
    if (sua & 1u) ++sua;
    rec[sua] = 'C';
    rec[sua + 1] = 'E';
    rec[sua + 2] = 28;
    rec[sua + 3] = 1;
    wrBoth32(rec + sua + 4, kCeLba);
    wrBoth32(rec + sua + 12, 0);
    wrBoth32(rec + sua + 20, 12);
    uint8_t* ce = img.data() + kCeLba * kBs;
    ce[0] = 'T';
    ce[1] = 'F';
    ce[2] = 12;
    ce[3] = 1;
    ce[4] = 0x02;
    const uint8_t tf7[] = {120, 1, 2, 0, 0, 0, 0};
    std::memcpy(ce + 5, tf7, 7);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    int64_t got = 0;
    std::atomic<bool> running{true};
    Iso9660Parser iso;
    ASSERT_TRUE(iso.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("HELLO") != std::string::npos) got = fr.modifiedAt;
    }, &running));
    EXPECT_EQ(got, 1577923200) << "Rock Ridge CE TF MODIFY must override ISO recording date";
}
