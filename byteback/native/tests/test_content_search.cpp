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
