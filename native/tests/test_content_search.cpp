#include "byteback_db.h"
#include "search/content_search.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>

using namespace byteback;

class ContentSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() / "byteback_content_search_test.db").string();
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
