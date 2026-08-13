// Native unit tests for the USN journal record parser (ntfs_util.h).
//
// $UsnJrnl:$J records per-file change events. We verify that parseUsnRecord
// correctly decodes a hand-built v2 record, rejects malformed inputs (bad
// version, truncated length, out-of-bounds name), and extracts reason flags
// + the UTF-8 name.
//
// Run with:
//   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
//   cmake --build native/build --config Release --target wolf_tests
//   ctest --test-dir native/build --output-on-failure
#include "fs/ntfs_util.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

using wolf::ntfs::parseUsnRecord;
using wolf::ntfs::UsnRecord;
using wolf::ntfs::USN_REASON_FILE_CREATE;
using wolf::ntfs::USN_REASON_FILE_DELETE;

namespace {
// Build a minimal valid USN v2 record for "secret.txt" created at MFT ref 1234.
// Returns the raw bytes; callers may then mutate fields to test failure cases.
std::vector<uint8_t> buildValidRecord() {
    // Layout (offsets): 0 recordLen, 4 majorVer, 6 minorVer, 8 fileRef,
    // 16 parentRef, 24 usn, 32 timestamp, 40 reason, 44 sourceInfo,
    // 48 securityId, 52 fileAttrs, 56 nameLen, 58 nameOff, 60 name[].
    const char* name = "secret.txt"; // 10 chars -> 20 bytes UTF-16LE
    size_t nameLen = std::strlen(name);
    uint16_t nameBytes = static_cast<uint16_t>(nameLen * 2);
    uint16_t nameOff = 60;
    uint32_t recordLen = nameOff + nameBytes;
    // Pad to 8-byte alignment (USN records are 8-aligned).
    while (recordLen % 8 != 0) recordLen++;

    std::vector<uint8_t> rec(recordLen, 0);

    auto putU32 = [&](size_t off, uint32_t v) {
        rec[off] = v & 0xFF; rec[off + 1] = (v >> 8) & 0xFF;
        rec[off + 2] = (v >> 16) & 0xFF; rec[off + 3] = (v >> 24) & 0xFF;
    };
    auto putU64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) rec[off + i] = (v >> (i * 8)) & 0xFF;
    };

    putU32(0, recordLen);
    putU32(4, 2);          // majorVersion = 2
    putU64(8, 1234);       // fileReferenceNumber
    putU64(16, 5);         // parentFileReferenceNumber (root dir)
    putU64(24, 99);        // usn
    // 32: timestamp FILETIME for 2020-01-01 ~ 132223104000000000
    putU64(32, 132223104000000000ULL);
    putU32(40, USN_REASON_FILE_CREATE); // reasonFlags
    putU32(44, 0);         // sourceInfo
    putU32(48, 0);         // securityId
    putU32(52, 0x20);      // fileAttributes = ARCHIVE
    putU32(56, nameBytes); // fileNameLength (bytes)
    putU32(58, nameOff);   // fileNameOffset — note: low 16 bits only via putU32
    // putU32 wrote 4 bytes at 56/58; fix 58 to be a clean u16 by overwriting hi bytes.
    rec[58] = nameOff & 0xFF; rec[59] = (nameOff >> 8) & 0xFF;

    // Write UTF-16LE name.
    for (size_t i = 0; i < nameLen; ++i) {
        rec[nameOff + i * 2] = static_cast<uint8_t>(name[i]);
        rec[nameOff + i * 2 + 1] = 0;
    }
    return rec;
}
} // namespace

TEST(Usn, ParsesValidV2Record) {
    auto rec = buildValidRecord();
    UsnRecord out;
    ASSERT_TRUE(parseUsnRecord(rec.data(), rec.size(), out));
    EXPECT_EQ(out.fileReference, 1234u);
    EXPECT_EQ(out.parentFileReference, 5u);
    EXPECT_EQ(out.usn, 99u);
    EXPECT_EQ(out.reasonFlags, USN_REASON_FILE_CREATE);
    EXPECT_EQ(out.fileAttributes, 0x20u);
    EXPECT_EQ(out.name, "secret.txt");
    // 2020-01-01 00:00:00 UTC = 1577836800 Unix seconds.
    EXPECT_EQ(out.timestamp, 1577836800);
}

TEST(Usn, RejectsBadVersion) {
    auto rec = buildValidRecord();
    rec[4] = 9; rec[5] = 0; // majorVersion = 9 (unsupported)
    UsnRecord out;
    EXPECT_FALSE(parseUsnRecord(rec.data(), rec.size(), out));
}

TEST(Usn, RejectsImpossibleRecordLength) {
    auto rec = buildValidRecord();
    // Claim a 0-length record.
    rec[0] = rec[1] = rec[2] = rec[3] = 0;
    UsnRecord out;
    EXPECT_FALSE(parseUsnRecord(rec.data(), rec.size(), out));
}

TEST(Usn, RejectsTruncatedBuffer) {
    auto rec = buildValidRecord();
    UsnRecord out;
    // Offer only 10 bytes — far below the 60-byte minimum header.
    EXPECT_FALSE(parseUsnRecord(rec.data(), 10, out));
    // Offer recordLength-1 — truncation.
    EXPECT_FALSE(parseUsnRecord(rec.data(), rec.size() - 1, out));
}

TEST(Usn, RejectsNameOutOfBounds) {
    auto rec = buildValidRecord();
    // Point nameOffset past the record end.
    rec[58] = 0xFF; rec[59] = 0xFF; // nameOffset = 65535
    UsnRecord out;
    EXPECT_FALSE(parseUsnRecord(rec.data(), rec.size(), out));
}

TEST(Usn, NullInputRejected) {
    UsnRecord out;
    EXPECT_FALSE(parseUsnRecord(nullptr, 100, out));
}
