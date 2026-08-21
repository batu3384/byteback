// Native unit tests for the NTFS parsing helpers in fs/ntfs_util.h.
//
// Covers the three byte-level conversions that every NTFS code path depends on:
//   - FILETIME -> Unix epoch (timestamps)
//   - UTF-16LE -> UTF-8 (filenames, incl. Turkish + surrogate pairs)
//   - Update Sequence Array fixup (record integrity)
//
// Run with:
//   cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target byteback_tests
//   ctest --test-dir native/build --output-on-failure
#include "fs/ntfs_util.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

using byteback::ntfs::applyUsaFixup;
using byteback::ntfs::filetimeToUnix;
using byteback::ntfs::utf16leToUtf8;
using byteback::ntfs::FILETIME_UNIX_EPOCH_OFFSET_100NS;
using byteback::ntfs::FILETIME_TICKS_PER_SECOND;

// ---------------------------------------------------------------------------
// FILETIME conversion
// ---------------------------------------------------------------------------
TEST(Filetime, ZeroIsUnknown) {
    EXPECT_EQ(filetimeToUnix(0), 0);
}

TEST(Filetime, PreEpochIsUnknown) {
    // Anything before 1601-01-01 (let alone before 1970) is invalid.
    EXPECT_EQ(filetimeToUnix(1), 0);
    EXPECT_EQ(filetimeToUnix(FILETIME_UNIX_EPOCH_OFFSET_100NS - 1), 0);
}

TEST(Filetime, UnixEpochIsZero) {
    // The exact FILETIME that corresponds to 1970-01-01 00:00:00 UTC.
    EXPECT_EQ(filetimeToUnix(FILETIME_UNIX_EPOCH_OFFSET_100NS), 0);
}

TEST(Filetime, KnownTimestamp) {
    // 2000-01-01 00:00:00 UTC == 946684800 Unix seconds.
    // FILETIME = 125911584000000000 (verified externally).
    constexpr uint64_t Y2K_FILETIME = 125911584000000000ULL;
    EXPECT_EQ(filetimeToUnix(Y2K_FILETIME), 946684800);
}

TEST(Filetime, DropsSubSecondPrecision) {
    // Two FILETIMEs in the same second differ only in the low bits; both
    // collapse to the same Unix second.
    constexpr uint64_t base = FILETIME_UNIX_EPOCH_OFFSET_100NS + 50 * FILETIME_TICKS_PER_SECOND;
    EXPECT_EQ(filetimeToUnix(base), filetimeToUnix(base + 12345));
}

// ---------------------------------------------------------------------------
// UTF-16LE -> UTF-8
// ---------------------------------------------------------------------------
TEST(Utf16, EmptyInputIsEmpty) {
    EXPECT_TRUE(utf16leToUtf8(nullptr, 0).empty());
    const uint16_t nothing[] = {0};
    EXPECT_TRUE(utf16leToUtf8(nothing, 0).empty());
}

TEST(Utf16, AsciiPassesThrough) {
    const uint16_t hello[] = {'H', 'e', 'l', 'l', 'o', 0};
    EXPECT_EQ(utf16leToUtf8(hello, 5), "Hello");
}

TEST(Utf16, TurkishCharactersSurvive) {
    // "Güneş" — the previous ASCII-only decoder turned this into "G_ne_".
    // U+00FC (ü), U+015F (ş). Verify UTF-8 round-trips correctly.
    const uint16_t gunes[] = {'G', 0x00FC, 'n', 'e', 0x015F, 0};
    std::string out = utf16leToUtf8(gunes, 5);
    // UTF-8 bytes: G=47, ü=C3 BC, n=6E, e=65, ş=C5 9F
    const unsigned char expected[] = {0x47, 0xC3, 0xBC, 0x6E, 0x65, 0xC5, 0x9F};
    ASSERT_EQ(out.size(), sizeof(expected));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(out[i]), expected[i]);
    }
}

TEST(Utf16, CyrillicSurvives) {
    // "ФСБ" (3 Cyrillic capitals). Each is 2 UTF-8 bytes.
    const uint16_t fsb[] = {0x0424, 0x0421, 0x0411, 0};
    std::string out = utf16leToUtf8(fsb, 3);
    EXPECT_EQ(out.size(), 6u);
}

TEST(Utf16, SurrogatePairDecodes) {
    // U+1F600 (grinning face emoji): high surrogate 0xD83D, low 0xDE00.
    // Requires 4 UTF-8 bytes (F0 9F 98 80). This exercises the surrogate path.
    const uint16_t emoji[] = {0xD83D, 0xDE00, 0};
    std::string out = utf16leToUtf8(emoji, 2);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(out[0]), 0xF0);
    EXPECT_EQ(static_cast<unsigned char>(out[1]), 0x9F);
    EXPECT_EQ(static_cast<unsigned char>(out[2]), 0x98);
    EXPECT_EQ(static_cast<unsigned char>(out[3]), 0x80);
}

TEST(Utf16, DanglingHighSurrogateBecomesReplacement) {
    // High surrogate with no following low surrogate -> U+FFFD (EF BF BD).
    const uint16_t broken[] = {0xD83D, 0};
    std::string out = utf16leToUtf8(broken, 1);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(out[0]), 0xEF);
    EXPECT_EQ(static_cast<unsigned char>(out[1]), 0xBF);
    EXPECT_EQ(static_cast<unsigned char>(out[2]), 0xBD);
}

TEST(Utf16, NulTerminates) {
    // NT name strings are NUL-terminated; text after the NUL must be ignored.
    const uint16_t withNul[] = {'A', 'B', 0, 'C', 'D'};
    EXPECT_EQ(utf16leToUtf8(withNul, 5), "AB");
}

// ---------------------------------------------------------------------------
// Update Sequence Array fixup
// ---------------------------------------------------------------------------
TEST(UsaFixup, RejectsBadParameters) {
    EXPECT_FALSE(applyUsaFixup(nullptr, 1024, 512, 0x2A, 5));
    uint8_t buf[1024];
    EXPECT_FALSE(applyUsaFixup(buf, 1024, 0, 0x2A, 5));    // zero sector size
    EXPECT_FALSE(applyUsaFixup(buf, 1024, 512, 0x2A, 1));  // seq only, no entries
}

TEST(UsaFixup, RestoresOriginalTrailingBytes) {
    // Build a 1024-byte record made of two 512-byte sectors. Stamp the last
    // two bytes of each sector with a sequence number, and record the real
    // bytes ("AB" and "CD") in the USA.
    std::vector<uint8_t> rec(1024, 0x11);
    constexpr uint16_t seq = 0x1234;
    constexpr uint16_t usaOffset = 0x30; // arbitrary, inside the record

    // Sector 1 tail (offset 510), sector 2 tail (offset 1022).
    *reinterpret_cast<uint16_t*>(rec.data() + 510) = seq;
    *reinterpret_cast<uint16_t*>(rec.data() + 1022) = seq;

    uint16_t* usa = reinterpret_cast<uint16_t*>(rec.data() + usaOffset);
    usa[0] = seq;
    usa[1] = 0x4241; // "AB" LE
    usa[2] = 0x4443; // "CD" LE

    ASSERT_TRUE(applyUsaFixup(rec.data(), 1024, 512, usaOffset, 3));
    EXPECT_EQ(*reinterpret_cast<uint16_t*>(rec.data() + 510), 0x4241);
    EXPECT_EQ(*reinterpret_cast<uint16_t*>(rec.data() + 1022), 0x4443);
}

TEST(UsaFixup, ReturnsFalseOnSequenceMismatch) {
    // If a sector's trailing bytes don't match the sequence number, the record
    // is partially overwritten and the fixup must fail (callers then treat the
    // record as suspect rather than silently trusting restored bytes).
    std::vector<uint8_t> rec(1024, 0x00);
    constexpr uint16_t seq = 0x1111;
    constexpr uint16_t usaOffset = 0x30;

    *reinterpret_cast<uint16_t*>(rec.data() + 510) = seq;
    *reinterpret_cast<uint16_t*>(rec.data() + 1022) = 0x2222; // wrong -> mismatch

    uint16_t* usa = reinterpret_cast<uint16_t*>(rec.data() + usaOffset);
    usa[0] = seq;
    usa[1] = 0xAA;
    usa[2] = 0xBB;

    EXPECT_FALSE(applyUsaFixup(rec.data(), 1024, 512, usaOffset, 3));
}

TEST(NtfsBoot, ParsesMftLcnAndRecordSize) {
    std::vector<uint8_t> boot(512, 0);
    std::memcpy(boot.data() + 3, "NTFS    ", 8);
    boot[0x0B] = 0x00;
    boot[0x0C] = 0x02;
    boot[0x0D] = 8;
    boot[0x30] = 1;
    boot[0x40] = 0xF6;
    uint32_t bps = 0, spc = 0, rec = 0;
    uint64_t lcn = 0;
    ASSERT_TRUE(byteback::ntfs::parseNtfsBoot(boot.data(), boot.size(), bps, spc, lcn, rec));
    EXPECT_EQ(bps, 512u);
    EXPECT_EQ(spc, 8u);
    EXPECT_EQ(lcn, 1u);
    EXPECT_EQ(rec, 1024u);
}

static void appendFileNameIndexEntry(std::vector<uint8_t>& buf, uint64_t childMft,
                                     uint64_t parentMft, const char* name, bool last) {
    const size_t nameLen = std::strlen(name);
    const uint16_t keyLen = static_cast<uint16_t>(66 + nameLen * 2);
    const uint16_t entryLen = static_cast<uint16_t>(16 + keyLen);
    const size_t start = buf.size();
    buf.resize(start + entryLen, 0);
    auto w64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) buf[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    auto w16 = [&](size_t off, uint16_t v) {
        buf[off] = static_cast<uint8_t>(v & 0xFF);
        buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    w64(start, childMft);
    w16(start + 8, entryLen);
    w16(start + 10, keyLen);
    w16(start + 12, last ? 0x02 : 0);
    w64(start + 16, parentMft);
    buf[start + 16 + 64] = static_cast<uint8_t>(nameLen);
    buf[start + 16 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i) {
        buf[start + 16 + 66 + i * 2] = static_cast<uint8_t>(name[i]);
    }
}

TEST(IndexRoot, SlackNameSurvivesAfterUsedWindow) {
    std::vector<uint8_t> root(32, 0);
    root[0] = 0x30;
    auto w32 = [&](size_t off, uint32_t v) {
        root[off] = static_cast<uint8_t>(v & 0xFF);
        root[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        root[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        root[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    w32(16, 16);
    appendFileNameIndexEntry(root, 7, 5, "live.txt", false);
    const size_t lastAt = root.size();
    root.resize(lastAt + 16, 0);
    root[lastAt + 8] = 16;
    root[lastAt + 12] = 0x02;
    const uint32_t usedEnd = static_cast<uint32_t>(root.size() - 16);
    appendFileNameIndexEntry(root, 3, 5, "gone.txt", false);
    w32(20, usedEnd);
    w32(24, static_cast<uint32_t>(root.size() - 16));

    auto hints = byteback::ntfs::parseIndexRoot(root.data(), root.size());
    bool live = false, slack = false;
    for (const auto& h : hints) {
        if (h.name == "live.txt" && !h.fromSlack) live = true;
        if (h.name == "gone.txt" && h.fromSlack) slack = true;
    }
    EXPECT_TRUE(live);
    EXPECT_TRUE(slack);
}
