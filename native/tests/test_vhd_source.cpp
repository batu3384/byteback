#include "io/vhd_source.h"
#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/refs_integrity.h"
#include "recovery/path_util.h"
#include "test_temp_path.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace byteback;

namespace {

void wrLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void wrLe64(uint8_t* p, uint64_t v) {
    wrLe32(p, static_cast<uint32_t>(v));
    wrLe32(p + 4, static_cast<uint32_t>(v >> 32));
}
void stampVhdxHeaderCrc(uint8_t* h) {
    std::memset(h + 4, 0, 4);
    wrLe32(h + 4, refsCrc32c(h, 4096));
}

} // namespace

TEST(VhdSource, FixedVhdFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vhd = wrapFixedVhd(fat);
    ASSERT_GT(vhd.size(), fat.size());

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhd), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "fixed VHD mount must expose FAT16 TEST.TXT";
}

TEST(VhdSource, MissingCookieDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(fat, &err));
}

TEST(VhdSource, DynamicVhdFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vhd = wrapDynamicVhd(fat);
    ASSERT_LT(vhd.size(), fat.size()) << "dynamic VHD must be sparse vs virtual size";

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhd), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "dynamic VHD BAT mount must expose FAT16 TEST.TXT";
}

TEST(VhdSource, DifferencingVhdDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto vhd = wrapFixedVhd(fat);
    ASSERT_GE(vhd.size(), 512u);
    uint8_t* f = vhd.data() + (vhd.size() - 512);
    f[0x3C] = 0;
    f[0x3D] = 0;
    f[0x3E] = 0;
    f[0x3F] = 4; // BE disk type 4 = differencing
    f[0x40] = f[0x41] = f[0x42] = f[0x43] = 0;
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        if (i >= 0x40 && i < 0x44) continue;
        sum += f[i];
    }
    const uint32_t chk = ~sum;
    f[0x40] = static_cast<uint8_t>(chk >> 24);
    f[0x41] = static_cast<uint8_t>(chk >> 16);
    f[0x42] = static_cast<uint8_t>(chk >> 8);
    f[0x43] = static_cast<uint8_t>(chk);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(vhd), &err));
}

TEST(VhdSource, DifferencingChildReadsParentFat16) {
    auto fat = testfix::buildFat16Volume();
    auto parent = wrapDynamicVhd(fat);
    std::vector<uint8_t> empty(fat.size(), 0);
    auto child = wrapDynamicVhd(empty, "vhd_diff_parent.vhd");
    const auto dir = bytebackTestTemp("byteback_vhd_diff");
    std::filesystem::create_directories(dir);
    const auto parentPath = (dir / "vhd_diff_parent.vhd").string();
    const auto childPath = (dir / "vhd_diff_child.vhd").string();
    {
        std::ofstream out(parentPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(parent.data()),
                              static_cast<std::streamsize>(parent.size())));
    }
    {
        std::ofstream out(childPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(child.data()),
                              static_cast<std::streamsize>(child.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(childPath, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "differencing VHD must read FAT16 TEST.TXT from parent";
    std::filesystem::remove(childPath);
    std::filesystem::remove(parentPath);
    std::filesystem::remove_all(dir);
}

TEST(VhdxSource, Fat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat);
    ASSERT_GE(vhdx.size(), 8u);
    EXPECT_EQ(std::memcmp(vhdx.data(), "vhdxfile", 8), 0);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhdx), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "VHDX mount must expose FAT16 TEST.TXT";
}

TEST(VhdxSource, ParentVhdxDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat, true);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(vhdx), &err));
}

TEST(VhdxSource, EmptyLogGuidStillMountsFat16) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat);
    const size_t hdrOffs[2] = {64u * 1024u, 128u * 1024u};
    for (size_t off : hdrOffs) {
        ASSERT_GT(vhdx.size(), off + 4096u);
        uint8_t* h = vhdx.data() + off;
        ASSERT_EQ(std::memcmp(h, "head", 4), 0);
        std::memset(h + 48, 0xAB, 16);
        h[68] = h[69] = h[70] = h[71] = 0;
        std::memset(h + 4, 0, 4);
        const uint32_t crc = refsCrc32c(h, 4096);
        h[4] = static_cast<uint8_t>(crc);
        h[5] = static_cast<uint8_t>(crc >> 8);
        h[6] = static_cast<uint8_t>(crc >> 16);
        h[7] = static_cast<uint8_t>(crc >> 24);
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhdx), &err)) << err;
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "flushed VHDX (LogGuid set, LogLength 0) must still mount";
}

TEST(VhdxSource, ReplaysLogRestoresFat16) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat);
    constexpr size_t k1MiB = 1024u * 1024u;
    constexpr size_t k4k = 4096;
    const size_t dataOff = 3 * k1MiB;
    ASSERT_GT(vhdx.size(), dataOff + k4k);
    uint8_t sector[4096];
    std::memcpy(sector, vhdx.data() + dataOff, k4k);
    std::memset(vhdx.data() + dataOff, 0, k4k);

    const uint64_t logOff = vhdx.size();
    ASSERT_EQ(logOff % k1MiB, 0u);
    vhdx.resize(vhdx.size() + k1MiB, 0);
    const uint64_t seq = 1;
    uint8_t guid[16];
    std::memset(guid, 0xAB, 16);
    uint8_t* e = vhdx.data() + static_cast<size_t>(logOff);
    std::memcpy(e, "loge", 4);
    wrLe32(e + 8, 8192);
    wrLe32(e + 12, 0);
    wrLe64(e + 16, seq);
    wrLe32(e + 24, 1);
    std::memcpy(e + 32, guid, 16);
    wrLe64(e + 48, vhdx.size());
    wrLe64(e + 56, vhdx.size());
    uint8_t* d = e + 64;
    std::memcpy(d, "desc", 4);
    std::memcpy(d + 4, sector + 4092, 4);
    std::memcpy(d + 8, sector, 8);
    wrLe64(d + 16, dataOff);
    wrLe64(d + 24, seq);
    uint8_t* ds = e + k4k;
    std::memcpy(ds, "data", 4);
    wrLe32(ds + 4, static_cast<uint32_t>(seq >> 32));
    std::memcpy(ds + 8, sector + 8, 4084);
    wrLe32(ds + 4092, static_cast<uint32_t>(seq));
    std::memset(e + 4, 0, 4);
    wrLe32(e + 4, refsCrc32c(e, 8192));

    const size_t hdrOffs[2] = {64u * 1024u, 128u * 1024u};
    for (size_t off : hdrOffs) {
        uint8_t* h = vhdx.data() + off;
        ASSERT_EQ(std::memcmp(h, "head", 4), 0);
        std::memcpy(h + 48, guid, 16);
        wrLe32(h + 68, static_cast<uint32_t>(k1MiB));
        wrLe64(h + 72, logOff);
        stampVhdxHeaderCrc(h);
    }

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhdx), &err)) << err;
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "VHDX log replay must restore FAT16 TEST.TXT";
}

TEST(VhdxSource, TruncatedLogDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat);
    const size_t hdrOffs[2] = {64u * 1024u, 128u * 1024u};
    const uint64_t logOff = vhdx.size();
    ASSERT_EQ(logOff % (1024u * 1024u), 0u);
    for (size_t off : hdrOffs) {
        ASSERT_GT(vhdx.size(), off + 4096u);
        uint8_t* h = vhdx.data() + off;
        ASSERT_EQ(std::memcmp(h, "head", 4), 0);
        std::memset(h + 48, 0xAB, 16);
        wrLe32(h + 68, 1024u * 1024u);
        wrLe64(h + 72, logOff);
        stampVhdxHeaderCrc(h);
    }
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(vhdx), &err));
}

TEST(VhdxSource, ReplaysLogZeroClearsSector) {
    auto fat = testfix::buildFat16Volume();
    auto vhdx = wrapVhdx(fat);
    constexpr size_t k1MiB = 1024u * 1024u;
    constexpr size_t k4k = 4096;
    const size_t dataOff = 3 * k1MiB;
    const size_t virtOff = 256u * 1024u;
    ASSERT_GT(fat.size(), virtOff + k4k);
    for (size_t i = 0; i < k4k; ++i) {
        ASSERT_EQ(fat[virtOff + i], 0) << "fixture slack must be zero so replay zero is observable";
    }
    ASSERT_GT(vhdx.size(), dataOff + virtOff + k4k);
    std::memset(vhdx.data() + dataOff + virtOff, 0xCC, k4k);

    const uint64_t logOff = vhdx.size();
    ASSERT_EQ(logOff % k1MiB, 0u);
    vhdx.resize(vhdx.size() + k1MiB, 0);
    const uint64_t seq = 1;
    uint8_t guid[16];
    std::memset(guid, 0xAB, 16);
    uint8_t* e = vhdx.data() + static_cast<size_t>(logOff);
    std::memcpy(e, "loge", 4);
    wrLe32(e + 8, 4096);
    wrLe64(e + 16, seq);
    wrLe32(e + 24, 1);
    std::memcpy(e + 32, guid, 16);
    wrLe64(e + 48, vhdx.size());
    wrLe64(e + 56, vhdx.size());
    uint8_t* d = e + 64;
    std::memcpy(d, "zero", 4);
    wrLe64(d + 8, k4k);
    wrLe64(d + 16, dataOff + virtOff);
    wrLe64(d + 24, seq);
    std::memset(e + 4, 0, 4);
    wrLe32(e + 4, refsCrc32c(e, 4096));

    const size_t hdrOffs[2] = {64u * 1024u, 128u * 1024u};
    for (size_t off : hdrOffs) {
        uint8_t* h = vhdx.data() + off;
        ASSERT_EQ(std::memcmp(h, "head", 4), 0);
        std::memcpy(h + 48, guid, 16);
        wrLe32(h + 68, static_cast<uint32_t>(k1MiB));
        wrLe64(h + 72, logOff);
        stampVhdxHeaderCrc(h);
    }

    std::vector<uint8_t> payload;
    std::string err;
    ASSERT_TRUE(extractVhdPayload(vhdx.data(), vhdx.size(), payload, err)) << err;
    ASSERT_EQ(payload.size(), fat.size());
    for (size_t i = 0; i < k4k; ++i) {
        EXPECT_EQ(payload[virtOff + i], 0) << i;
    }

    DiskReader reader;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vhdx), &err)) << err;
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "VHDX log zero descriptor must not smash FAT16 TEST.TXT";
}

TEST(VhdxSource, DifferencingChildReadsParentFat16) {
    auto fat = testfix::buildFat16Volume();
    auto parent = wrapVhdx(fat);
    std::vector<uint8_t> empty(fat.size(), 0);
    auto child = wrapVhdx(empty, true, "vhdx_diff_parent.vhdx");
    const auto dir = bytebackTestTemp("byteback_vhdx_diff");
    std::filesystem::create_directories(dir);
    const auto parentPath = (dir / "vhdx_diff_parent.vhdx").string();
    const auto childPath = (dir / "vhdx_diff_child.vhdx").string();
    {
        std::ofstream out(parentPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(parent.data()),
                              static_cast<std::streamsize>(parent.size())));
    }
    {
        std::ofstream out(childPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(child.data()),
                              static_cast<std::streamsize>(child.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(childPath, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "differencing VHDX must read FAT16 TEST.TXT from parent";
    std::filesystem::remove(childPath);
    std::filesystem::remove(parentPath);
    std::filesystem::remove_all(dir);
}

TEST(VmdkSource, Fat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vmdk = wrapVmdk(fat);
    ASSERT_GE(vmdk.size(), 4u);
    EXPECT_EQ(vmdk[0], 'K');
    EXPECT_EQ(vmdk[1], 'D');
    EXPECT_EQ(vmdk[2], 'M');
    EXPECT_EQ(vmdk[3], 'V');

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vmdk), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "VMDK sparse mount must expose FAT16 TEST.TXT";
}

TEST(VmdkSource, ParentVmdkDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto vmdk = wrapVmdk(fat, true);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(vmdk), &err));
}

TEST(VmdkSource, DifferencingChildReadsParentFat16) {
    auto fat = testfix::buildFat16Volume();
    auto parent = wrapVmdk(fat);
    std::vector<uint8_t> empty(fat.size(), 0);
    auto child = wrapVmdk(empty, true, false, "vmdk_diff_parent.vmdk");
    const auto dir = bytebackTestTemp("byteback_vmdk_diff");
    std::filesystem::create_directories(dir);
    const auto parentPath = (dir / "vmdk_diff_parent.vmdk").string();
    const auto childPath = (dir / "vmdk_diff_child.vmdk").string();
    {
        std::ofstream out(parentPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(parent.data()),
                              static_cast<std::streamsize>(parent.size())));
    }
    {
        std::ofstream out(childPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(child.data()),
                              static_cast<std::streamsize>(child.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(childPath, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "differencing VMDK must read FAT16 TEST.TXT from parent";
    std::filesystem::remove(childPath);
    std::filesystem::remove(parentPath);
    std::filesystem::remove_all(dir);
}

TEST(VmdkSource, CompressedVmdkFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vmdk = wrapVmdk(fat, false, true);
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vmdk), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "zlib-compressed VMDK grain must expose FAT16 TEST.TXT";
}

TEST(VdiSource, Fat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vdi = wrapVdi(fat);
    ASSERT_GE(vdi.size(), 68u);
    EXPECT_EQ(vdi[64], 0x7F);
    EXPECT_EQ(vdi[65], 0x10);
    EXPECT_EQ(vdi[66], 0xDA);
    EXPECT_EQ(vdi[67], 0xBE);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(vdi), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "VDI mount must expose FAT16 TEST.TXT";
}

TEST(VdiSource, DifferencingVdiDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto vdi = wrapVdi(fat, true);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(vdi), &err));
}

TEST(VdiSource, DifferencingChildReadsParentFat16) {
    auto fat = testfix::buildFat16Volume();
    auto parent = wrapVdi(fat);
    std::vector<uint8_t> empty(fat.size(), 0);
    auto child = wrapVdi(empty, true);
    const auto dir = bytebackTestTemp("byteback_vdi_diff");
    std::filesystem::create_directories(dir);
    const auto parentPath = (dir / "vdi_diff_parent.vdi").string();
    const auto childPath = (dir / "vdi_diff_child.vdi").string();
    {
        std::ofstream out(parentPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(parent.data()),
                              static_cast<std::streamsize>(parent.size())));
    }
    {
        std::ofstream out(childPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(child.data()),
                              static_cast<std::streamsize>(child.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(childPath, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "differencing VDI must read FAT16 TEST.TXT from parent";
    std::filesystem::remove(childPath);
    std::filesystem::remove(parentPath);
    std::filesystem::remove_all(dir);
}

TEST(VhdSource, AttachFileAndCloneListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vhd = wrapFixedVhd(fat);
    const auto path = bytebackTestTemp("byteback_attach", ".vhd").string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(vhd.data()),
                              static_cast<std::streamsize>(vhd.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(path, &err)) << err;
    auto clone = reader.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(*clone, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "attachVhdFile clone must expose FAT16 TEST.TXT";
    std::filesystem::remove(path);
}

TEST(VhdSource, AttachFileUtf8PathListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto vhd = wrapFixedVhd(fat);
    auto dir = std::filesystem::temp_directory_path() /
               std::filesystem::u8path(u8"byteback_vhd_\u00fcye");
    dir += "_";
    dir += std::to_string(bytebackTestPid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto p = dir / std::filesystem::u8path(u8"disk.vhd");
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(vhd.data()),
                              static_cast<std::streamsize>(vhd.size())));
    }
    {
        DiskReader reader;
        std::string err;
        ASSERT_TRUE(reader.attachVhdFile(pathToUtf8(p), &err)) << err;
        EXPECT_EQ(reader.getDiskSize(), fat.size());
        bool found = false;
        std::atomic<bool> running{true};
        FATParser parser;
        ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
            if (fr.name.find("TEST") != std::string::npos) found = true;
        }, &running));
        EXPECT_TRUE(found) << "UTF-8 VHD path must expose FAT16 TEST.TXT";
    }
    std::filesystem::remove_all(dir);
}

TEST(Qcow2Source, Fat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto qcow = wrapQcow2(fat);
    ASSERT_GE(qcow.size(), 4u);
    EXPECT_EQ(qcow[0], 'Q');
    EXPECT_EQ(qcow[1], 'F');
    EXPECT_EQ(qcow[2], 'I');
    EXPECT_EQ(qcow[3], 0xFB);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(qcow), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "QCOW2 mount must expose FAT16 TEST.TXT";
}

TEST(Qcow2Source, BackingFileDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto qcow = wrapQcow2(fat, true);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(qcow), &err));
}

TEST(Qcow2Source, BackingChildReadsParentFat16) {
    auto fat = testfix::buildFat16Volume();
    auto parent = wrapQcow2(fat);
    std::vector<uint8_t> empty(fat.size(), 0);
    auto child = wrapQcow2(empty, true, false, "qcow2_diff_parent.qcow2");
    const auto dir = bytebackTestTemp("byteback_qcow2_diff");
    std::filesystem::create_directories(dir);
    const auto parentPath = (dir / "qcow2_diff_parent.qcow2").string();
    const auto childPath = (dir / "qcow2_diff_child.qcow2").string();
    {
        std::ofstream out(parentPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(parent.data()),
                              static_cast<std::streamsize>(parent.size())));
    }
    {
        std::ofstream out(childPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(child.data()),
                              static_cast<std::streamsize>(child.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdFile(childPath, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "QCOW2 backing must read FAT16 TEST.TXT from parent";
    std::filesystem::remove(childPath);
    std::filesystem::remove(parentPath);
    std::filesystem::remove_all(dir);
}

TEST(Qcow2Source, CompressedClusterDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto qcow = wrapQcow2(fat);
    constexpr uint64_t l2Off = 2ull << 16;
    ASSERT_GT(qcow.size(), l2Off + 8);
    qcow[static_cast<size_t>(l2Off) + 0] |= 0x40; // BE bit 62 of first L2 entry
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(qcow), &err));
}

TEST(Qcow2Source, CompressedClusterListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto qcow = wrapQcow2(fat, false, true);
    ASSERT_FALSE(qcow.empty());
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(qcow), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "zlib-compressed QCOW2 cluster must expose FAT16 TEST.TXT";
}

TEST(Qcow2Source, AttachEvidenceImageListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto qcow = wrapQcow2(fat);
    const auto path = bytebackTestTemp("byteback_attach", ".qcow2").string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(qcow.data()),
                              static_cast<std::streamsize>(qcow.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEvidenceImage(path, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "attachEvidenceImage .qcow2 must expose FAT16 TEST.TXT";
    reader.detachImageBackend();
    std::filesystem::remove(path);
}

TEST(DmgSource, UdrwFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat);
    ASSERT_GE(dmg.size(), 512u);
    EXPECT_EQ(std::memcmp(dmg.data() + dmg.size() - 512, "koly", 4), 0);

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(dmg), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());

    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "UDIF UDRW mount must expose FAT16 TEST.TXT";
}

TEST(DmgSource, UdzoFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat, 0x80000005u);
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(dmg), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "UDIF UDZO mount must expose FAT16 TEST.TXT";
}

TEST(DmgSource, AdcFat16ListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat, 0x80000004u);
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachVhdMemory(std::move(dmg), &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "UDIF ADC mount must expose FAT16 TEST.TXT";
}

TEST(DmgSource, EncryptedDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat, 1, true);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(dmg), &err));
}

TEST(DmgSource, LzfseDoesNotMount) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat, 0x80000007u);
    DiskReader reader;
    std::string err;
    EXPECT_FALSE(reader.attachVhdMemory(std::move(dmg), &err));
}

TEST(DmgSource, AttachEvidenceImageListsTestTxt) {
    auto fat = testfix::buildFat16Volume();
    auto dmg = wrapDmg(fat);
    const auto path = bytebackTestTemp("byteback_attach", ".dmg").string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(dmg.data()),
                              static_cast<std::streamsize>(dmg.size())));
    }
    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachEvidenceImage(path, &err)) << err;
    EXPECT_EQ(reader.getDiskSize(), fat.size());
    bool found = false;
    std::atomic<bool> running{true};
    FATParser parser;
    ASSERT_TRUE(parser.scan(reader, [&](const FileRecord& fr) {
        if (fr.name.find("TEST") != std::string::npos) found = true;
    }, &running));
    EXPECT_TRUE(found) << "attachEvidenceImage .dmg must expose FAT16 TEST.TXT";
    reader.detachImageBackend();
    std::filesystem::remove(path);
}
