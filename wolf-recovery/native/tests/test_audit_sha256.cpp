// Native unit tests for the forensic audit logger's SHA-256 and its hash
// chain (CA-002). The logger's integrity claim ("hash-chained audit log")
// rests entirely on this hand-rolled implementation — RFC 6234 test vectors
// pin it to the standard, and the chain test proves the written ChainHash
// really folds the previous link in.
//
// Singleton note: AuditLogger::GetInstance() owns one worker thread for the
// process lifetime and Shutdown() is terminal, so the chain test runs exactly
// one session (fresh temp file) and verifies determinism by recomputing the
// expected links with the (now RFC-verified) CalculateSHA256 itself.
#include "forensic/audit_logger.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using forensic::AuditLogger;

// ---- RFC 6234 / FIPS 180-4 vectors ----
TEST(AuditSha256, Rfc6234Vectors) {
    auto hex = [](const char* s) {
        return AuditLogger::CalculateSHA256(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    };

    EXPECT_EQ(hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                  "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(AuditSha256, MillionA) {
    std::string big(1000000, 'a');
    EXPECT_EQ(AuditLogger::CalculateSHA256(
                  reinterpret_cast<const uint8_t*>(big.data()), big.size()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(AuditSha256, DeterministicAcrossCalls) {
    const uint8_t payload[] = {0x00, 0x01, 0x02, 0xFF, 0x80, 0x7F};
    auto h1 = AuditLogger::CalculateSHA256(payload, sizeof(payload));
    auto h2 = AuditLogger::CalculateSHA256(payload, sizeof(payload));
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 64u);

    // One-bit difference must change the digest (avalanche sanity).
    const uint8_t flipped[] = {0x00, 0x01, 0x02, 0xFF, 0x80, 0x7E};
    auto h3 = AuditLogger::CalculateSHA256(flipped, sizeof(flipped));
    EXPECT_NE(h1, h3);
}

// ---- Hash chain written to the log file ----
TEST(AuditChain, ChainHashFoldsPreviousLink) {
    const std::string path = "test_audit_chain.log";
    { std::ofstream f(path, std::ios::trunc); } // start clean

    auto& logger = AuditLogger::GetInstance();
    logger.Initialize(path);
    logger.LogEvent("CHAIN_TEST_ALPHA");
    logger.LogEvent("CHAIN_TEST_BETA");
    logger.Shutdown(); // terminal for the singleton — must be the only session

    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ::remove(path.c_str());
    ASSERT_EQ(lines.size(), 2u);

    auto splitHash = [](const std::string& l) {
        auto pos = l.rfind(" | ChainHash: ");
        return (pos == std::string::npos) ? std::string() : l.substr(pos + 14);
    };
    auto splitMsg = [](const std::string& l) {
        // message = everything before the trailing " | ChainHash: ..."
        auto pos = l.rfind(" | ChainHash: ");
        return (pos == std::string::npos) ? std::string() : l.substr(0, pos);
    };

    std::string h1 = splitHash(lines[0]);
    std::string h2 = splitHash(lines[1]);
    ASSERT_EQ(h1.size(), 64u);
    ASSERT_EQ(h2.size(), 64u);
    EXPECT_NE(h1, h2);

    // Recompute both links independently with the RFC-verified primitive.
    // Genesis link is 64 zeros (AuditLogger ctor), and each link hashes the
    // previous hex digest concatenated with the exact stored message.
    std::string genesis(64, '0');
    std::string m1 = splitMsg(lines[0]);
    std::string m2 = splitMsg(lines[1]);
    std::string e1 = genesis + m1;
    std::string e2 = h1 + m2;
    auto calc = [](const std::string& s) {
        return AuditLogger::CalculateSHA256(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    EXPECT_EQ(calc(e1), h1);
    EXPECT_EQ(calc(e2), h2);

    // The messages must carry the events we logged.
    EXPECT_NE(m1.find("EVENT | CHAIN_TEST_ALPHA"), std::string::npos);
    EXPECT_NE(m2.find("EVENT | CHAIN_TEST_BETA"), std::string::npos);
}
