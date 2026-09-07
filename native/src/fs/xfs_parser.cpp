// Read-only XFS filesystem parser.
//
// Every on-disk structure is big-endian unless noted. Offsets below cite the
// kernel headers that define them (linux/fs/xfs/libxfs/):
//   xfs_format.h     - struct xfs_dsb (superblock), struct xfs_dinode,
//                      struct xfs_btree_block (+XFS_BTREE_LBLOCK*_LEN),
//                      XFS_BMAP_MAGIC/'BMA3', struct xfs_bmdr_block
//   xfs_da_format.h  - dir2/dir3 block headers, xfs_dir2_data_entry,
//                      xfs_dir2_sf_hdr/sf_entry, block magics
//   xfs_bmap_btree.h - xfs_bmbt_rec bit layout, bmdr key/ptr addressing
#include "fs/xfs_parser.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace byteback {
namespace {

// ---- big-endian field readers ----------------------------------------------

inline uint16_t Be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t Be64(const uint8_t* p) {
    return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4);
}
inline bool IsPow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }
inline uint8_t Log2Exact(uint64_t v) { uint8_t l = 0; while ((v >>= 1) != 0) ++l; return l; }
inline uint8_t CeilLog2(uint64_t v) { uint8_t l = 0; --v; while (v != 0) { ++l; v >>= 1; } return l; }
inline size_t Round8(size_t v) { return (v + 7u) & ~static_cast<size_t>(7u); }

// ---- on-disk constants ------------------------------------------------------

// xfs_format.h: #define XFS_SB_MAGIC 0x58465342 /* 'XFSB' */
constexpr uint32_t kSbMagic = 0x58465342;
// xfs_format.h: #define XFS_DINODE_MAGIC 0x494e /* 'IN' */
constexpr uint16_t kInodeMagic = 0x494E;
// xfs_format.h: #define XFS_BMAP_MAGIC 0x424d4150 'BMAP', XFS_BMAP_CRC_MAGIC 0x424d4133 'BMA3'
constexpr uint32_t kBmapMagic = 0x424D4150;
constexpr uint32_t kBmapCrcMagic = 0x424D4133;
// xfs_btree.h: XFS_BTREE_LBLOCK_LEN = 24, XFS_BTREE_LBLOCK_CRC_LEN = 72
// (long form: magic4 level2 numrecs2 leftsib8 rightsib8 [+blkno8 lsn8 uuid16 owner8 crc4 pad4]).
constexpr size_t kBtreeLblockLen = 24;
constexpr size_t kBtreeLblockCrcLen = 72;
// xfs_da_format.h block magics:
constexpr uint32_t kDir2BlockMagic = 0x58443242; // 'XD2B' single-block dirs
constexpr uint32_t kDir2DataMagic = 0x58443244; // 'XD2D' multiblock dir data
constexpr uint32_t kDir3BlockMagic = 0x58444233; // 'XDB3' v5 single-block dirs
constexpr uint32_t kDir3DataMagic = 0x58444433; // 'XDD3' v5 multiblock dir data
// xfs_da_format.h, u16 in xfs_da_blkinfo.magic @8 (leaf/node/free blocks carry no entries).
constexpr uint16_t kDir2Leaf1Magic = 0xd2f1;
constexpr uint16_t kDir2LeafNMagic = 0xd2ff;
constexpr uint16_t kDir3Leaf1Magic = 0x3df1;
constexpr uint16_t kDir3LeafNMagic = 0x3dff;
constexpr uint16_t kDaNodeMagic = 0xfebe;
constexpr uint16_t kDa3NodeMagic = 0x3ebe;
// xfs_da_format.h: #define XFS_DIR2_DATA_FREE_TAG 0xffff (xfs_dir2_data_unused.freetag)
constexpr uint16_t kDirDataFreeTag = 0xFFFF;

// xfs_dinode.di_format values (xfs_format.h enum xfs_dinode_fmt).
constexpr uint8_t kFmtLocal = 1;
constexpr uint8_t kFmtExtents = 2;
constexpr uint8_t kFmtBtree = 3;
// st_mode format nibbles.
constexpr uint16_t kModeDir = 0x4000;
constexpr uint16_t kModeReg = 0x8000;

// struct xfs_dsb field offsets (BE): sb_magicnum@0, sb_blocksize@4, sb_dblocks@8,
// sb_rootino@56, sb_agblocks@84, sb_agcount@88, sb_versionnum@100, sb_inodesize@104,
// sb_blocklog@120, sb_sectlog@121, sb_inodelog@122, sb_inopblog@123, sb_agblklog@124,
// sb_dirblklog@192.
constexpr size_t kSbDirblklogOff = 192;
constexpr uint16_t kSbVersionNumBits = 0x000F;   // XFS_SB_VERSION_NUMBITS
constexpr uint16_t kSbVersionDirV2Bit = 0x2000;  // XFS_SB_VERSION_DIRV2BIT

// struct xfs_dinode offsets (BE): di_magic@0, di_mode@2, di_version@4, di_format@5,
// di_size@56, di_nextents@76, di_forkoff@82. Data fork: offsetof(xfs_dinode, di_crc)=100
// for v1/v2 inodes, sizeof(xfs_dinode)=176 for v3 (XFS_DINODE_SIZE).
constexpr size_t kDinodeSizeV2 = 100;
constexpr size_t kDinodeSizeV3 = 176;

// Recursion safety.
constexpr int kMaxWalkDepth = 64;
constexpr int kBtreeMaxDepth = 32;

// ---- decoded helpers --------------------------------------------------------

// One xfs_bmbt_rec extent.
struct Extent {
    uint64_t startoff = 0;    // logical fsb
    uint64_t startblock = 0;  // absolute fsb (agno<<agblklog | agbno)
    uint64_t count = 0;       // fsbs
    bool unwritten = false;   // preallocated (still a valid data run)
};

struct DirEntryRef {
    std::string name;
    uint64_t ino = 0;
};

// Decoded xfs_dinode core + data fork window.
struct InodeView {
    bool ok = false;
    uint16_t mode = 0;
    uint8_t version = 0;
    uint8_t format = 0;
    uint64_t size = 0;
    uint32_t nextents = 0;
    const uint8_t* fork = nullptr;
    size_t forkLen = 0;
};

InodeView DecodeInode(const std::vector<uint8_t>& raw) {
    InodeView v;
    // xfs_dinode.di_magic 'IN'; the smallest core we touch is the 100-byte v1/v2 one.
    if (raw.size() < kDinodeSizeV2 || Be16(raw.data()) != kInodeMagic) return v;
    v.mode = Be16(raw.data() + 2);      // di_mode
    v.version = raw[4];                 // di_version
    v.format = raw[5];                  // di_format
    if (v.version < 1 || v.version > 3) return v;
    v.size = Be64(raw.data() + 56);     // di_size
    v.nextents = Be32(raw.data() + 76); // di_nextents
    const size_t dinodeSize = (v.version >= 3) ? kDinodeSizeV3 : kDinodeSizeV2;
    if (raw.size() < dinodeSize) return v;
    const size_t litino = raw.size() - dinodeSize;        // XFS_LITINO
    const uint8_t forkoff = raw[82];                      // di_forkoff (attr split, <<3)
    size_t dsize = forkoff ? (static_cast<size_t>(forkoff) << 3) : litino; // XFS_DFORK_DSIZE
    if (dsize > litino) dsize = litino;
    v.fork = raw.data() + dinodeSize;
    v.forkLen = dsize;
    v.ok = true;
    return v;
}

// struct xfs_bmbt_rec (xfs_format.h): l0:63 flag, l0:9-62 startoff,
// l0:0-8 + l1:21-63 startblock, l1:0-20 blockcount.
void DecodeExtRec(const uint8_t* p, Extent* e) {
    const uint64_t l0 = Be64(p);
    const uint64_t l1 = Be64(p + 8);
    e->unwritten = (l0 >> 63) != 0;
    e->startoff = (l0 >> 9) & ((1ull << 54) - 1);
    e->startblock = ((l0 & 0x1FF) << 43) | (l1 >> 21);
    e->count = l1 & 0x1FFFFF;
}

// Deterministic inode location (XFS_INO_TO_AGNO / XFS_INO_TO_AGINO / XFS_AGINO_TO_OFFSET).
bool ReadInodeAt(DiskReader& reader, uint64_t partOffset, const XfsSuperblock& sb,
                 uint64_t ino, std::vector<uint8_t>* out) {
    if (sb.blocksize == 0 || sb.inodesize < 256 || sb.agcount == 0 || sb.agblocks == 0 ||
        sb.agblklog >= 64 || sb.inopblog >= 32 || sb.inodelog >= 32) {
        return false;
    }
    const uint64_t agno = ino >> sb.agblklog;
    if (agno >= sb.agcount) return false;
    const uint64_t agino = ino & (((1ull << sb.agblklog) - 1));
    const uint64_t off = partOffset + agno * (static_cast<uint64_t>(sb.agblocks) * sb.blocksize) +
                         ((agino >> sb.inopblog) << sb.blocklog) +
                         ((agino & ((1u << sb.inopblog) - 1)) << sb.inodelog);
    out->assign(sb.inodesize, 0);
    if (!reader.readBytes(off, sb.inodesize, out->data()).success) return false;
    return Be16(out->data()) == kInodeMagic; // xfs_dinode.di_magic 'IN'
}

// Shared walk state (plain data so free functions can carry it).
struct WalkCtx {
    DiskReader* reader = nullptr;
    uint64_t partOffset = 0;
    XfsSuperblock sb{};
    uint8_t dirblklogForDirs = 0; // dir block size = blocksize << this
    bool dirOk = false;           // dir entry walking enabled (dir v2 + sane dirblklog)
};

// ---- B+tree data fork (format 3) --------------------------------------------

// On-disk bmap btree block (long form): 'BMAP' 24-byte or 'BMA3' 72-byte header
// (XFS_BTREE_LBLOCK[_CRC]_LEN); leaf records are xfs_bmbt_rec, internal keys are
// xfs_bmbt_key (8 bytes, br_startoff) with 8-byte pointers placed after the
// maxrecs-sized key area (xfs_bmbt_ptr_addr: hdr + maxrecs*sizeof(key) + i*8).
void BtreeBlock(const WalkCtx& ctx, uint64_t fsb, std::vector<Extent>* out, int depth) {
    if (depth <= 0 || fsb == 0 || fsb == ~0ull) return; // NULLFSBLOCK / absurd
    std::vector<uint8_t> buf(ctx.sb.blocksize);
    if (!ctx.reader->readBytes(ctx.partOffset + fsb * ctx.sb.blocksize, ctx.sb.blocksize,
                               buf.data()).success) {
        return;
    }
    const uint8_t* b = buf.data();
    size_t hdr;
    switch (Be32(b)) { // xfs_btree_block.bb_magic
        case kBmapMagic: hdr = kBtreeLblockLen; break;
        case kBmapCrcMagic: hdr = kBtreeLblockCrcLen; break;
        default: return; // unknown btree block: honest skip
    }
    if (ctx.sb.blocksize <= hdr) return;
    const size_t space = ctx.sb.blocksize - hdr;
    const uint16_t level = Be16(b + 4);   // xfs_btree_block.bb_level
    const uint16_t numrecs = Be16(b + 6); // .bb_numrecs
    if (level == 0) {
        const size_t n = std::min<size_t>(numrecs, space / 16);
        for (size_t i = 0; i < n; ++i) {
            Extent e;
            DecodeExtRec(b + hdr + 16 * i, &e);
            out->push_back(e);
        }
        return;
    }
    const size_t maxrecs = space / 16; // xfs_bmbt_maxrecs(blocklen, leaf=false)
    if (maxrecs == 0) return;
    const size_t n = std::min<size_t>(numrecs, maxrecs);
    for (size_t i = 0; i < n; ++i) {
        BtreeBlock(ctx, Be64(b + hdr + maxrecs * 8 + 8 * i), out, depth - 1);
    }
}

// In-inode root (struct xfs_bmdr_block): level u16 BE@0, numrecs u16 BE@2; same
// layout on v4 and v5 (no CRC fields in the inode root). Keys then pointers,
// pointers after the maxrecs key area (xfs_bmdr_ptr_addr).
bool BtreeRootFork(const WalkCtx& ctx, const uint8_t* fork, size_t forkLen,
                   std::vector<Extent>* out) {
    if (forkLen < 4) return false;
    const uint16_t level = Be16(fork);
    const uint16_t numrecs = Be16(fork + 2);
    if (level == 0) {
        const size_t n = std::min<size_t>(numrecs, (forkLen - 4) / 16);
        for (size_t i = 0; i < n; ++i) {
            Extent e;
            DecodeExtRec(fork + 4 + 16 * i, &e);
            out->push_back(e);
        }
        return true;
    }
    const size_t maxrecs = (forkLen - 4) / 16; // xfs_bmdr_maxrecs(forklen, leaf=false)
    if (maxrecs == 0) return false;
    const size_t n = std::min<size_t>(numrecs, maxrecs);
    for (size_t i = 0; i < n; ++i) {
        BtreeBlock(ctx, Be64(fork + 4 + maxrecs * 8 + 8 * i), out, kBtreeMaxDepth);
    }
    return true;
}

// Data fork -> extent list. Unknown formats yield an empty list (honest skip).
void ExtentsForInode(const WalkCtx& ctx, const InodeView& v, std::vector<Extent>* out) {
    if (v.format == kFmtExtents) {
        const size_t n = std::min<uint64_t>(v.nextents, v.forkLen / 16);
        for (size_t i = 0; i < n; ++i) {
            Extent e;
            DecodeExtRec(v.fork + 16 * i, &e);
            out->push_back(e);
        }
    } else if (v.format == kFmtBtree) {
        BtreeRootFork(ctx, v.fork, v.forkLen, out);
    }
}

// ---- directory entries ------------------------------------------------------

// Conservative name check: reject control chars, NUL and '/' so a corrupt fork
// can't fabricate paths. Bytes >= 0x80 (UTF-8) are allowed.
bool ValidName(const uint8_t* p, size_t n) {
    if (n == 0 || n > 255) return false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c == '/' || c == 0 || c < 0x20) return false;
    }
    return true;
}

// Shortform dir (xfs_dinode format LOCAL): xfs_dir2_sf_hdr {count u8, i8count u8,
// parent BE64-or-BE32 (10-byte hdr when i8count>0, else 6)}; entries are byte-packed:
// namelen u8, offset BE16, name, [filetype u8 on v5], ino BE64/BE32 after the name
// (xfs_dir2_sf_entsize / xfs_dir2_sf_get_ino).
void CollectSfEntries(const WalkCtx& ctx, const uint8_t* fork, size_t forkLen, uint64_t size,
                      std::vector<DirEntryRef>* out) {
    if (forkLen < 2) return;
    const uint8_t count = fork[0];
    const uint8_t i8count = fork[1];
    const size_t hdrSize = i8count ? 10 : 6; // xfs_dir2_sf_hdr_size
    const bool ftype = ctx.sb.hasV3Inodes(); // filetype byte present on v5 filesystems
    const size_t end = std::min<size_t>(forkLen, size < forkLen ? static_cast<size_t>(size) : forkLen);
    size_t pos = hdrSize;
    for (uint8_t i = 0; i < count; ++i) {
        const size_t inoSize = i8count ? 8u : 4u;
        if (pos + 3 > end) return; // namelen + saved offset
        const uint8_t nlen = fork[pos];
        const size_t inoOff = pos + 3 + nlen + (ftype ? 1u : 0u);
        const size_t entsize = 3 + nlen + (ftype ? 1u : 0u) + inoSize;
        if (nlen == 0 || inoOff + inoSize > end || entsize == 0 || pos + entsize > end) return;
        const uint8_t* name = fork + pos + 3;
        if (!ValidName(name, nlen)) return; // corrupt fork; don't guess past it
        const uint64_t ino = i8count ? (Be64(fork + inoOff) & ((1ull << 56) - 1)) // XFS_MAXINUMBER
                                     : Be32(fork + inoOff);
        std::string n(reinterpret_cast<const char*>(name), nlen);
        if (ino != 0 && n != "." && n != "..") out->push_back({std::move(n), ino});
        pos += entsize;
    }
}

// One dir data/block block: xfs_dir2_data_hdr (16 B: magic BE32 + 3 bestfree pairs)
// or xfs_dir3_data_hdr (64 B), then packed xfs_dir2_data_entry records:
// inumber BE64 @0, namelen u8 @8, name @9, [filetype u8 on dir3], tag BE16 at the
// end of the 8-byte-aligned record (xfs_dir2_data_entsize = round8(9+namelen
// [+1 dir3]+2)). Freed regions start with freetag 0xFFFF + length BE16.
// blockDir (XD2B/XDB3) blocks additionally end with the xfs_dir2_leaf_entry table
// plus xfs_dir2_block_tail {count BE32, stale BE32} in the last 8 bytes.
void ParseDirBlock(const uint8_t* b, uint32_t len, bool blockDir, bool dir3,
                   std::vector<DirEntryRef>* out) {
    uint32_t dataEnd = len;
    if (blockDir) {
        if (len < 8) return;
        const uint64_t nLeaf = static_cast<uint64_t>(Be32(b + len - 8)) + Be32(b + len - 4);
        if (nLeaf > len / 8u) return; // corrupt tail
        const uint64_t end = static_cast<uint64_t>(len) - 8 - 8 * nLeaf;
        if (end < (dir3 ? 64u : 16u)) return;
        dataEnd = static_cast<uint32_t>(end);
    }
    uint32_t pos = dir3 ? 64 : 16;
    while (pos + 9 <= dataEnd) { // smallest record: inumber(8) + namelen(1)
        if (Be16(b + pos) == kDirDataFreeTag) { // xfs_dir2_data_unused
            const uint16_t flen = Be16(b + pos + 2);
            if (flen < 8 || pos + flen > dataEnd) break;
            pos += flen;
            continue;
        }
        const uint64_t ino = Be64(b + pos); // xfs_dir2_data_entry.inumber
        const uint8_t nlen = b[pos + 8];    // .namelen
        const size_t entsize = Round8(9 + nlen + (dir3 ? 1u : 0u) + 2u);
        if (nlen == 0 || pos + 9 + nlen > dataEnd || pos + entsize > dataEnd) break;
        const uint8_t* name = b + pos + 9;
        if (ValidName(name, nlen) && ino != 0) {
            std::string n(reinterpret_cast<const char*>(name), nlen);
            if (n != "." && n != "..") out->push_back({std::move(n), ino});
        }
        pos += static_cast<uint32_t>(entsize);
    }
}

// Walk the directory's extent list block-by-block. Index blocks (leaf 0xd2f1/
// 0xd2ff/0x3df1/0x3dff and node 0xfebe/0x3ebe in xfs_da_blkinfo.magic @8, and the
// XD2F/XDF3 free-index blocks) hold no entries, so skipping them and scanning the
// data blocks covers block, leaf, node and data dir formats uniformly.
void CollectDirEntries(const WalkCtx& ctx, std::vector<Extent> runs, std::vector<DirEntryRef>* out) {
    std::sort(runs.begin(), runs.end(),
              [](const Extent& a, const Extent& b) { return a.startoff < b.startoff; });
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [](const Extent& e) { return e.count == 0; }),
               runs.end());
    if (runs.empty()) return;
    const uint8_t dirblklog = ctx.dirblklogForDirs;
    const uint64_t blkFsb = 1ull << dirblklog;                 // dir blocks per fsb log
    const uint32_t blkBytes = ctx.sb.blocksize << dirblklog;
    const uint64_t lastFsb = runs.back().startoff + runs.back().count;
    size_t ri = 0;
    std::vector<uint8_t> buf(blkBytes);
    for (uint64_t fsb = 0; fsb + blkFsb <= lastFsb; fsb += blkFsb) {
        while (ri < runs.size() && runs[ri].startoff + runs[ri].count <= fsb) ++ri;
        if (ri >= runs.size()) break;
        const Extent& r = runs[ri];
        if (r.startoff > fsb || r.startoff + r.count < fsb + blkFsb) continue; // gap/split
        const uint64_t off = ctx.partOffset + (r.startblock + (fsb - r.startoff)) * ctx.sb.blocksize;
        if (!ctx.reader->readBytes(off, blkBytes, buf.data()).success) continue;
        const uint32_t magic = Be32(buf.data());
        switch (magic) {
            case kDir2BlockMagic: ParseDirBlock(buf.data(), blkBytes, true, false, out); break;
            case kDir2DataMagic: ParseDirBlock(buf.data(), blkBytes, false, false, out); break;
            case kDir3BlockMagic: ParseDirBlock(buf.data(), blkBytes, true, true, out); break;
            case kDir3DataMagic: ParseDirBlock(buf.data(), blkBytes, false, true, out); break;
            default: break; // leaf/node/free/zero: index or unknown, no entries here
        }
    }
}

// ---- tree walk --------------------------------------------------------------

void WalkInode(WalkCtx& ctx, uint64_t ino, const std::string& path,
               const XfsParser::FileCallback& cb, std::set<uint64_t>* visited, int depth) {
    if (depth > kMaxWalkDepth) return;
    if (!visited->insert(ino).second) return; // cycle guard
    std::vector<uint8_t> raw;
    if (!ReadInodeAt(*ctx.reader, ctx.partOffset, ctx.sb, ino, &raw)) return;
    const InodeView v = DecodeInode(raw);
    if (!v.ok) return; // unknown inode version/magic: skip honestly

    std::vector<Extent> exts;
    ExtentsForInode(ctx, v, &exts);
    std::sort(exts.begin(), exts.end(),
              [](const Extent& a, const Extent& b) { return a.startoff < b.startoff; });
    std::vector<std::pair<uint64_t, uint64_t>> runs;
    for (const Extent& e : exts) {
        if (e.count == 0) continue;
        if (e.startblock > (UINT64_MAX >> ctx.sb.blocklog)) continue; // byte offset would wrap
        runs.emplace_back(e.startblock * ctx.sb.blocksize, e.count * ctx.sb.blocksize);
    }

    const uint16_t fmt = v.mode & 0xF000;
    if (fmt == kModeDir) {
        cb(path.empty() ? "/" : path, ino, v.size, true, runs);
        if (!ctx.dirOk) return;
        std::vector<DirEntryRef> entries;
        if (v.format == kFmtLocal) {
            CollectSfEntries(ctx, v.fork, v.forkLen, v.size, &entries);
        } else if (v.format == kFmtExtents || v.format == kFmtBtree) {
            CollectDirEntries(ctx, std::move(exts), &entries);
        } // else: unknown dir format, no entries (honest)
        for (const DirEntryRef& e : entries) {
            WalkInode(ctx, e.ino, path + "/" + e.name, cb, visited, depth + 1);
        }
    } else if (fmt == kModeReg) {
        cb(path, ino, v.size, false, runs);
    } // symlinks/devices/etc. are out of scope for the file walk
}

} // namespace

bool XfsParser::open(DiskReader& reader, uint64_t partitionOffsetBytes) {
    reader_ = nullptr;
    sb_ = {};
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint8_t buf[512] = {};
    if (!reader.readBytes(partitionOffsetBytes, sizeof(buf), buf).success) return false;
    if (Be32(buf) != kSbMagic) return false; // xfs_dsb.sb_magicnum

    XfsSuperblock sb;
    sb.blocksize = Be32(buf + 4);    // sb_blocksize
    sb.dblocks = Be64(buf + 8);      // sb_dblocks
    sb.rootino = Be64(buf + 56);     // sb_rootino
    sb.agblocks = Be32(buf + 84);    // sb_agblocks
    sb.agcount = Be32(buf + 88);     // sb_agcount
    sb.versionnum = Be16(buf + 100); // sb_versionnum
    sb.inodesize = Be16(buf + 104);  // sb_inodesize
    sb.blocklog = buf[120];          // sb_blocklog
    sb.inodelog = buf[122];          // sb_inodelog
    sb.inopblog = buf[123];          // sb_inopblog
    sb.agblklog = buf[124];          // sb_agblklog

    if (!IsPow2(sb.blocksize) || sb.blocksize < 512 || sb.blocksize > 65536) return false;
    if (sb.blocklog != Log2Exact(sb.blocksize)) return false;
    if (!IsPow2(sb.inodesize) || sb.inodesize < 256 || sb.inodesize > sb.blocksize) return false;
    if (sb.inodelog != Log2Exact(sb.inodesize)) return false;
    if (sb.inopblog != sb.blocklog - sb.inodelog) return false;
    if ((sb.versionnum & kSbVersionNumBits) < 4) return false; // legacy v1-3 layouts
    if (sb.agcount < 1 || sb.agblocks < 1) return false;
    if (sb.agblklog != CeilLog2(sb.agblocks)) return false;

    // Cross-check the secondary superblock: AG 1 starts at agblocks*blocksize and
    // must carry the same magic (xfs_sb_to_disk writes a full sb per AG).
    uint8_t sec[4] = {};
    if (!reader.readBytes(partitionOffsetBytes + static_cast<uint64_t>(sb.agblocks) * sb.blocksize,
                          sizeof(sec), sec).success) {
        return false;
    }
    if (Be32(sec) != kSbMagic) return false;

    sb_ = sb;
    partOffset_ = partitionOffsetBytes;
    reader_ = &reader;
    return true;
}

bool XfsParser::readInode(uint64_t inodeNo, std::vector<uint8_t>& out) const {
    if (!reader_) return false;
    return ReadInodeAt(*reader_, partOffset_, sb_, inodeNo, &out);
}

bool XfsParser::walkTree(const FileCallback& cb) {
    if (!reader_) return false;

    WalkCtx ctx;
    ctx.reader = reader_;
    ctx.partOffset = partOffset_;
    ctx.sb = sb_;
    // sb_dirblklog (u8 @192) is not part of the XfsSuperblock contract; re-read it.
    uint8_t dirblklog = 0;
    const bool gotDirblklog = reader_->readBytes(partOffset_ + kSbDirblklogOff, 1, &dirblklog).success;
    ctx.dirblklogForDirs = dirblklog;
    // Pre-dir-v2 filesystems (XFS_SB_VERSION_DIRV2BIT clear) use the legacy v1 dir
    // format, which we do not decode.
    ctx.dirOk = gotDirblklog && dirblklog <= 8 && sb_.blocklog + dirblklog <= 16 &&
                (sb_.versionnum & kSbVersionDirV2Bit) != 0;

    std::set<uint64_t> visited;
    std::vector<uint8_t> raw;
    if (!ReadInodeAt(*reader_, partOffset_, sb_, sb_.rootino, &raw)) return false;
    WalkInode(ctx, sb_.rootino, std::string(), cb, &visited, 0);
    return true;
}

} // namespace byteback
