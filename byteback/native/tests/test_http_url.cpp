#include "io/byte_source.h"
#include <gtest/gtest.h>
#include <string>

using namespace byteback;

TEST(HttpUrl, BlocksPrivateAndLocalHosts) {
    EXPECT_FALSE(httpUrlHostAllowed("http://127.0.0.1/image.e01"));
    EXPECT_FALSE(httpUrlHostAllowed("http://localhost/"));
    EXPECT_FALSE(httpUrlHostAllowed("https://192.168.0.1/x"));
    EXPECT_FALSE(httpUrlHostAllowed("http://10.0.0.5/"));
    EXPECT_FALSE(httpUrlHostAllowed("http://172.16.1.1/"));
    EXPECT_FALSE(httpUrlHostAllowed("http://169.254.169.254/"));
}

TEST(HttpUrl, AllowsPublicHostnames) {
    EXPECT_TRUE(httpUrlHostAllowed("https://example.com/image.e01"));
    EXPECT_TRUE(httpUrlHostAllowed("http://forensics.example.org/data.raw"));
}

TEST(HttpUrl, RejectsNonHttpSchemes) {
    EXPECT_FALSE(httpUrlHostAllowed("file:///C:/secret.e01"));
    EXPECT_FALSE(httpUrlHostAllowed("ftp://example.com/x"));
}

TEST(HttpUrl, OpenHttpRejectsBlockedHost) {
    std::string err;
    auto src = openHttpByteSource("http://127.0.0.1/test.e01", err);
    EXPECT_EQ(src, nullptr);
    EXPECT_NE(err.find("not allowed"), std::string::npos);
}
