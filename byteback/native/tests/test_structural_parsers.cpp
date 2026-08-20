#include "carver/structural_parsers.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback::carver;

namespace {

void appendLe16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void appendLe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 24) & 0xFF);
}
void appendBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}

std::vector<uint8_t> buildMinimalZip(const char* innerPath) {
    std::vector<uint8_t> b;
    static const uint8_t local[] = {'P', 'K', 0x03, 0x04};
    b.insert(b.end(), local, local + 4);
    b.insert(b.end(), 26, 0);
    if (innerPath) {
        size_t off = b.size();
        b.resize(off + std::strlen(innerPath));
        std::memcpy(b.data() + off, innerPath, std::strlen(innerPath));
    }
    static const uint8_t central[] = {'P', 'K', 0x01, 0x02};
    b.insert(b.end(), central, central + 4);
    static const uint8_t eocd[] = {'P', 'K', 0x05, 0x06};
    b.insert(b.end(), eocd, eocd + 4);
    appendLe16(b, 0); // disk
    appendLe16(b, 0); // disk start
    appendLe16(b, 1); // entries on disk
    appendLe16(b, 1); // total entries
    appendLe32(b, 30); // central dir size
    appendLe32(b, 30); // central dir offset
    appendLe16(b, 0); // comment len
    return b;
}

std::vector<uint8_t> buildMinimalSqlite(uint32_t pageSize, uint32_t pageCount) {
    std::vector<uint8_t> b(100, 0);
    std::memcpy(b.data(), "SQLite format 3", 16);
    appendBe32(b, 0); // placeholder
    b[16] = static_cast<uint8_t>((pageSize >> 8) & 0xFF);
    b[17] = static_cast<uint8_t>(pageSize & 0xFF);
    b[28] = static_cast<uint8_t>((pageCount >> 24) & 0xFF);
    b[29] = static_cast<uint8_t>((pageCount >> 16) & 0xFF);
    b[30] = static_cast<uint8_t>((pageCount >> 8) & 0xFF);
    b[31] = static_cast<uint8_t>(pageCount & 0xFF);
    b[100] = 0x0d; // btree leaf page at page 1
    b.resize(pageSize * pageCount, 0);
    return b;
}

std::vector<uint8_t> buildMinimalMp4() {
    std::vector<uint8_t> b;
    appendBe32(b, 16);
    b.insert(b.end(), {'f', 't', 'y', 'p'});
    b.insert(b.end(), {'i', 's', 'o', 'm'});
    b.insert(b.end(), 4, 0); // ftyp minor version + padding to 16-byte atom
    appendBe32(b, 16);
    b.insert(b.end(), {'m', 'o', 'o', 'v'});
    b.insert(b.end(), 8, 0);
    appendBe32(b, 20);
    b.insert(b.end(), {'m', 'd', 'a', 't'});
    b.insert(b.end(), 12, 0xAB);
    return b;
}

} // namespace

TEST(StructuralParsers, ZipFamilyDetectsDocxSubtype) {
    auto b = buildMinimalZip("word/document.xml");
    auto r = parseZipFamily(b.data(), b.size());
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.extension, "docx");
    EXPECT_GE(r.confidence, 85);
}

TEST(StructuralParsers, ZipFamilyEocdBoundsSize) {
    auto b = buildMinimalZip(nullptr);
    auto r = parseZipFamily(b.data(), b.size());
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.size, b.size());
}

TEST(StructuralParsers, SqliteHeaderEstimatesSize) {
    auto b = buildMinimalSqlite(4096, 4);
    auto r = parseSqliteDb(b.data(), b.size());
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.size, 4096u * 4);
    EXPECT_GE(r.confidence, 80);
}

TEST(StructuralParsers, Mp4AtomWalkFindsMdatEnd) {
    auto b = buildMinimalMp4();
    auto r = parseMp4Mov(b.data(), b.size());
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.size, b.size());
    EXPECT_GE(r.confidence, 75);
}

TEST(StructuralParsers, Mp4RejectsGarbage) {
    const uint8_t g[] = {0, 0, 0, 0, 'f', 't', 'y', 'p'};
    auto r = parseMp4Mov(g, sizeof(g));
    EXPECT_FALSE(r.valid);
}
