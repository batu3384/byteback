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

// Range-read response validation: a server that ignores the Range header and
// replies 200 (full body from offset 0) must NOT be treated as a range read —
// silently accepting it returns wrong bytes at any non-zero offset.
TEST(HttpRange, ReadStatusValidation) {
    EXPECT_TRUE(httpRangeReadStatusOk(206, 0, 100, 1000));
    EXPECT_TRUE(httpRangeReadStatusOk(206, 500, 100, 1000));
    EXPECT_TRUE(httpRangeReadStatusOk(200, 0, 1000, 1000)); // full-object 200 == whole content
    EXPECT_FALSE(httpRangeReadStatusOk(200, 500, 100, 1000));
    EXPECT_FALSE(httpRangeReadStatusOk(200, 0, 999, 1000)); // partial object via 200
    EXPECT_FALSE(httpRangeReadStatusOk(404, 0, 100, 1000));
    EXPECT_FALSE(httpRangeReadStatusOk(302, 0, 100, 1000));
}

// Size probe: only trust Content-Length on a real success response, never on
// an error page (a 404 with Content-Length would fake a tiny image size).
TEST(HttpRange, ProbeStatusValidation) {
    EXPECT_TRUE(httpProbeStatusOk(200));
    EXPECT_TRUE(httpProbeStatusOk(206));
    EXPECT_FALSE(httpProbeStatusOk(404));
    EXPECT_FALSE(httpProbeStatusOk(302));
    EXPECT_FALSE(httpProbeStatusOk(500));
}
