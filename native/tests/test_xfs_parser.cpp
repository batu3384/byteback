// XFS parser tests. Synthetic in-memory images only (ponytail: no mkfs.xfs on
// Windows) — fixtures are written byte-by-byte citing the kernel struct member
// at each offset (linux/fs/xfs/libxfs/xfs_format.h, xfs_da_format.h). Several
// assertions use hand-computed hex literals so an offset slip fails loudly.
#include "byteback_io.h"
#include "fs/xfs_parser.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace byteback;

namespace {

// ---- big-endian byte writers (explicit, no host-endian reinterpret casts) ----
// They grow the target buffer on demand so fork builders can append at will.

void Wb16(std::vector<uint8_t>& v, size_t o, uint16_t x) {
    if (v.size() < o + 2) v.resize(o + 2);
    v[o] = static_cast<uint8_t>(x >> 8);
    v[o + 1] = static_cast<uint8_t>(x & 0xFF);
}
void Wb32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    if (v.size() < o + 4) v.resize(o + 4);
    for (int i = 0; i < 4; ++i) v[o + i] = static_cast<uint8_t>((x >> (24 - 8 * i)) & 0xFF);
}
void Wb64(std::vector<uint8_t>& v, size_t o, uint64_t x) {
    if (v.size() < o + 8) v.resize(o + 8);
    for (int i = 0; i < 8; ++i) v[o + i] = static_cast<uint8_t>((x >> (56 - 8 * i)) & 0xFF);
}

// ---- fixture spec ------------------------------------------------------------
//
// 2 AGs x 64 blocks x 512 bytes = 65536-byte image; 256-byte inodes (2 per
// block). Inode N lives at:
//   agOffset(N >> 6) + ((agino >> 1) << 9) + ((agino & 1) << 8)   [agino = N & 63]
// e.g. ino 0x45 (AG1, agino 5) -> 0x8000 + 0x400 + 0x100 = 0x8500.
struct Spec {
    uint32_t blocksize = 512;
    uint8_t blocklog = 9;
    uint16_t inodesize = 256;
    uint8_t inodelog = 8;
    uint8_t inopblog = 1;
    uint32_t agblocks = 64;
    uint8_t agblklog = 6;
    uint32_t agcount = 2;
    uint64_t rootino = 0x45;
    bool v3 = false;
    uint32_t agsInImage = 2;
};

uint64_t AgBytes(const Spec& s, uint64_t ag) { return ag * s.agblocks * s.blocksize; }

uint64_t InoOff(const Spec& s, uint64_t ino) {
    const uint64_t agno = ino >> s.agblklog;
    const uint64_t agino = ino & ((1ull << s.agblklog) - 1);
    return AgBytes(s, agno) + ((agino >> s.inopblog) << s.blocklog) +
           ((agino & ((1u << s.inopblog) - 1)) << s.inodelog);
}

size_t ForkOff(const Spec& s) { return s.v3 ? 176 : 100; } // sizeof(xfs_dinode) vs offsetof(di_crc)

std::vector<uint8_t> BuildImage(const Spec& s) {
    std::vector<uint8_t> img(static_cast<size_t>(AgBytes(s, s.agsInImage)), 0);
    // struct xfs_dsb (BE):
    Wb32(img, 0, 0x58465342);                                    // sb_magicnum 'XFSB'
    Wb32(img, 4, s.blocksize);                                   // sb_blocksize
    Wb64(img, 8, static_cast<uint64_t>(s.agblocks) * s.agcount); // sb_dblocks
    Wb64(img, 56, s.rootino);                                    // sb_rootino
    Wb32(img, 84, s.agblocks);                                   // sb_agblocks
    Wb32(img, 88, s.agcount);                                    // sb_agcount
    Wb16(img, 100, s.v3 ? 0xA005 : 0x2004);                      // sb_versionnum
    Wb16(img, 102, 512);                                         // sb_sectsize
    Wb16(img, 104, s.inodesize);                                 // sb_inodesize
    Wb16(img, 106, static_cast<uint16_t>(s.blocksize / s.inodesize)); // sb_inopblock
    img[120] = s.blocklog;                                       // sb_blocklog
    img[121] = 9;                                                // sb_sectlog
    img[122] = s.inodelog;                                       // sb_inodelog
    img[123] = s.inopblog;                                       // sb_inopblog
    img[124] = s.agblklog;                                       // sb_agblklog
    img[192] = 0;                                                // sb_dirblklog
    if (s.agsInImage > 1) {
        std::memcpy(img.data() + AgBytes(s, 1), img.data(), 512); // per-AG secondary sb
    }
    return img;
}

// struct xfs_dinode: di_magic BE16@0 'IN', di_mode BE16@2, di_version u8@4,
// di_format u8@5, di_nlink BE32@16, di_size BE64@56, di_nextents BE32@76.
// Data fork (dir/file contents) starts at 100 (v2) / 176 (v3).
void WriteInode(std::vector<uint8_t>& img, const Spec& s, uint64_t ino, uint16_t mode,
                uint8_t format, uint64_t size, const std::vector<uint8_t>& fork) {
    const size_t o = static_cast<size_t>(InoOff(s, ino));
    Wb16(img, o, 0x494E);
    Wb16(img, o + 2, mode);
    img[o + 4] = s.v3 ? 3 : 2;
    img[o + 5] = format;
    Wb32(img, o + 16, 1);
    Wb64(img, o + 56, size);
    Wb32(img, o + 76, static_cast<uint32_t>(format == 2 ? fork.size() / 16 : 0)); // di_nextents
    if (!fork.empty()) {
        std::memcpy(img.data() + o + ForkOff(s), fork.data(), fork.size());
    }
}

// 16-byte struct xfs_bmbt_rec: l0:63 flag, l0:9-62 startoff,
// l0:0-8 + l1:21-63 startblock, l1:0-20 blockcount.
void AppendExtRec(std::vector<uint8_t>& fork, uint64_t flag, uint64_t startoff,
                  uint64_t startblock, uint64_t count) {
    const uint64_t l0 = (flag << 63) | (startoff << 9) | (startblock >> 43);
    const uint64_t l1 = ((startblock & ((1ull << 43) - 1)) << 21) | count;
    const size_t o = fork.size(); // capture once: the writers auto-resize
    Wb64(fork, o, l0);
    Wb64(fork, o + 8, l1);
    fork.resize(o + 16);
}

// One-extent data fork helper.
std::vector<uint8_t> OneExtentFork(uint64_t startblock, uint64_t count) {
    std::vector<uint8_t> f;
    AppendExtRec(f, 0, 0, startblock, count);
    return f;
}

// xfs_dir2_sf_hdr {count u8, i8count u8, parent BE64|BE32} then byte-packed
// xfs_dir2_sf_entry {namelen u8, offset BE16, name, [filetype u8 on v5],
// ino BE64|BE32}.
std::vector<uint8_t> SfFork(uint64_t parentIno,
                            const std::vector<std::pair<std::string, uint64_t>>& entries,
                            bool v3) {
    std::vector<uint8_t> f;
    f.push_back(static_cast<uint8_t>(entries.size()));   // count
    f.push_back(0);                                      // i8count (all inos < 4 GiB here)
    Wb32(f, f.size(), static_cast<uint32_t>(parentIno)); // parent (6-byte header)
    for (const auto& e : entries) {
        f.push_back(static_cast<uint8_t>(e.first.size())); // namelen
        Wb16(f, f.size(), 0x30);                           // saved offset (unused by parser)
        f.insert(f.end(), e.first.begin(), e.first.end());
        if (v3) f.push_back(1); // filetype XFS_DIR3_FT_REG_FILE (parser ignores it)
        Wb32(f, f.size(), static_cast<uint32_t>(e.second));
    }
    return f;
}

// Shortform dir inode: di_size of a LOCAL-format dir is the used fork length
// (xfs_dir2_sf_hdr + entries), which the walker trusts as the entry area end.
void WriteSfDir(std::vector<uint8_t>& img, const Spec& s, uint64_t ino, uint64_t parent,
                const std::vector<std::pair<std::string, uint64_t>>& entries, bool v3) {
    std::vector<uint8_t> f = SfFork(parent, entries, v3);
    WriteInode(img, s, ino, 0x41ED /*S_IFDIR|0755*/, 1 /*FMT_LOCAL*/, f.size(), f);
}

// One xfs_dir2_data_entry at `off`: inumber BE64@0, namelen u8@8, name@9,
// tag BE16 in the last 2 bytes of the 8-byte-aligned record.
void WriteDataEntry(std::vector<uint8_t>& img, size_t off, uint64_t ino, const char* name) {
    const size_t nlen = std::strlen(name);
    Wb64(img, off, ino);
    img[off + 8] = static_cast<uint8_t>(nlen);
    std::memcpy(img.data() + off + 9, name, nlen);
    const size_t entsize = ((9 + nlen + 2u) + 7u) & ~size_t(7u);
    Wb16(img, off + entsize - 2, static_cast<uint16_t>(off)); // tag = own offset
}

struct Hit {
    std::string path;
    uint64_t ino = 0;
    uint64_t size = 0;
    bool isDir = false;
    std::vector<std::pair<uint64_t, uint64_t>> runs;
};

std::vector<Hit> Walk(XfsParser* p) {
    std::vector<Hit> hits;
    p->walkTree([&](const std::string& path, uint64_t ino, uint64_t size, bool isDir,
                    const std::vector<std::pair<uint64_t, uint64_t>>& runs) {
        hits.push_back({path, ino, size, isDir, runs});
    });
    return hits;
}

const Hit* FindHit(const std::vector<Hit>& hits, const std::string& path) {
    for (const auto& h : hits) {
        if (h.path == path) return &h;
    }
    return nullptr;
}

int CountHit(const std::vector<Hit>& hits, const std::string& path) {
    int n = 0;
    for (const auto& h : hits) {
        if (h.path == path) ++n;
    }
    return n;
}

} // namespace

// ---- superblock ---------------------------------------------------------------

TEST(XfsParser, SuperblockAcceptAndFields) {
    for (bool v3 : {false, true}) {
        Spec s;
        s.v3 = v3;
        DiskReader reader;
        reader.attachMemoryVolume(BuildImage(s), 512);
        XfsParser p;
        ASSERT_TRUE(p.open(reader, 0)) << "v3=" << v3;
        const XfsSuperblock& sb = p.superblock();
        EXPECT_EQ(sb.blocksize, 512u);
        EXPECT_EQ(sb.dblocks, 128u);
        EXPECT_EQ(sb.rootino, 0x45u);
        EXPECT_EQ(sb.agblocks, 64u);
        EXPECT_EQ(sb.agcount, 2u);
        EXPECT_EQ(sb.versionnum, v3 ? 0xA005 : 0x2004);
        EXPECT_EQ(sb.inodesize, 256u);
        EXPECT_EQ(sb.blocklog, 9u);
        EXPECT_EQ(sb.inodelog, 8u);
        EXPECT_EQ(sb.inopblog, 1u);
        EXPECT_EQ(sb.agblklog, 6u);
        EXPECT_EQ(sb.hasV3Inodes(), v3);
    }
}

TEST(XfsParser, SuperblockRejectsBadMagic) {
    auto img = BuildImage(Spec{});
    img[0] = 'X'; // "XFSA": sb_magicnum != XFSB (also covers all-zero garbage)
    img[3] = 'A';
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    EXPECT_FALSE(p.open(reader, 0));
}

TEST(XfsParser, SuperblockRejectsBadBlocklog) {
    auto img = BuildImage(Spec{});
    img[120] = 7; // sb_blocklog != log2(sb_blocksize=512)=9
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    EXPECT_FALSE(p.open(reader, 0));
}

TEST(XfsParser, SuperblockRejectsNonPow2Blocksize) {
    auto img = BuildImage(Spec{});
    Wb32(img, 4, 1000); // sb_blocksize not a power of two
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    EXPECT_FALSE(p.open(reader, 0));
}

TEST(XfsParser, SuperblockRejectsSecondaryMismatch) {
    auto img = BuildImage(Spec{});
    std::memset(img.data() + static_cast<size_t>(AgBytes(Spec{}, 1)), 0, 512); // AG 1 sb wiped
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    EXPECT_FALSE(p.open(reader, 0));
}

TEST(XfsParser, UnopenedParserFails) {
    XfsParser p;
    std::vector<uint8_t> raw;
    EXPECT_FALSE(p.readInode(0, raw));
    EXPECT_FALSE(p.walkTree([](const std::string&, uint64_t, uint64_t, bool,
                              const std::vector<std::pair<uint64_t, uint64_t>>&) {}));
}

// ---- inode location (golden offsets) ------------------------------------------

TEST(XfsParser, ReadInodeV2GoldenOffset) {
    // ino 0x45 = AG1, agino 5 -> byte 0x8000 + (5>>1)<<9 + (5&1)<<8 = 0x8500.
    auto img = BuildImage(Spec{});
    const size_t off = static_cast<size_t>(InoOff(Spec{}, 0x45));
    ASSERT_EQ(off, 0x8500u); // hand-computed hex literal
    Wb16(img, off, 0x494E);  // di_magic 'IN' — placed ONLY at the golden offset
    Wb64(img, off + 56, 0x1122334455667788ull); // di_size sentinel

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    std::vector<uint8_t> raw;
    ASSERT_TRUE(p.readInode(0x45, raw));
    EXPECT_EQ(raw.size(), 256u);
    EXPECT_EQ(raw[0], 0x49u);
    EXPECT_EQ(raw[1], 0x4Eu);
    uint64_t sz = 0; // di_size BE64 @56 read back through the parser's IO path
    for (int i = 0; i < 8; ++i) sz = (sz << 8) | raw[56 + i];
    EXPECT_EQ(sz, 0x1122334455667788ull);
    // ino 0x44 maps to 0x8400, which has no 'IN' magic -> honest failure.
    std::vector<uint8_t> raw2;
    EXPECT_FALSE(p.readInode(0x44, raw2));
    // Out-of-range AG -> rejected before IO.
    EXPECT_FALSE(p.readInode(0x45 + (1ull << 6) * 2, raw2));
}

TEST(XfsParser, ReadInodeV3GoldenOffset) {
    Spec s;
    s.v3 = true;
    s.rootino = 0x44; // AG1, agino 4 -> 0x8000 + (4>>1)<<9 + 0 = 0x8400
    auto img = BuildImage(s);
    const size_t off = static_cast<size_t>(InoOff(s, 0x44));
    ASSERT_EQ(off, 0x8400u); // hand-computed hex literal
    Wb16(img, off, 0x494E);
    img[off + 4] = 3; // di_version = 3

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    std::vector<uint8_t> raw;
    ASSERT_TRUE(p.readInode(0x44, raw));
    EXPECT_EQ(raw[4], 3u);
}

// ---- extent decode (hex-literal record, unwritten flag, multi-extent) ---------

TEST(XfsParser, ExtentDecodeFlagAndMultiExtent) {
    auto img = BuildImage(Spec{});
    // File at ino 7 (AG0, agino 7 -> byte 0x700), two extent records in the fork:
    // rec0 hand-computed bytes: l0 = 0x8000000000000E00, l1 = 0x2468A00003
    //   [flag:1][startoff:54][startblock:52][count:21]
    //   flag=1 (unwritten), startoff=(l0>>9)=7, startblock=((l0&0x1FF)<<43)|(l1>>21)
    //   = (0x2468A00003>>21) = 0x12345 (l0 low 9 bits are startblock's TOP 9 bits
    //   = 0 here), count = l1 & 0x1FFFFF = 3
    //   -> run (0x12345*512, 3*512) = (0x2468A00, 0x600)
    // rec1: flag=0, startoff=10, startblock=100, count=1 -> (51200, 512)
    std::vector<uint8_t> fork;
    Wb64(fork, 0, 0x8000000000000E00ull);
    Wb64(fork, 8, 0x2468A00003ull);
    fork.resize(16);
    AppendExtRec(fork, 0, 10, 100, 1);
    WriteInode(img, Spec{}, 7, 0x81A4 /*S_IFREG|0644*/, 2 /*FMT_EXTENTS*/, 0xCAFEBABE, fork);
    // Root dir (ino 0x45) shortform -> "note" -> 7.
    WriteSfDir(img, Spec{}, 0x45, 0x45, {{"note", 7}}, false);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    const Hit* note = FindHit(hits, "/note");
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isDir);
    EXPECT_EQ(note->size, 0xCAFEBABEull);
    ASSERT_EQ(note->runs.size(), 2u);
    EXPECT_EQ(note->runs[0].first, 0x2468A00ull); // hand-computed hex literal
    EXPECT_EQ(note->runs[0].second, 0x600ull);
    EXPECT_EQ(note->runs[1].first, 51200u);
    EXPECT_EQ(note->runs[1].second, 512u);
    const Hit* root = FindHit(hits, "/");
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->isDir);
    EXPECT_EQ(root->ino, 0x45u);
}

// ---- shortform dir walk --------------------------------------------------------

TEST(XfsParser, ShortformDirWalk) {
    auto img = BuildImage(Spec{});
    WriteInode(img, Spec{}, 7, 0x81A4, 2, 4096, OneExtentFork(60, 1)); // run (30720, 512)
    WriteInode(img, Spec{}, 8, 0x81A4, 2, 4096, OneExtentFork(61, 1));
    WriteSfDir(img, Spec{}, 0x45, 0x45, {{"alfa", 7}, {"beta.txt", 8}}, false);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    const Hit* a = FindHit(hits, "/alfa");
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->isDir);
    EXPECT_EQ(a->ino, 7u);
    ASSERT_EQ(a->runs.size(), 1u);
    EXPECT_EQ(a->runs[0].first, 30720u); // 60*512
    EXPECT_EQ(a->runs[0].second, 512u);
    const Hit* b = FindHit(hits, "/beta.txt");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->ino, 8u);
}

// ---- block dir walk ('XD2B', entries + unused region + leaf-table tail) --------

TEST(XfsParser, BlockDirWalk) {
    Spec s;
    auto img = BuildImage(s);
    // Root dir -> extent (startoff 0, startblock 20, count 1): dir block @0x2800.
    {
        std::vector<uint8_t> f;
        AppendExtRec(f, 0, 0, 20, 1);
        WriteInode(img, s, 0x45, 0x41ED, 2, 512, f);
    }
    const size_t blk = static_cast<size_t>(20 * 512);
    // xfs_dir2_data_hdr: magic 'XD2B' BE32@0, bestfree[3] @4..16.
    Wb32(img, blk, 0x58443242);
    WriteDataEntry(img, blk + 16, 0x45, ".");
    WriteDataEntry(img, blk + 32, 0x45, "..");
    WriteDataEntry(img, blk + 48, 7, "doc");
    // xfs_dir2_data_unused: freetag BE16@64=0xffff, length BE16@66, tag at its end.
    Wb16(img, blk + 64, 0xFFFF);
    Wb16(img, blk + 66, 496 - 64); // free region ends where the leaf table begins
    Wb16(img, blk + 494, 64);
    // xfs_dir2_block_tail {count BE32, stale BE32} in the last 8 bytes.
    Wb32(img, blk + 504, 1);
    Wb32(img, blk + 508, 0);
    // "doc" -> ino 7 regular file at fsb 50.
    WriteInode(img, s, 7, 0x81A4, 2, 1, OneExtentFork(50, 1));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    const Hit* doc = FindHit(hits, "/doc");
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->isDir);
    EXPECT_EQ(doc->ino, 7u);
    ASSERT_EQ(doc->runs.size(), 1u);
    EXPECT_EQ(doc->runs[0].first, 25600u); // 50*512
    EXPECT_EQ(doc->runs[0].second, 512u);
    EXPECT_EQ(CountHit(hits, "/."), 0); // "." and ".." never emitted
    EXPECT_EQ(CountHit(hits, "/.."), 0);
    EXPECT_NE(FindHit(hits, "/"), nullptr);
}

// ---- leaf dir walk (XD2D data block + skipped index leaf block) -----------------

TEST(XfsParser, LeafDirWalkSkipsIndexBlocks) {
    Spec s;
    auto img = BuildImage(s);
    // Root dir extents: (startoff 0, startblock 20) data block, (1, 21) leaf1 index.
    {
        std::vector<uint8_t> f;
        AppendExtRec(f, 0, 0, 20, 1);
        AppendExtRec(f, 1, 1, 21, 1);
        WriteInode(img, s, 0x45, 0x41ED, 2, 1024, f);
    }
    const size_t data = static_cast<size_t>(20 * 512);
    Wb32(img, data, 0x58443244); // 'XD2D' xfs_dir2_data_hdr.magic
    WriteDataEntry(img, data + 16, 8, "one");
    WriteDataEntry(img, data + 32, 9, "two");
    Wb16(img, data + 48, 0xFFFF);
    Wb16(img, data + 50, 512 - 48);
    Wb16(img, data + 510, 48);
    // Leaf1 block: xfs_da_blkinfo {forw BE32@0, back BE32@4, magic BE16@8=0xd2f1},
    // count BE16@12, stale BE16@14, then xfs_dir2_leaf_entry {hash BE32, addr BE32}.
    // Holds NO entries itself — the walker must skip it (else dupes/garbage).
    const size_t leaf = static_cast<size_t>(21 * 512);
    Wb16(img, leaf + 8, 0xd2f1);
    Wb16(img, leaf + 12, 2);
    Wb32(img, leaf + 16, 0x1234);
    Wb32(img, leaf + 20, 8);
    Wb32(img, leaf + 24, 0x5678);
    Wb32(img, leaf + 28, 16);
    WriteInode(img, s, 8, 0x81A4, 2, 1, OneExtentFork(60, 1));
    WriteInode(img, s, 9, 0x81A4, 2, 1, OneExtentFork(61, 1));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    EXPECT_EQ(CountHit(hits, "/one"), 1);
    EXPECT_EQ(CountHit(hits, "/two"), 1);
    const Hit* one = FindHit(hits, "/one");
    const Hit* two = FindHit(hits, "/two");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    EXPECT_EQ(one->ino, 8u);
    EXPECT_EQ(two->ino, 9u);
}

// ---- B+tree data fork (in-inode internal root -> on-disk leaf) ------------------

namespace {

// Assemble a btree-format file inode: in-inode root level 1 with one pointer to
// an on-disk leaf block at fsb `leafFsb` holding one extent record.
void WriteBtreeFile(std::vector<uint8_t>& img, const Spec& s, uint64_t ino,
                    uint64_t leafFsb, uint32_t leafMagic) {
    const size_t inoOff = static_cast<size_t>(InoOff(s, ino));
    const size_t fo = inoOff + ForkOff(s);
    const size_t forkLen = s.inodesize - ForkOff(s);           // XFS_LITINO
    const size_t maxrecs = (forkLen - 4) / 16;                 // xfs_bmdr_maxrecs, internal
    Wb16(img, fo, 1);                                          // xfs_bmdr_block.bb_level
    Wb16(img, fo + 2, 1);                                      // .bb_numrecs
    Wb64(img, fo + 4, 0);                                      // xfs_bmbt_key.br_startoff
    Wb64(img, fo + 4 + maxrecs * 8, leafFsb);                  // xfs_bmdr_ptr_addr(0)
    img[inoOff + 4] = s.v3 ? 3 : 2;                            // di_version
    img[inoOff + 5] = 3;                                       // di_format = BTREE
    Wb16(img, inoOff, 0x494E);                                 // di_magic
    Wb16(img, inoOff + 2, 0x81A4);                             // di_mode S_IFREG|0644
    Wb32(img, inoOff + 16, 1);                                 // di_nlink
    Wb64(img, inoOff + 56, 2048);                              // di_size
    // On-disk leaf: 'BMAP' 24-byte (v4) or 'BMA3' 72-byte (v5, CRC) header,
    // then one xfs_bmbt_rec (startoff 0, startblock 40, count 2).
    const size_t lb = static_cast<size_t>(leafFsb * s.blocksize);
    Wb32(img, lb, leafMagic);
    Wb16(img, lb + 4, 0); // bb_level
    Wb16(img, lb + 6, 1); // bb_numrecs
    Wb64(img, lb + 8, 0xFFFFFFFFFFFFFFFFull);  // bb_leftsib NULLFSBLOCK
    Wb64(img, lb + 16, 0xFFFFFFFFFFFFFFFFull); // bb_rightsib NULLFSBLOCK
    std::vector<uint8_t> rec;
    AppendExtRec(rec, 0, 0, 40, 2);
    const size_t recOff = (leafMagic == 0x424D4133) ? 72 : 24; // LBLOCK_CRC_LEN vs LBLOCK_LEN
    std::memcpy(img.data() + lb + recOff, rec.data(), 16);
}

} // namespace

TEST(XfsParser, BtreeFileWalkV2) {
    auto img = BuildImage(Spec{});
    WriteBtreeFile(img, Spec{}, 7, 30, 0x424D4150 /*'BMAP'*/);
    WriteSfDir(img, Spec{}, 0x45, 0x45, {{"big", 7}}, false);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    const Hit* big = FindHit(hits, "/big");
    ASSERT_NE(big, nullptr);
    EXPECT_FALSE(big->isDir);
    EXPECT_EQ(big->size, 2048u);
    ASSERT_EQ(big->runs.size(), 1u);
    EXPECT_EQ(big->runs[0].first, 20480u); // 40*512
    EXPECT_EQ(big->runs[0].second, 1024u); // 2*512
}

TEST(XfsParser, BtreeFileWalkV3) {
    // v5 filesystem: fork @176, maxrecs = (256-176-4)/16 = 4 -> ptr0 @ fork+36;
    // 'BMA3' leaf with the 72-byte CRC header. Proves all three v3 offsets at once.
    Spec s;
    s.v3 = true;
    s.rootino = 0x44;
    auto img = BuildImage(s);
    WriteBtreeFile(img, s, 7, 30, 0x424D4133 /*'BMA3'*/);
    WriteSfDir(img, s, 0x44, 0x44, {{"big", 7}}, true); // dir3 sf entry (filetype byte)

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    const Hit* big = FindHit(hits, "/big");
    ASSERT_NE(big, nullptr) << "v3 fork offset (176) or 'BMA3' header (72) slipped";
    ASSERT_EQ(big->runs.size(), 1u);
    EXPECT_EQ(big->runs[0].first, 20480u);
    EXPECT_EQ(big->runs[0].second, 1024u);
}

// ---- cycle guard ---------------------------------------------------------------

TEST(XfsParser, CycleGuardTerminates) {
    auto img = BuildImage(Spec{});
    // Root 0x45 -> "b" -> 0x46; 0x46 -> "a" -> 0x45 (back edge) and "s" -> itself.
    WriteSfDir(img, Spec{}, 0x45, 0x45, {{"b", 0x46}}, false);
    WriteSfDir(img, Spec{}, 0x46, 0x45, {{"a", 0x45}, {"s", 0x46}}, false);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    EXPECT_TRUE(p.walkTree([](const std::string&, uint64_t, uint64_t, bool,
                              const std::vector<std::pair<uint64_t, uint64_t>>&) {}));
    ASSERT_EQ(hits.size(), 2u); // "/" and "/b" — visited set breaks the cycle
    EXPECT_EQ(hits[0].path, "/");
    EXPECT_EQ(hits[1].path, "/b");
}

// ---- depth cap ------------------------------------------------------------------

TEST(XfsParser, DepthCapBounded) {
    Spec s;
    s.rootino = 2;      // chain of 70 dirs: ino 2 -> "d" -> 3 -> ... -> 71
    s.agblocks = 128;   // bigger AG so the whole chain stays inside AG 0 —
    s.agblklog = 7;     // inodes must never sit at agino 0 (the AG sb slot)
    auto img = BuildImage(s);
    for (uint64_t ino = 2; ino < 71; ++ino) {
        WriteSfDir(img, s, ino, ino, {{"d", ino + 1}}, false);
    }
    WriteSfDir(img, s, 71, 71, {}, false);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);
    XfsParser p;
    ASSERT_TRUE(p.open(reader, 0));
    const auto hits = Walk(&p);
    // Depth cap 64: root (depth 0) through depth 64 -> exactly 65 dirs visited;
    // the chain is longer, so the cap (not the data) ended the walk.
    EXPECT_EQ(hits.size(), 65u);
    for (const auto& h : hits) EXPECT_TRUE(h.isDir);
    ASSERT_NE(FindHit(hits, "/d"), nullptr);
    EXPECT_EQ(FindHit(hits, "/d")->ino, 3u); // first child
}
