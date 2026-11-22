// Btrfs parser tests — fixtures encode the REAL on-disk layout from
// include/uapi/linux/btrfs_tree.h (kernel header), not parser assumptions.
#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

namespace {

constexpr uint32_t kNode = 4096;
constexpr uint32_t kSb = 64 * 1024;

void w64(std::vector<uint8_t>& b, size_t o, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = static_cast<uint8_t>(v >> (8 * i));
}
void w32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; b[o+2] = (v>>16)&0xFF; b[o+3] = (v>>24)&0xFF;
}
void w16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; }

// disk_key {oid u64, type u8, off u64}
void diskKey(std::vector<uint8_t>& b, size_t o, uint64_t oid, uint8_t type, uint64_t off) {
    w64(b, o, oid); b[o + 8] = type; w64(b, o + 9, off);
}

// btrfs_header @node start: bytenr@0x30, fsid@0x20, generation@0x50,
// owner@0x58, nritems@0x60, level@0x64.
void nodeHeader(std::vector<uint8_t>& b, size_t base, uint64_t bytenr, uint64_t gen,
                uint64_t owner, uint32_t nritems, uint8_t level, const uint8_t* fsid) {
    std::memcpy(b.data() + base + 0x20, fsid, 16);
    w64(b, base + 0x30, bytenr);
    w64(b, base + 0x50, gen);
    w64(b, base + 0x58, owner);
    w32(b, base + 0x60, nritems);
    b[base + 0x64] = level;
}

// Leaf item: 25B TOC entry at +101; data at 101 + offset.
struct LeafItem { uint64_t oid; uint8_t type; uint64_t koff; std::vector<uint8_t> data; };
void leaf(std::vector<uint8_t>& b, size_t base, uint64_t bytenr, uint64_t gen, uint64_t owner,
          const std::vector<LeafItem>& items, const uint8_t* fsid) {
    nodeHeader(b, base, bytenr, gen, owner, static_cast<uint32_t>(items.size()), 0, fsid);
    // Real leaf layout: TOC grows forward from +101, item DATA grows backward
    // from end of node; item.offset is relative to end-of-header (+101).
    uint32_t dataTail = kNode - 101;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        dataTail -= static_cast<uint32_t>(it.data.size());
        size_t e = base + 101 + i * 25;
        diskKey(b, e, it.oid, it.type, it.koff);
        w32(b, e + 17, dataTail);
        w32(b, e + 21, static_cast<uint32_t>(it.data.size()));
        std::memcpy(b.data() + base + 101 + dataTail, it.data.data(), it.data.size());
    }
}

std::vector<uint8_t> inodeItem(uint64_t size, uint32_t mode, int64_t mtime, int64_t otime) {
    std::vector<uint8_t> d(140, 0);
    w64(d, 0, 7);        // generation
    w64(d, 16, size);
    w32(d, 40, 1);       // nlink
    w32(d, 52, mode);
    w64(d, 92, 1700000001ull);  // atime
    w64(d, 104, 1700000002ull); // ctime
    w64(d, 116, static_cast<uint64_t>(mtime));
    w64(d, 128, static_cast<uint64_t>(otime));
    return d;
}

std::vector<uint8_t> inodeRef(const std::string& name) {
    std::vector<uint8_t> d(10 + name.size());
    w64(d, 0, 1);                       // index
    w16(d, 8, static_cast<uint16_t>(name.size()));
    std::memcpy(d.data() + 10, name.data(), name.size());
    return d;
}

std::vector<uint8_t> extentData(uint64_t diskBytenr, uint64_t off, uint64_t numBytes,
                                uint8_t compr = 0) {
    std::vector<uint8_t> d(53, 0);
    w64(d, 0, 7);
    w64(d, 8, numBytes);   // ram_bytes
    d[16] = compr;
    d[20] = 1;             // REGULAR
    w64(d, 21, diskBytenr);
    w64(d, 29, numBytes);  // disk_num_bytes
    w64(d, 37, off);
    w64(d, 45, numBytes);
    return d;
}

std::vector<uint8_t> rootItem(uint64_t bytenr, uint8_t level) {
    std::vector<uint8_t> d(256, 0);
    w64(d, 140, 7);        // generation (after 140B inode item)
    w64(d, 156, bytenr);
    d[218] = level;
    return d;
}

std::vector<uint8_t> dirItem(uint64_t childOid, const std::string& name, uint8_t dtype = 1) {
    std::vector<uint8_t> d(30 + name.size());
    diskKey(d, 0, childOid, dtype, 0);   // location
    w64(d, 17, 7);
    w16(d, 27, static_cast<uint16_t>(name.size()));
    d[29] = dtype == 132 ? 2 : 1;        // FT_DIR : FT_REG_FILE — keep simple
    std::memcpy(d.data() + 30, name.data(), name.size());
    return d;
}

// Single-stripe chunk {key + btrfs_chunk}: logical=keyOff, phys=stripeOff.
std::vector<uint8_t> chunkRecord(uint64_t logical, uint64_t length, uint64_t phys) {
    // key(17) + btrfs_chunk(48 + num_stripes*32); first stripe inline at +48.
    std::vector<uint8_t> d(17 + 48 + 32, 0);
    diskKey(d, 0, 256, 228, logical);      // FIRST_CHUNK_TREE_OID, CHUNK_ITEM
    w64(d, 17 + 0, length);
    w64(d, 17 + 8, 3);                     // owner = chunk tree
    w64(d, 17 + 16, 65536);                // stripe_len
    w64(d, 17 + 24, 1);                    // type SINGLE
    w16(d, 17 + 44, 1);                    // num_stripes
    w64(d, 17 + 48, 1);                    // devid
    w64(d, 17 + 56, phys);                 // stripe.offset
    return d;
}

// Build a single-device btrfs volume.
// Chunk map: logical [0x100000,+8MiB) -> physical [0x200000,+8MiB)
// Root tree @logical 0x100000, FS tree @logical 0x101000.
struct Vol {
    std::vector<uint8_t> img;
    static constexpr uint64_t kMapLen = 8ull << 20;
    static constexpr uint64_t kPhysBase = 0x200000;
    static constexpr uint64_t kLogBase = 0x100000;
};

Vol makeVolume() {
    Vol v;
    v.img.resize(kSb + 4096 + Vol::kPhysBase + Vol::kMapLen, 0);

    uint8_t fsid[16];
    for (int i = 0; i < 16; ++i) fsid[i] = static_cast<uint8_t>(0xA0 + i);

    // --- superblock @64KiB ---
    size_t s = kSb;
    std::memcpy(v.img.data() + s + 0x20, fsid, 16);
    w64(v.img, s + 0x30, kSb);               // bytenr
    w64(v.img, s + 0x40, 0x4D5F53665248425Full); // "_BHRfS_M"
    w64(v.img, s + 0x48, 10);                // generation
    w64(v.img, s + 0x50, Vol::kLogBase);     // root (root tree)
    w64(v.img, s + 0x58, 0);                 // chunk_root (sys array suffices)
    w64(v.img, s + 0x70, Vol::kMapLen * 2);  // total_bytes
    w32(v.img, s + 0x90, 512);               // sectorsize
    w32(v.img, s + 0x94, kNode);             // nodesize
    auto cr = chunkRecord(Vol::kLogBase, Vol::kMapLen, Vol::kPhysBase);
    w32(v.img, s + 0xA0, static_cast<uint32_t>(cr.size())); // sys_chunk_array_size
    std::memcpy(v.img.data() + s + 0x32B, cr.data(), cr.size());

    // Backup root[0]: older-generation fs_root at logical +0x2000, gen 7.
    w64(v.img, s + 0xB2B + 48, Vol::kLogBase + 0x2000); // fs_root
    w64(v.img, s + 0xB2B + 56, 7);                      // fs_root_gen (staleGen)

    auto at = [&](uint64_t logical) { return Vol::kPhysBase + (logical - Vol::kLogBase); };

    // --- root tree leaf @logical 0x100000: ROOT_ITEM oid 5 -> 0x101000 ---
    leaf(v.img, at(Vol::kLogBase), Vol::kLogBase, 10, 1,
         {{5, 132, 0, rootItem(Vol::kLogBase + 0x1000, 0)}}, fsid);

    // --- fs tree leaf @0x101000: live files ---
    leaf(v.img, at(Vol::kLogBase + 0x1000), Vol::kLogBase + 0x1000, 10, 5,
         {
             // dir ino 256 "docs" under root (6)
             {256, 1, 0, inodeItem(0, 0x4000 | 0755, 1700000100, 1700000100)},
             {256, 12, 6, inodeRef("docs")},
             {6, 84, 0, dirItem(256, "docs")},
             // file ino 257 "report.pdf" under docs(256), extent at
             // logical 0x300000 -> physical via chunk map, 8192 bytes
             {257, 1, 0, inodeItem(8192, 0x8000 | 0644, 1700000200, 1700000050)},
             {257, 12, 256, inodeRef("report.pdf")},
             {256, 84, 0, dirItem(257, "report.pdf")},
             {257, 108, 0, extentData(Vol::kLogBase + 0x200000, 0, 8192)},
         }, fsid);

    // --- stale fs tree @0x102000 (gen 7): file deleted since then ---
    leaf(v.img, at(Vol::kLogBase + 0x2000), Vol::kLogBase + 0x2000, 7, 5,
         {
             {256, 1, 0, inodeItem(0, 0x4000 | 0755, 1700000100, 1700000100)},
             {256, 12, 6, inodeRef("docs")},
             {257, 1, 0, inodeItem(8192, 0x8000 | 0644, 1700000200, 1700000050)},
             {257, 12, 256, inodeRef("report.pdf")},
             // deleted file ino 300 "gone.txt" — absent from live tree
             {300, 1, 0, inodeItem(512, 0x8000 | 0644, 1700000000, 1700000000)},
             {300, 12, 256, inodeRef("gone.txt")},
             {300, 108, 0, extentData(Vol::kLogBase + 0x300000, 0, 512)},
         }, fsid);

    // Payload so extents are real bytes.
    std::memset(v.img.data() + at(Vol::kLogBase + 0x200000), 0x55, 8192);
    std::memset(v.img.data() + at(Vol::kLogBase + 0x300000), 0x66, 512);
    return v;
}

std::vector<FileRecord> runScan(std::vector<uint8_t> img) {
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<FileRecord> out;
    BtrfsParser p;
    EXPECT_TRUE(p.scanAt(reader, [&](const FileRecord& fr) { out.push_back(fr); }, nullptr, 0));
    return out;
}

const FileRecord* findByName(const std::vector<FileRecord>& v, const std::string& n) {
    for (const auto& r : v) if (r.name == n || r.name.size() > n.size() && r.name.compare(r.name.size() - n.size(), n.size(), n) == 0) return &r;
    return nullptr;
}

} // namespace

TEST(BtrfsParser, VolumeRecordEmitted) {
    auto recs = runScan(makeVolume().img);
    const FileRecord* vol = findByName(recs, "Btrfs_Volume");
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->source, "btrfs_volume");
    EXPECT_GT(vol->sizeBytes, 0u);
}

TEST(BtrfsParser, LiveFileRealPathAndRuns) {
    auto recs = runScan(makeVolume().img);
    const FileRecord* f = findByName(recs, "report.pdf");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->status, 1);
    EXPECT_EQ(f->path, "/docs/report.pdf");
    EXPECT_EQ(f->sizeBytes, 8192u);
    EXPECT_EQ(f->modifiedAt, 1700000200);
    EXPECT_EQ(f->createdAt, 1700000050);
    ASSERT_EQ(f->runs.size(), 1u);
    // logical 0x300000 -> phys 0x400000 (map base delta), in sectors /512
    EXPECT_EQ(f->runs[0].startSector, (Vol::kPhysBase + 0x200000) / 512);
    EXPECT_EQ(f->runs[0].sectorCount, 16u);
}

TEST(BtrfsParser, DirectoryEmittedAsDirectory) {
    auto recs = runScan(makeVolume().img);
    const FileRecord* d = findByName(recs, "docs");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->category, "Directory");
    EXPECT_EQ(d->status, 1);
}

TEST(BtrfsParser, StaleRootYieldsDeletedInode) {
    auto recs = runScan(makeVolume().img);
    const FileRecord* g = findByName(recs, "gone.txt");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->status, 0);
    EXPECT_EQ(g->source, "btrfs_stale_root");
    EXPECT_EQ(g->path, "/docs/gone.txt");
    EXPECT_LT(g->confidence, 70);
    ASSERT_EQ(g->runs.size(), 1u);
}

TEST(BtrfsParser, MagicAbsentReturnsFalse) {
    auto img = makeVolume().img;
    std::memset(img.data() + kSb + 0x40, 0, 8);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    BtrfsParser p;
    std::vector<FileRecord> recs;
    EXPECT_FALSE(p.scanAt(reader, [&](const FileRecord& fr) { recs.push_back(fr); }, nullptr, 0));
}
