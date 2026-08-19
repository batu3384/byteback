#include "forensic/nsrl_lookup.h"

#include <gtest/gtest.h>
#include <fstream>

using forensic::NsrlLookup;

TEST(NsrlLookup, LoadsBareHexLines) {
    const char* path = "test_nsrl_subset.txt";
    {
        std::ofstream out(path);
        out << "# empty file md5\n";
        out << "d41d8cd98f00b204e9800998ecf8427e\n";
        out << "0CC175B9C0F1B6A831C399E269772661\n";
    }

    NsrlLookup nsrl;
    ASSERT_TRUE(nsrl.loadFromFile(path));
    EXPECT_EQ(nsrl.size(), 2u);
    EXPECT_TRUE(nsrl.contains("d41d8cd98f00b204e9800998ecf8427e"));
    EXPECT_TRUE(nsrl.contains("0cc175b9c0f1b6a831c399e269772661"));
    EXPECT_FALSE(nsrl.contains("900150983cd24fb0d6963f7d28e17f72"));
    std::remove(path);
}

TEST(NsrlLookup, AcceptsCsvFirstColumn) {
    const char* path = "test_nsrl_csv.txt";
    {
        std::ofstream out(path);
        out << "900150983cd24fb0d6963f7d28e17f72,\"abc.txt\",123\n";
    }

    NsrlLookup nsrl;
    ASSERT_TRUE(nsrl.loadFromFile(path));
    EXPECT_EQ(nsrl.size(), 1u);
    EXPECT_TRUE(nsrl.contains("900150983cd24fb0d6963f7d28e17f72"));
    std::remove(path);
}

TEST(NsrlLookup, RejectsInvalidHashes) {
    NsrlLookup nsrl;
    EXPECT_FALSE(nsrl.contains("not-a-hash"));
    EXPECT_FALSE(nsrl.contains("abc"));
}

TEST(NsrlLookup, OfficialRdsUsesMd5ColumnNotSha1) {
    const char* path = "test_nsrl_rds.txt";
    {
        std::ofstream out(path);
        out << "da39a3ee5e6b4b0d3255bfef95601890afd80709,d41d8cd98f00b204e9800998ecf8427e,00000000\n";
    }

    NsrlLookup nsrl;
    ASSERT_TRUE(nsrl.loadFromFile(path));
    EXPECT_EQ(nsrl.size(), 1u);
    EXPECT_TRUE(nsrl.contains("d41d8cd98f00b204e9800998ecf8427e"));
    std::remove(path);
}
