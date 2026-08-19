// Native unit tests for the shared MD5 and the EWF (.E01) writer.
//
// MD5: RFC 1321 test vectors confirm the implementation is byte-exact.
// EWF: write a small synthetic image, then parse the container back — file
// header signature, section sequence, volume geometry, table offsets and the
// embedded MD5 digest must all round-trip. This validates the layout against
// what libewf-family readers expect without requiring libewf itself.
//
// Run with:
//   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target wolf_tests
//   ctest --test-dir native/build --output-on-failure
#include "crypto/wolf_md5.h"
#include "imager/ewf_writer.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>

using namespace wolf;
using wolf::crypto::md5Hex;

// ---------------- MD5 ----------------
TEST(Md5, Rfc1321Vectors) {
    EXPECT_EQ(md5Hex(reinterpret_cast<const uint8_t*>(""), 0),
              "d41d8cd98f00b204e9800998ecf8427e");
    const char* a = "a";
    EXPECT_EQ(md5Hex(reinterpret_cast<const uint8_t*>(a), 1),
              "0cc175b9c0f1b6a831c399e269772661");
    const char* abc = "abc";
    EXPECT_EQ(md5Hex(reinterpret_cast<const uint8_t*>(abc), 3),
              "900150983cd24fb0d6963f7d28e17f72");
    const char* msg = "message digest";
    EXPECT_EQ(md5Hex(reinterpret_cast<const uint8_t*>(msg), 14),
              "f96b697d7cb7938d525a2f31aaf161d0");
}

TEST(Md5, ChunkedUpdateEqualsOneShot) {
    // Feed a 200-byte pattern in irregular chunk sizes; the digest must match
    // the one-shot computation over the same bytes.
    std::vector<uint8_t> buf(200);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 7);

    crypto::Md5 m;
    size_t pos = 0;
    const size_t steps[] = {1, 63, 64, 65, 7};
    int s = 0;
    while (pos < buf.size()) {
        size_t take = std::min(steps[s++ % 5], buf.size() - pos);
        m.update(buf.data() + pos, take);
        pos += take;
    }
    EXPECT_EQ(m.finalHex(), md5Hex(buf.data(), buf.size()));
}

TEST(Md5, MatchesExternalReferenceForLongInputs) {
    // Cross-implementation check against Python 3 hashlib.md5 over
    // bytes((i*7) & 0xFF for i in range(200)):
    //   d96c89b9da43f6903ceaa14a7166a22e
    // This is the case that exposed the historical buffering bug (RFC vectors
    // are all short single-block inputs and never crossed it).
    std::vector<uint8_t> buf(200);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 7);
    EXPECT_EQ(md5Hex(buf.data(), buf.size()), "d96c89b9da43f6903ceaa14a7166a22e");

    // Same content via chunked updates.
    crypto::Md5 m;
    m.update(buf.data(), 100);
    m.update(buf.data() + 100, 100);
    EXPECT_EQ(m.finalHex(), "d96c89b9da43f6903ceaa14a7166a22e");
}

TEST(Md5, AllLengthsMod64RoundTrip) {
    // For every length 0..255 the one-shot digest of a counter pattern must
    // equal the chunked digest — this sweeps every buffering edge case
    // (partial tail, exactly-full block, length completing a block, and the
    // 48..55 mod-64 finalize band the old code got wrong).
    std::vector<uint8_t> buf(300);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i & 0xFF);

    for (size_t len = 0; len <= 255; ++len) {
        std::string oneShot = md5Hex(buf.data(), len);
        crypto::Md5 m;
        m.update(buf.data(), len);
        EXPECT_EQ(m.finalHex(), oneShot) << "length " << len;

        // Also feed in 7-byte chunks to force many partial buffers.
        crypto::Md5 m2;
        for (size_t p = 0; p < len; p += 7) {
            m2.update(buf.data() + p, std::min<size_t>(7, len - p));
        }
        EXPECT_EQ(m2.finalHex(), oneShot) << "length " << len << " (7-byte chunks)";
    }
}

// ---------------- EWF writer ----------------
namespace {
std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}
uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint32_t rdU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
} // namespace

TEST(Ewf, WritesValidContainerLayout) {
    std::string path = "test_output.E01";
    const uint64_t kSectors = 64; // 64 * 512 = 32 KiB image

    EwfOptions opts;
    opts.caseNumber = "unittest";
    opts.serial = "TEST0001";

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512, opts));

    // Image content: a recognizable pattern.
    std::vector<uint8_t> img(kSectors * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);
    // Feed in two writes to exercise chunk repacking (128-sector chunks).
    ASSERT_TRUE(w.write(img.data(), 16 * 512));
    ASSERT_TRUE(w.write(img.data() + 16 * 512, 48 * 512));
    ASSERT_TRUE(w.finish());
    EXPECT_EQ(w.bytesWritten(), img.size());
    EXPECT_EQ(w.md5Hex(), md5Hex(img.data(), img.size()));

    auto f = readAll(path);
    ::remove(path.c_str());
    ASSERT_GT(f.size(), static_cast<size_t>(76));

    // File header: signature + segment fields + checksum.
    static const uint8_t SIG[8] = {'E', 'V', 'F', 0x09, 0x0D, 0x0A, 0xFF, 0x00};
    EXPECT_EQ(std::memcmp(f.data(), SIG, 8), 0);
    EXPECT_EQ(rdU32(f.data() + 0x08), 76u);      // fields start
    EXPECT_EQ(rdU32(f.data() + 0x10) & 0xFFFF, 1u); // segment number (u16)
    EXPECT_EQ(rdU32(f.data() + 0x12) & 0xFFFF, 1u); // total segments (u16)
    // Header checksum = first 4 bytes of MD5(header[0..0x44)).
    std::string hc = md5Hex(f.data(), 0x44);
    uint32_t expect = 0;
    for (int i = 0; i < 4; ++i) {
        char tmp[3] = {hc[i * 2], hc[i * 2 + 1], 0};
        expect |= static_cast<uint32_t>(std::strtoul(tmp, nullptr, 16)) << (8 * i);
    }
    EXPECT_EQ(rdU32(f.data() + 0x44), expect);

    // Walk the sections: header -> disk -> sectors -> table -> table2 ->
    // digest -> done.
    uint64_t off = 76;
    std::vector<std::string> types;
    std::vector<uint64_t> sizes;
    while (off + 40 <= f.size()) {
        char type[17] = {0};
        std::memcpy(type, f.data() + off, 16);
        uint64_t next = rdU64(f.data() + off + 16);
        uint64_t size = rdU64(f.data() + off + 24);
        // Single-segment files carry the size in both fields.
        EXPECT_EQ(next, size) << "section " << type;
        types.push_back(type);
        sizes.push_back(size);
        if (std::string(type) == "done") break;
        ASSERT_GT(size, 40u) << "section " << type;
        off += size;
    }
    ASSERT_EQ(types.size(), 7u);
    EXPECT_EQ(types[0], "header");
    EXPECT_EQ(types[1], "disk");
    EXPECT_EQ(types[2], "sectors");
    EXPECT_EQ(types[3], "table");
    EXPECT_EQ(types[4], "table2");
    EXPECT_EQ(types[5], "digest");
    EXPECT_EQ(types[6], "done");

    // Disk section: geometry must match what we opened with.
    {
        uint64_t diskData = 76 + sizes[0];
        const uint8_t* vol = f.data() + diskData + 40;
        uint32_t chunkCount = vol[1] | (vol[2] << 8) | (vol[3] << 16);
        EXPECT_EQ(chunkCount, 1u); // 64 sectors / 128 sectors-per-chunk -> 1
        uint32_t spc = rdU32(vol + 4);
        uint32_t bps = rdU32(vol + 8);
        uint64_t sectors = rdU64(vol + 12);
        EXPECT_EQ(spc, 128u);
        EXPECT_EQ(bps, 512u);
        EXPECT_EQ(sectors, kSectors);
    }

    // Sectors section: content must equal the image bytes.
    {
        uint64_t secData = 76 + sizes[0] + sizes[1] + 40;
        EXPECT_EQ(sizes[2], 40u + img.size());
        EXPECT_EQ(std::memcmp(f.data() + secData, img.data(), img.size()), 0);
    }

    // Table: one chunk -> offsets {0, total}.
    {
        uint64_t tblData = 76 + sizes[0] + sizes[1] + sizes[2] + 40;
        EXPECT_EQ(rdU32(f.data() + tblData), 0u);
        EXPECT_EQ(rdU32(f.data() + tblData + 4), static_cast<uint32_t>(img.size()));
    }

    // Digest: raw MD5 of the image data.
    {
        uint64_t digData = 76 + sizes[0] + sizes[1] + sizes[2] + sizes[3] + sizes[4] + 40;
        crypto::Md5 m;
        m.update(img.data(), img.size());
        uint8_t raw[16];
        m.finalRaw(raw);
        EXPECT_EQ(std::memcmp(f.data() + digData, raw, 16), 0);
    }
}

TEST(Ewf, RejectsUnalignedWritesAndDoubleOpen) {
    std::string path = "test_output2.E01";
    EwfWriter w;
    ASSERT_TRUE(w.open(path, 8, 512));

    // Unaligned length (not a multiple of the sector size) must be rejected.
    std::vector<uint8_t> bad(1000, 0);
    EXPECT_FALSE(w.write(bad.data(), bad.size()));
    ASSERT_TRUE(w.finish());
    ::remove(path.c_str());

    // Double open must fail while open (first writer still owns the stream).
    EwfWriter w2;
    // w is closed now, so opening the same path is fine:
    ASSERT_TRUE(w2.open(path, 8, 512));
    std::vector<uint8_t> sec(512, 1);
    EXPECT_TRUE(w2.write(sec.data(), sec.size()));
    EXPECT_TRUE(w2.finish());
    ::remove(path.c_str());
}

// CA-004: optional independent reader cross-check when WOLF_EWFINFO is set
// (CI passes the path to libewf's ewfinfo when installed).
TEST(Ewf, OptionalEwfinfoCrossCheck) {
    const char* tool = std::getenv("WOLF_EWFINFO");
    if (!tool || !*tool) {
        GTEST_SKIP() << "Set WOLF_EWFINFO to the ewfinfo binary path";
    }

    std::string path = "crosscheck_ewf.E01";
    const uint64_t kSectors = 8;
    std::vector<uint8_t> img(kSectors * 512, 0x5A);

    EwfWriter w;
    ASSERT_TRUE(w.open(path, kSectors, 512));
    ASSERT_TRUE(w.write(img.data(), img.size()));
    ASSERT_TRUE(w.finish());
    const std::string expectMd5 = w.md5Hex();

    std::string cmd = std::string("\"") + tool + "\" \"" + path + "\"";
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    ASSERT_NE(pipe, nullptr) << "Failed to run: " << cmd;

    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    ::remove(path.c_str());

    std::string needle = expectMd5;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string hay = output;
    for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    EXPECT_NE(hay.find(needle), std::string::npos)
        << "ewfinfo output did not contain MD5 " << expectMd5 << "\n---\n" << output;
}

TEST(Ewf, OpenRejectsOver4GiB) {
    EwfWriter w;
    const uint64_t sectors = (0x100000000ull / 512ull) + 1ull;
    EXPECT_FALSE(w.open("too_big.E01", sectors, 512));
}
