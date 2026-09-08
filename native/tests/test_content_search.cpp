#include "byteback_db.h"
#include "search/content_search.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>
#include <chrono>
#include <atomic>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace byteback;

namespace {
// CA-054: per-process DB name — two concurrent byteback_tests.exe instances
// (multi-lane sweeps share the machine temp dir) used to fight over the fixed
// path: the second open hit a locked/corrupted SQLite file.
int testPid() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
} // namespace

class ContentSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("byteback_content_search_test_" + std::to_string(testPid()) + ".db"))
                    .string();
        std::filesystem::remove(path_);
        ASSERT_TRUE(store_.open(path_));
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    MetadataStore store_;
    std::string path_;
};

TEST_F(ContentSearchTest, ContentFtsFindsIndexedText) {
    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "note.txt";
    r.sizeBytes = 64;
    r.startSector = 1;
    r.endSector = 2;
    r.status = 0;
    int64_t fileId = store_.insertFile(scanId, r);
    ASSERT_GT(fileId, 0);

    ASSERT_TRUE(store_.upsertContentSample(scanId, fileId, "hello byteback recovery content"));
    auto ids = store_.searchContentFts(scanId, "byteback", 0, 10);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], fileId);
}

TEST_F(ContentSearchTest, FindsSecretInMemoryDisk) {
    std::vector<uint8_t> img(512 * 4, 0);
    const char payload[] = "SECRET_PHRASE_XYZ";
    std::memcpy(img.data() + 512, payload, sizeof(payload));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "data.bin";
    r.sizeBytes = sizeof(payload) - 1;
    r.startSector = 1;
    r.endSector = 2;
    r.status = 0;
    store_.insertFile(scanId, r);

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "SECRET_PHRASE", {}, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].name, "data.bin");
    EXPECT_TRUE(store_.isContentIndexComplete(scanId));
}

TEST_F(ContentSearchTest, SearchFilesCategoryFilter) {
    int64_t scanId = store_.createScan(0, "quick", 100);

    FileRecord img;
    img.name = "photo.jpg";
    img.path = "/pics/photo.jpg";
    img.category = "Image";
    img.sizeBytes = 1;
    img.status = 0;
    store_.insertFile(scanId, img);

    FileRecord doc;
    doc.name = "fatura.pdf";
    doc.path = "/docs/fatura.pdf";
    doc.category = "Document";
    doc.sizeBytes = 1;
    doc.status = 0;
    store_.insertFile(scanId, doc);

    auto docs = store_.searchFiles(scanId, "fatura", 0, 10, false, "Document");
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs[0].category, "Document");

    auto images = store_.searchFiles(scanId, "photo", 0, 10, false, "Image");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].category, "Image");
}

TEST_F(ContentSearchTest, IndexesPastFirst256KiB) {
    const size_t ss = 512;
    const size_t sectors = 800;
    std::vector<uint8_t> img(ss * sectors, 0);
    const char payload[] = "SECRET_TAIL_XYZ";
    std::memcpy(img.data() + 300 * 1024, payload, sizeof(payload) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "big.bin";
    r.sizeBytes = 400 * 1024;
    r.startSector = 0;
    r.endSector = sectors;
    r.status = 0;
    store_.insertFile(scanId, r);

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "SECRET_TAIL", {}, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].name, "big.bin");
}

// A query whose bytes straddle a chunk boundary (here "SECRET" split into
// "SEC"+"RET" at offset 512) must still match — live path and FTS path.
TEST_F(ContentSearchTest, FindsQuerySpanningChunkBoundary) {
    std::vector<uint8_t> img(512 * 64, 0);
    const char payload[] = "SECRET";
    std::memcpy(img.data() + 509, payload, 6); // straddles the 512-byte boundary

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "span.bin";
    r.sizeBytes = 1024;
    r.startSector = 0;
    r.endSector = 2;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.chunkBytes = 512;

    std::vector<FileRecord> liveHits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "SECRET", opts, [&](const FileRecord& f) {
        liveHits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(liveHits.size(), 1u);
    EXPECT_EQ(liveHits[0].name, "span.bin");

    // Once the index is complete, the FTS path must find it too.
    ASSERT_TRUE(store_.isContentIndexComplete(scanId));
    std::vector<FileRecord> ftsHits;
    runContentSearch(store_, reader, scanId, "SECRET", opts, [&](const FileRecord& f) {
        ftsHits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(ftsHits.size(), 1u);
    EXPECT_EQ(ftsHits[0].name, "span.bin");
}

// Many chunks must be flushed to the store in bounded batches (memory safety
// on huge files) without losing content near the end of the file.
TEST_F(ContentSearchTest, LargeFileBatchedFlushKeepsTailFindable) {
    const size_t ss = 512;
    const size_t sectors = 512; // 256 KiB image
    std::vector<uint8_t> img(ss * sectors, 0);
    const char payload[] = "BATCH_TAIL_TOKEN";
    std::memcpy(img.data() + ss * sectors - 64, payload, sizeof(payload) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "many.bin";
    r.sizeBytes = ss * sectors;
    r.startSector = 0;
    r.endSector = sectors;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.chunkBytes = 1024; // 256 chunks -> multiple internal flush batches

    std::vector<FileRecord> liveHits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "BATCH_TAIL", opts, [&](const FileRecord& f) {
        liveHits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(liveHits.size(), 1u);

    ASSERT_TRUE(store_.isContentIndexComplete(scanId));
    auto ids = store_.searchContentFts(scanId, "BATCH_TAIL", 0, 10);
    ASSERT_EQ(ids.size(), 1u);
}

// CA-031: an icontains query whose bytes straddle the DEFAULT 256 KiB chunk
// boundary (the historical miss class) must return a snippet with a highlight
// span that exactly covers the match — on the live path and, after indexing,
// on the FTS path. The count path must stay unaffected.
TEST_F(ContentSearchTest, SnippetSpansDefault256KiBChunkBoundary) {
    const size_t ss = 512;
    const size_t sectors = 1024; // 512 KiB image
    // 0x00 filler sanitizes to spaces -> separate FTS tokens; a letter filler
    // would merge the payload into one giant token the FTS path cannot match.
    std::vector<uint8_t> img(ss * sectors, 0x00);
    const char payload[] = "GRANT_SECRET_2024";
    const size_t payloadOff = 256 * 1024 - 9; // straddles the 256 KiB boundary
    std::memcpy(img.data() + payloadOff, payload, sizeof(payload) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "grant.bin";
    r.sizeBytes = ss * sectors;
    r.startSector = 0;
    r.endSector = sectors;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts; // default 256 KiB chunks
    const std::string query = "GRANT_SECRET";

    // Live path (index still incomplete): exact snippet + highlight span.
    std::vector<FileRecord> liveHits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, query, opts, [&](const FileRecord& f) {
        liveHits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(liveHits.size(), 1u);
    const FileRecord& live = liveHits[0];
    EXPECT_FALSE(live.snippet.empty());
    EXPECT_GE(live.snippet.size(), 160u); // ~160 bytes of context requested
    ASSERT_GE(live.snippetMatchStart, 0);
    ASSERT_GT(live.snippetMatchEnd, live.snippetMatchStart);
    EXPECT_EQ(live.snippet.substr(static_cast<size_t>(live.snippetMatchStart),
                                  static_cast<size_t>(live.snippetMatchEnd - live.snippetMatchStart)),
              query);

    // Count path untouched by the snippet work.
    EXPECT_EQ(searchFileContentCount(store_, reader, scanId, query, opts), 1);

    // FTS path (index complete now): chunk 0 stores [0, 256K + 4K overlap),
    // so the straddling match is re-locatable and keeps its exact span.
    ASSERT_TRUE(store_.isContentIndexComplete(scanId));
    std::vector<FileRecord> ftsHits;
    runContentSearch(store_, reader, scanId, query, opts, [&](const FileRecord& f) {
        ftsHits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(ftsHits.size(), 1u);
    const FileRecord& fts = ftsHits[0];
    EXPECT_FALSE(fts.snippet.empty());
    ASSERT_GE(fts.snippetMatchStart, 0);
    ASSERT_GT(fts.snippetMatchEnd, fts.snippetMatchStart);
    EXPECT_EQ(fts.snippet.substr(static_cast<size_t>(fts.snippetMatchStart),
                                 static_cast<size_t>(fts.snippetMatchEnd - fts.snippetMatchStart)),
              query);
}

// CA-031: regex queries locate their FIRST match; the span covers exactly the
// regex match text. Invalid patterns match nothing (never throw).
TEST_F(ContentSearchTest, SnippetRegexQueryFirstMatch) {
    std::vector<uint8_t> img(512 * 16, 0);
    const char before[] = "noise noise ";
    const char match[] = "TOKEN_12345";
    std::memcpy(img.data() + 100, before, sizeof(before) - 1);
    std::memcpy(img.data() + 100 + sizeof(before) - 1, match, sizeof(match) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "re.bin";
    r.sizeBytes = img.size();
    r.startSector = 0;
    r.endSector = 16;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.useRegex = true;

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "TOKEN_[0-9]+", opts, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FALSE(hits[0].snippet.empty());
    ASSERT_GE(hits[0].snippetMatchStart, 0);
    ASSERT_GT(hits[0].snippetMatchEnd, hits[0].snippetMatchStart);
    EXPECT_EQ(hits[0].snippet.substr(static_cast<size_t>(hits[0].snippetMatchStart),
                                     static_cast<size_t>(hits[0].snippetMatchEnd - hits[0].snippetMatchStart)),
              match);

    // Invalid regex: no crash, no matches.
    std::vector<FileRecord> bad;
    runContentSearch(store_, reader, scanId, "TOKEN_[0-9", opts, [&](const FileRecord& f) {
        bad.push_back(f);
    }, nullptr, &running);
    EXPECT_TRUE(bad.empty());
}

// CA-055: a snippet window cut out of raw bytes must keep LENGTH and OFFSET
// stability across sanitization. The window contains an overlong sequence
// (C0 81), a UTF-16 surrogate (ED A0 80) and a multi-byte sequence truncated
// by the window edge (E4 B8 at bytes 158-159) — sequences V8 would re-encode
// as U+FFFD, shifting every byte offset. The lenient-preserving pass turns
// each invalid BYTE into one '?', so snippet.size() == window size, the ASCII
// match span survives verbatim, and the output is valid UTF-8 (renderer's
// highlight.ts byteOffsetToUnitIndex conversion stays anchored).
TEST_F(ContentSearchTest, SnippetSanitizeKeepsLengthAndOffsets) {
    const size_t ss = 512;
    std::vector<uint8_t> img(ss * 2, 'A');
    std::memcpy(img.data() + 0, "0123456789", 10);
    std::memcpy(img.data() + 10, "MATCH_ME", 8);   // span 10..18
    img[18] = 0xC0; img[19] = 0x81;                // overlong 'A'
    img[20] = 0xED; img[21] = 0xA0; img[22] = 0x80; // surrogate U+D800
    // Bytes 158-159: E4 B8 — the 160-byte snippet window ends mid-sequence.
    img[158] = 0xE4; img[159] = 0xB8;

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "raw.bin";
    r.sizeBytes = ss * 2;
    r.startSector = 0;
    r.endSector = 2;
    r.status = 0;
    store_.insertFile(scanId, r);

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "MATCH_ME", {}, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(hits.size(), 1u);
    const FileRecord& h = hits[0];

    ASSERT_GE(h.snippetMatchStart, 0);
    ASSERT_GT(h.snippetMatchEnd, h.snippetMatchStart);
    EXPECT_EQ(h.snippetMatchStart, 10);
    EXPECT_EQ(h.snippetMatchEnd, 18);
    EXPECT_EQ(h.snippet.size(), 160u); // 1:1: window length survived sanitize
    EXPECT_EQ(h.snippet.substr(10, 8), "MATCH_ME");
    EXPECT_EQ(h.snippet.substr(18, 5), "?????"); // overlong + surrogate, byte per byte
    EXPECT_EQ(h.snippet.substr(158, 2), "??");   // sequence truncated at window edge

    // Validity: the whole snippet is strict UTF-8, so the renderer conversion
    // introduces no U+FFFD and holds the offsets above.
    const auto* p = reinterpret_cast<const unsigned char*>(h.snippet.data());
    size_t i = 0;
    bool validUtf8 = true;
    while (i < h.snippet.size()) {
        const unsigned char c = p[i];
        size_t need = 0;
        unsigned char min2 = 0x80, max2 = 0xBF;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) min2 = 0xA0; else if (c == 0xED) max2 = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) min2 = 0x90; else if (c == 0xF4) max2 = 0x8F; }
        else { validUtf8 = false; break; }
        if (i + need >= h.snippet.size() || p[i + 1] < min2 || p[i + 1] > max2) { validUtf8 = false; break; }
        for (size_t k = 2; k <= need; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { validUtf8 = false; break; }
        }
        if (!validUtf8) break;
        i += need + 1;
    }
    EXPECT_TRUE(validUtf8);
}

// ReDoS guard regression pin: a nested-quantifier pattern ("(a+)+$") must fall
// back to literal search instead of compiling — the compiled form explodes on
// an 'a'*N+'b' haystack (renderer's isSafeHighlightRegex policy mirrored).
TEST_F(ContentSearchTest, BacktrackingRegexFallsBackToLiteral) {
    std::vector<uint8_t> img(512 * 4, '.');
    for (int i = 0; i < 64; ++i) img[100 + i] = 'a';
    img[164] = 'b'; // classic (a+)+$ bomb trigger

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "bomb.bin";
    r.sizeBytes = img.size();
    r.startSector = 0;
    r.endSector = 4;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.useRegex = true;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "(a+)+$", opts, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    // Literal fallback: the literal string "(a+)+$" does not occur → no hits,
    // and the scan completes in bounded time (pre-fix: minutes-to-forever).
    EXPECT_TRUE(hits.empty());
    EXPECT_LT(elapsedMs, 10000);
}

// CA-031 regression pin: the snippet window must never ship a WRONG span.
// A match longer than the ~160B window (regex [A-Z]{300}) cannot fit — the
// record must carry a snippet with NO span (-1/-1), not a clipped fake span.
TEST_F(ContentSearchTest, SnippetMatchLongerThanWindowShipsNoSpan) {
    std::vector<uint8_t> img(512 * 4, '.');
    std::memset(img.data() + 100, 'A', 300); // match (300B) > window (160B)

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "long.bin";
    r.sizeBytes = img.size();
    r.startSector = 0;
    r.endSector = 4;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.useRegex = true;

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "A{300}", opts, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FALSE(hits[0].snippet.empty());
    EXPECT_EQ(hits[0].snippetMatchStart, -1);
    EXPECT_EQ(hits[0].snippetMatchEnd, -1);
}

// CA-031 regression pin: a zero-width regex (e.g. "Z*") matches the empty
// string at every position — it must NOT turn every file into a hit (result
// flood) and must never produce a 0-length span. Parity with the renderer's
// zero-width guard in buildMatchParts.
TEST_F(ContentSearchTest, ZeroWidthRegexMatchIsNotAHit) {
    std::vector<uint8_t> img(512 * 4, 0);
    const char payload[] = "PLAIN_TEXT_DATA";
    std::memcpy(img.data() + 512, payload, sizeof(payload) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "zero.bin";
    r.sizeBytes = img.size();
    r.startSector = 0;
    r.endSector = 4;
    r.status = 0;
    store_.insertFile(scanId, r);

    ContentSearchOptions opts;
    opts.useRegex = true;

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "Z*", opts, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    EXPECT_TRUE(hits.empty());
}

// CA-031 regression pin: when only a LATER chunk matches, the FTS path can
// only reach chunk 0 through getContentSample — it must ship the head snippet
// with NO span instead of highlighting a wrong position.
TEST_F(ContentSearchTest, FtsMatchOnlyInLaterChunkShipsHeadSnippetWithoutSpan) {
    const size_t ss = 512;
    const size_t sectors = 1024; // 512 KiB image -> 2 default chunks
    std::vector<uint8_t> img(ss * sectors, 0x00);
    const char payload[] = "DEEP_TAIL_TOKEN";
    std::memcpy(img.data() + 300 * 1024, payload, sizeof(payload) - 1); // chunk 1

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "deep.bin";
    r.sizeBytes = ss * sectors;
    r.startSector = 0;
    r.endSector = sectors;
    r.status = 0;
    store_.insertFile(scanId, r);

    // Index the file first (walk once with a neutral literal that cannot hit).
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "NO_SUCH_TOKEN_ANYWHERE", {}, [](const FileRecord&) {},
                     nullptr, &running);
    ASSERT_TRUE(store_.isContentIndexComplete(scanId));

    // FTS path now serves the DEEP_TAIL_TOKEN hit.
    std::vector<FileRecord> hits;
    runContentSearch(store_, reader, scanId, "DEEP_TAIL_TOKEN", {}, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(hits.size(), 1u);
    const FileRecord& f = hits[0];
    EXPECT_FALSE(f.snippet.empty());                    // head snippet present
    EXPECT_EQ(f.snippet.find("DEEP_TAIL_TOKEN"), std::string::npos); // wrong-chunk content must not leak in
    EXPECT_EQ(f.snippetMatchStart, -1);                 // and no fabricated span
    EXPECT_EQ(f.snippetMatchEnd, -1);
}


// CA-031 invariant pin: sanitization maps every input byte 1:1 (kept byte or
// '.'), so byte offsets computed BEFORE sanitization stay valid AFTER it —
// in BOTH modes (lenient passthrough and aggressive binary replacement).
TEST(ContentSearchSnippet, SanitizeSnippetContextPreservesByteLength) {
    // Lenient mode (>=80% printable): bytes >=128 pass through verbatim.
    std::string text = "text text text \xC5\x9F\x01"; // 16 bytes, 1 control byte
    std::string out = sanitizeSnippetContext(text);
    ASSERT_EQ(out.size(), text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c < 127) || c >= 128) {
            EXPECT_EQ(static_cast<unsigned char>(out[i]), c) << "i=" << i;
        } else {
            EXPECT_EQ(out[i], '.') << "i=" << i;
        }
    }

    // Aggressive mode (<80% printable): every non-text byte becomes '.', but
    // the 1:1 byte mapping (and thus offset validity) still holds.
    std::string raw = "ab\x01\xFF\xC5\x9F\x80\x01\x02\xFE"; // mostly binary
    std::string bin = sanitizeSnippetContext(raw);
    ASSERT_EQ(bin.size(), raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c < 127)) {
            EXPECT_EQ(static_cast<unsigned char>(bin[i]), c) << "i=" << i;
        } else {
            EXPECT_EQ(bin[i], '.') << "i=" << i;
        }
    }
}

// CA-031 binary-context guard: when the bytes around the match are mostly
// non-text (<80% printable), the snippet is still returned but sanitized
// aggressively — every non-text byte becomes '.'; the match itself stays
// visible and highlighted.
TEST_F(ContentSearchTest, SnippetBinaryContextSanitizedAggressively) {
    std::vector<uint8_t> img(512 * 4, 0xFF); // non-text filler
    const char payload[] = "SECRET";
    std::memcpy(img.data() + 512, payload, sizeof(payload) - 1);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    int64_t scanId = store_.createScan(0, "quick", 100);
    FileRecord r;
    r.name = "bin.bin";
    r.sizeBytes = img.size();
    r.startSector = 1;
    r.endSector = 2;
    r.status = 0;
    store_.insertFile(scanId, r);

    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    runContentSearch(store_, reader, scanId, "SECRET", {}, [&](const FileRecord& f) {
        hits.push_back(f);
    }, nullptr, &running);
    ASSERT_EQ(hits.size(), 1u);
    const FileRecord& f = hits[0];
    ASSERT_FALSE(f.snippet.empty());
    ASSERT_GE(f.snippetMatchStart, 0);
    ASSERT_GT(f.snippetMatchEnd, f.snippetMatchStart);
    EXPECT_EQ(f.snippet.substr(static_cast<size_t>(f.snippetMatchStart),
                               static_cast<size_t>(f.snippetMatchEnd - f.snippetMatchStart)),
              "SECRET");
    // Aggressive output: printable ASCII only (letters, spaces, dots).
    for (unsigned char c : f.snippet) {
        ASSERT_GE(c, 32) << "byte " << static_cast<int>(c);
        ASSERT_LE(c, 126) << "byte " << static_cast<int>(c);
    }
    EXPECT_NE(f.snippet.find('.'), std::string::npos); // filler was replaced
}
