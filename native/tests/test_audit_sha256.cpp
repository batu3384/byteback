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

// ---- Runtime chain verifier ----
TEST(AuditChain, VerifierAcceptsValidChainAndRejectsTampering) {
    const auto calc = [](const std::string& s) {
        return AuditLogger::CalculateSHA256(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };

    // Build a 3-entry valid chain with the same genesis + fold rule.
    const std::string path = "test_audit_verify.log";
    std::vector<std::string> msgs = {"EVENT | V_ONE", "EVENT | V_TWO", "EVENT | V_THREE"};
    std::string prev(64, '0');
    std::vector<std::string> lines;
    for (const auto& m : msgs) {
        std::string h = calc(prev + m);
        lines.push_back(m + " | ChainHash: " + h);
        prev = h;
    }
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (const auto& l : lines) f << l << "\n";
    }

    auto ok = forensic::VerifyAuditChainFile(path);
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.entries, 3);
    EXPECT_EQ(ok.brokenAt, 0);

    // Tamper with entry 2's payload: verifier must pin the exact line.
    lines[1].replace(lines[1].find("V_TWO"), 5, "V_TWO_FIXED");
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (const auto& l : lines) f << l << "\n";
    }
    auto bad = forensic::VerifyAuditChainFile(path);
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.brokenAt, 2);
    EXPECT_EQ(bad.entries, 2);

    // Empty file: not ok, zero entries.
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
    }
    auto empty = forensic::VerifyAuditChainFile(path);
    EXPECT_FALSE(empty.ok);
    EXPECT_EQ(empty.entries, 0);

    ::remove(path.c_str());
}

// Lane E sweep: the writer appends+flushes line by line while a scan runs, so
// a verify during ACTIVE writing can observe a torn FINAL line (no ChainHash
// or a truncated hash). That must read as "incomplete tail", not BOGUS — the
// chain up to the last complete entry is intact. A torn-looking line that has
// lines AFTER it is still a genuine break.
TEST(AuditChain, TornFinalLineIsIncompleteNotBroken) {
    const auto calc = [](const std::string& s) {
        return AuditLogger::CalculateSHA256(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    const std::string path = "test_audit_torn.log";
    std::string prev(64, '0');
    std::vector<std::string> lines;
    for (const char* m : {"EVENT | T_ONE", "EVENT | T_TWO"}) {
        const std::string h = calc(prev + m);
        lines.push_back(std::string(m) + " | ChainHash: " + h);
        prev = h;
    }
    // Line 3 is mid-write: message present, hash truncated.
    const std::string torn = "EVENT | T_THREE | ChainHash: abcdef";

    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << lines[0] << "\n" << lines[1] << "\n" << torn; // no trailing \n
    }
    auto r = forensic::VerifyAuditChainFile(path);
    EXPECT_TRUE(r.ok);            // first two links verified
    EXPECT_EQ(r.entries, 2);
    EXPECT_EQ(r.brokenAt, 0);

    // Same torn line, but more lines follow it: genuine corruption.
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << lines[0] << "\n" << torn << "\n" << lines[1] << "\n";
    }
    auto bad = forensic::VerifyAuditChainFile(path);
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.brokenAt, 2);

    ::remove(path.c_str());
}

// ---- CA-028 flush hardening ----
// Crash-evidence categories (RECOVER*/WIPE*/BITLOCKER*/FILE_RECOVERED) must be
// durable the moment their log call returns: a hard crash right after must not
// be able to drop them from the chain. Standalone instances (public ctor)
// model the crash-killed logger without terminating the process singleton.
TEST(AuditChain, CriticalEventsAreDurableImmediatelyAfterCall) {
    const std::string path = "test_audit_critical.log";
    { std::ofstream f(path, std::ios::trunc); } // start clean

    {
        AuditLogger local; // crash-simulation scope
        local.Initialize(path);
        local.LogEvent("RECOVER_BEGIN | fileId=7");
        local.LogEvent("WIPE_END | passes=3");
        local.LogEvent("BITLOCKER_UNLOCK | volume=C:");

        // "Crash" happens HERE: read the file with no Shutdown/FlushPending.
        // All three evidence lines must already be on disk, chain intact.
        std::ifstream in(path);
        ASSERT_TRUE(in.is_open());
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        ASSERT_EQ(lines.size(), 3u);
        EXPECT_NE(lines[0].find("EVENT | RECOVER_BEGIN | fileId=7"), std::string::npos);
        EXPECT_NE(lines[1].find("EVENT | WIPE_END | passes=3"), std::string::npos);
        EXPECT_NE(lines[2].find("EVENT | BITLOCKER_UNLOCK | volume=C:"), std::string::npos);
        for (const auto& l : lines) {
            EXPECT_NE(l.find(" | ChainHash: "), std::string::npos);
        }
        auto r = forensic::VerifyAuditChainFile(path);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.entries, 3);
        EXPECT_EQ(r.brokenAt, 0);
    } // destructor: clean teardown after the property held
    ::remove(path.c_str());
}

// Non-critical entries ride the async queue; FlushPending() must drain the
// queue synchronously so a caller can force durability at a checkpoint.
TEST(AuditChain, FlushPendingDrainsAsyncQueueSynchronously) {
    const std::string path = "test_audit_flush.log";
    { std::ofstream f(path, std::ios::trunc); }

    AuditLogger local;
    local.Initialize(path);
    local.LogEvent("FLUSH_CHECK_A | n=1"); // queued, not critical
    local.LogEvent("FLUSH_CHECK_B | n=2");
    local.FlushPending();                  // after this returns, both are durable

    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("EVENT | FLUSH_CHECK_A | n=1"), std::string::npos);
    EXPECT_NE(lines[1].find("EVENT | FLUSH_CHECK_B | n=2"), std::string::npos);

    auto r = forensic::VerifyAuditChainFile(path);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.entries, 2);
    local.Shutdown();
    ::remove(path.c_str());
}

// CA-028 adversarial: a tampered FINAL line that still carries a complete,
// well-formed 64-hex hash (payload rewritten, old hash left in place) must be
// reported as BROKEN — an append+flush writer can truncate a line but never
// fabricate a full-length wrong hash. Reporting it as "incomplete tail" would
// silently bless tampering of the most recent entry.
TEST(AuditChain, TamperedFinalLineWithWellFormedHashIsBroken) {
    const auto calc = [](const std::string& s) {
        return forensic::AuditLogger::CalculateSHA256(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    const std::string path = "test_audit_final_tamper.log";
    std::string prev(64, '0');
    std::vector<std::string> lines;
    for (const char* m : {"EVENT | F_ONE", "EVENT | F_TWO", "EVENT | F_THREE"}) {
        const std::string h = calc(prev + m);
        lines.push_back(std::string(m) + " | ChainHash: " + h);
        prev = h;
    }
    // Tamper the FINAL entry's payload, keeping its (now wrong) 64-hex hash.
    lines[2].replace(lines[2].find("F_THREE"), 7, "F_THREE_WIPED");
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (const auto& l : lines) f << l << "\n";
    }

    auto r = forensic::VerifyAuditChainFile(path);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.brokenAt, 3);
    EXPECT_EQ(r.detail, "hash mismatch");

    ::remove(path.c_str());
}

// CA-028 adversarial: the renderer is an untrusted audit source and critical
// categories write synchronously per call — an uncapped loop would grow the
// audit file without bound and thrash the disk from the main process. The
// bridge path must reject flood-rate events (token bucket); accepted events
// stay chain-intact and exactly match the on-disk line count.
TEST(AuditChain, BridgeEventFloodIsRateLimited) {
    const std::string path = "test_audit_flood.log";
    { std::ofstream f(path, std::ios::trunc); }

    AuditLogger local;
    local.Initialize(path);

    int accepted = 0;
    for (int i = 0; i < 400; ++i) {
        // WIPE_* prefix -> critical category -> synchronous write per call.
        if (local.LogEventFromBridge("WIPE_FLOOD_PROBE | i=" + std::to_string(i))) ++accepted;
    }
    EXPECT_GT(accepted, 0);     // burst admits the first events
    EXPECT_LT(accepted, 400);   // a cap exists at all
    EXPECT_LE(accepted, 100);   // burst (60) + generous refill slack

    // Every accepted event is durable (sync write) and the chain still verifies.
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    std::size_t lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    EXPECT_EQ(lines, static_cast<std::size_t>(accepted));
    auto r = forensic::VerifyAuditChainFile(path);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.entries, accepted);

    local.Shutdown();
    ::remove(path.c_str());
}

// CA-028 adversarial: the event TOKEN must be non-empty — "| spoof | x" or "
// leading-space" payloads previously slipped through (the format contract is
// "UPPERCASE_TOKEN | payload"). The JS origin tag still rides first, but a
// malformed line must not enter the chain at all.
TEST(AuditChain, BridgeEventRequiresNonEmptyToken) {
    const std::string path = "test_audit_token.log";
    { std::ofstream f(path, std::ios::trunc); }

    AuditLogger local;
    local.Initialize(path);

    EXPECT_FALSE(local.LogEventFromBridge("| spoofed | payload"));
    EXPECT_FALSE(local.LogEventFromBridge(" leading-space | x"));
    EXPECT_FALSE(local.LogEventFromBridge("|"));
    EXPECT_FALSE(local.LogEventFromBridge(" "));

    local.FlushPending();
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) ++n;
    }
    EXPECT_EQ(n, 0u); // nothing entered the chain

    local.Shutdown();
    ::remove(path.c_str());
}

// Bridge-origin events (logAuditEvent export): valid tokens join the same
// hash chain tagged with the "JS" origin segment; malformed input is rejected
// without touching the file.
TEST(AuditChain, BridgeEventsJoinChainTaggedJsAndRejectBadInput) {
    const std::string path = "test_audit_bridge.log";
    { std::ofstream f(path, std::ios::trunc); }

    AuditLogger local;
    local.Initialize(path);

    // Round-trip: native event + bridge event + bridge-origin CRITICAL event
    // in one chain. FlushPending between steps pins the on-disk order (the
    // worker thread drains the async queue concurrently).
    local.LogEvent("SCAN_END | scanId=9");
    local.FlushPending(); // line 1: native event durable
    EXPECT_TRUE(local.LogEventFromBridge("UI_EXPORT_START | fileId=42"));
    local.FlushPending(); // line 2: bridge event durable
    EXPECT_TRUE(local.LogEventFromBridge("RECOVER_NOTE | stage=verify")); // sync: line 3

    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("EVENT | SCAN_END | scanId=9"), std::string::npos);
    EXPECT_NE(lines[1].find("EVENT | JS | UI_EXPORT_START | fileId=42"), std::string::npos);
    EXPECT_NE(lines[2].find("EVENT | JS | RECOVER_NOTE | stage=verify"), std::string::npos);
    auto r = forensic::VerifyAuditChainFile(path);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.entries, 3);

    // Validation: empty, non-UPPERCASE token, control chars, oversized.
    EXPECT_FALSE(local.LogEventFromBridge(""));
    EXPECT_FALSE(local.LogEventFromBridge("lowercase_token | x"));
    EXPECT_FALSE(local.LogEventFromBridge("BAD\tTOKEN | x"));
    EXPECT_FALSE(local.LogEventFromBridge("TRAILING\nNEWLINE"));
    EXPECT_FALSE(local.LogEventFromBridge(std::string(513, 'A')));
    // Still exactly 3 entries on disk after the rejects.
    local.FlushPending();
    {
        std::ifstream in2(path);
        std::size_t n = 0;
        while (std::getline(in2, line)) {
            if (!line.empty()) ++n;
        }
        EXPECT_EQ(n, 3u);
    }
    local.Shutdown();
    ::remove(path.c_str());
}
