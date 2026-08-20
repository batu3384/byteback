#include "io/volume_mapper_win.h"
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

TEST(VolumeMapper, FsKindLabels) {
    EXPECT_STREQ(volumeFsKindLabel(VolumeFsKind::Ntfs), "ntfs");
    EXPECT_STREQ(volumeFsKindLabel(VolumeFsKind::Unknown), "unknown");
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
}
#endif
