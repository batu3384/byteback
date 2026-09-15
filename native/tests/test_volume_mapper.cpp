#include "io/volume_mapper_win.h"
#include "io/hex_bind.h"
#include <gtest/gtest.h>

using namespace byteback;

TEST(VolumeMapper, NormalizeDriveLetterVariants) {
    auto d = normalizeDriveLetter(L"d:");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, L"D:");

    auto slash = normalizeDriveLetter(L"e:\\");
    ASSERT_TRUE(slash.has_value());
    EXPECT_EQ(*slash, L"E:");

    auto spaced = normalizeDriveLetter(L"  F:  ");
    ASSERT_TRUE(spaced.has_value());
    EXPECT_EQ(*spaced, L"F:");
}

TEST(VolumeMapper, RejectsInvalidLetters) {
    EXPECT_FALSE(normalizeDriveLetter(L"").has_value());
    EXPECT_FALSE(normalizeDriveLetter(L"12:").has_value());
    EXPECT_FALSE(normalizeDriveLetter(L"C:\\Windows").has_value());
    EXPECT_FALSE(normalizeDriveLetterUtf8("").has_value());
}

TEST(VolumeMapper, VolumeDevicePathShape) {
    EXPECT_TRUE(isWin32VolumeDevicePath("\\\\.\\C:"));
    EXPECT_TRUE(isWin32VolumeDevicePath("\\\\.\\d:"));
    EXPECT_FALSE(isWin32VolumeDevicePath("\\\\.\\PhysicalDrive0"));
    EXPECT_FALSE(isWin32VolumeDevicePath("\\\\.\\C:\\"));
    EXPECT_FALSE(isWin32VolumeDevicePath("C:\\"));
    EXPECT_FALSE(isWin32VolumeDevicePath("C:"));
    EXPECT_FALSE(isWin32VolumeDevicePath(""));
}

TEST(VolumeMapper, FsKindLabels) {
    EXPECT_STREQ(volumeFsKindLabel(VolumeFsKind::Ntfs), "ntfs");
    EXPECT_STREQ(volumeFsKindLabel(VolumeFsKind::Unknown), "unknown");
}

TEST(HexBind, RaidBackendOnlyForSentinelIndex) {
    EXPECT_TRUE(hexUsesRaidBackend(true, -1));
    EXPECT_FALSE(hexUsesRaidBackend(true, 0));
    EXPECT_FALSE(hexUsesRaidBackend(true, 3));
    EXPECT_FALSE(hexUsesRaidBackend(false, -1));
    EXPECT_FALSE(hexUsesRaidBackend(false, 0));
}

#ifdef _WIN32
TEST(VolumeMapper, ResolveSystemVolumeIfAccessible) {
    auto resolved = resolveDriveLetter(L"C:");
    if (!resolved) {
        GTEST_SKIP() << "C: volume not accessible (admin required)";
    }
    EXPECT_GE(resolved->driveIndex, 0);
    EXPECT_GE(resolved->partitionStartSector, 0);
    EXPECT_GE(resolved->partitionSizeSectors, 1u);
    EXPECT_FALSE(resolved->diskNumbers.empty());
    EXPECT_EQ(resolved->diskNumbers.front(), resolved->driveIndex);
    ASSERT_TRUE(isWin32VolumeDevicePath(resolved->volumePath));
}
#endif
