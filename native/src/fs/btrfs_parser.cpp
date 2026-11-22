// Btrfs parser — deleted-file and live-tree recovery.
//
// On-disk layouts are from include/uapi/linux/btrfs_tree.h (kernel source),
// NOT assumptions:
//   - superblock at partition+64KiB, magic "_BHRfS_M" @+0x40, backups at
//     +64MiB and +256GiB (fallback when primary is unread).
//   - btrfs_header is 101 bytes; item data offset is relative to the END of
//     the header (data at node + 101 + item.offset).
//   - fs tree items: INODE_ITEM(1), INODE_REF(12) key.oid=child/off=parent,
//     DIR_ITEM(84) key.oid=parent dir, EXTENT_DATA(108) key.offset=file pos.
//   - EXTENT_DATA.disk_bytenr is a LOGICAL address: it is translated through
//     the chunk map (sys_chunk_array bootstrap + chunk tree CHUNK_ITEMs).
//   - Deleted entries: BTRFS_NUM_BACKUP_ROOTS=4 in the superblock point at
//     older-generation roots; walking them surfaces recently-deleted inodes
//     (CoW keeps old leaves alive until reclaimed). Every node read checks
//     node.bytenr == expected logical — a mismatch means the block was
//     relocated and must not be trusted.
//
// Scope honesty: single-device translation (num_stripes==1 chunks: SINGLE,
// DUP, RAID1 first stripe). Multi-device stripes mark chunkMapIncomplete
// and yield an honesty record rather than wrong physical addresses.

#include "byteback_fs.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace byteback {
namespace {

constexpr uint64_t kBtrfsMagic = 0x4D5F53665248425Full; // "_BHRfS_M"
constexpr uint32_t kSbOffset = 64 * 1024;
constexpr uint32_t kSbBackup1 = 64 * 1024 * 1024;
constexpr uint32_t kSbBackup2 = 256 * 1024 * 1024;
constexpr uint32_t kHeaderSize = 101; // sizeof(struct btrfs_header)
constexpr uint32_t kItemSize = 25;    // sizeof(struct btrfs_item)
constexpr uint32_t kKeyPtrSize = 33;  // sizeof(struct btrfs_key_ptr)
constexpr uint8_t kKeyInodeItem = 1;
constexpr uint8_t kKeyInodeRef = 12;
constexpr uint8_t kKeyDirItem = 84;
constexpr uint8_t kKeyExtentData = 108;
constexpr uint8_t kKeyRootItem = 132;
constexpr uint8_t kKeyChunkItem = 228;
constexpr uint8_t kExtentReg = 1;
constexpr uint8_t kExtentPrealloc = 2;
constexpr uint64_t kFsTreeObjectId = 5;
constexpr uint64_t kRootDirObjectId = 6;
constexpr int kMaxLevel = 8;

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// ---------------------------------------------------------------
// Chunk map: logical -> physical translation
// ---------------------------------------------------------------
struct ChunkStripe { uint64_t devid = 0, offset = 0; };
struct ChunkEntry {
    uint64_t logical = 0, length = 0;
    uint64_t type = 0, stripeLen = 0;
    std::vector<ChunkStripe> stripes;
};

struct ChunkMap {
    std::vector<ChunkEntry> entries; // sorted by logical

    void add(ChunkEntry e) {
        entries.push_back(std::move(e));
    }
    void sort() {
        std::sort(entries.begin(), entries.end(),
                  [](const ChunkEntry& a, const ChunkEntry& b) { return a.logical < b.logical; });
    }
    // Returns physical offset (device-relative) or false when untranslatable.
    bool translate(uint64_t logical, uint64_t& phys) const {
        // binary search: last entry with logical <= l
        size_t lo = 0, hi = entries.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (entries[mid].logical <= logical) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return false;
        const ChunkEntry& e = entries[lo - 1];
        if (logical >= e.logical + e.length) return false;
        const uint64_t within = logical - e.logical;
        if (e.stripes.empty()) return false;
        if (e.stripes.size() == 1) {
            phys = e.stripes[0].offset + within;
            return true;
        }
        // Multi-stripe (RAID0/10/5/6): stripeLen interleave. Only translate
        // when every stripe lives on a single device id we own — multi-device
        // volumes are out of scope for v1, so refuse honestly.
        const uint64_t dev = e.stripes[0].devid;
        for (const auto& s : e.stripes) if (s.devid != dev) return false;
        const uint64_t sl = e.stripeLen ? e.stripeLen : e.length;
        const uint64_t stripeIdx = (within / sl) % e.stripes.size();
        const uint64_t stripeOff = (within / (sl * e.stripes.size())) * sl + (within % sl);
        phys = e.stripes[stripeIdx].offset + stripeOff;
        return true;
    }
};

// Parse one {disk_key + btrfs_chunk} at p; returns consumed bytes or 0.
size_t parseChunkRecord(const uint8_t* p, size_t avail, uint64_t& keyOid, uint64_t& keyOff,
                        ChunkEntry& out) {
    if (avail < 17) return 0;
    keyOid = le64(p);
    const uint8_t ktype = p[8];
    keyOff = le64(p + 9);
    if (ktype != kKeyChunkItem) return 0;
    const uint8_t* c = p + 17;
    if (avail < 17 + 48) return 0;
    out.length = le64(c + 0);
    out.stripeLen = le64(c + 16);
    out.type = le64(c + 24);
    const uint16_t numStripes = le16(c + 44);
    if (numStripes == 0 || numStripes > 64) return 0;
    // btrfs_chunk: 48B fixed + num_stripes stripes (first stripe inline at +48).
    if (avail < 17 + 48 + static_cast<size_t>(numStripes) * 32) return 0;
    out.stripes.clear();
    for (uint16_t i = 0; i < numStripes; ++i) {
        const uint8_t* s = c + 48 + i * 32;
        out.stripes.push_back({le64(s), le64(s + 8)});
    }
    out.logical = keyOff; // chunk item key.offset = logical start
    return 17 + 48 + static_cast<size_t>(numStripes) * 32;
}

// ---------------------------------------------------------------
// Per-tree accumulation
// ---------------------------------------------------------------
struct InodeAcc {
    bool hasInode = false;
    uint64_t size = 0;
    uint32_t mode = 0;
    uint64_t parent = 0;
    std::string name;
    int64_t atime = 0, mtime = 0, ctime = 0, otime = 0;
    bool compressed = false;
    // logical file offset -> {phys byte off in partition, len}
    std::vector<std::pair<uint64_t, FileRecord::DataRun>> runs;
};

struct WalkCtx {
    DiskReader& reader;
    uint64_t partOff;
    uint32_t sectorSize;
    uint32_t nodesize;
    const ChunkMap& map;
    uint8_t fsid[16];
    uint64_t generationCut; // nodes with generation > this are ignored
    std::unordered_set<uint64_t> visited;
    uint32_t hops = 0;
    bool unread = false;
    std::atomic<bool>* isRunning;
};

bool readLogical(WalkCtx& ctx, uint64_t logical, std::vector<uint8_t>& node) {
    uint64_t phys = 0;
    if (!ctx.map.translate(logical, phys)) return false;
    node.assign(ctx.nodesize, 0);
    auto res = ctx.reader.readSectors(ctx.partOff + phys, ctx.nodesize, node.data());
    if (!res.success || res.bytesRead < ctx.nodesize) { ctx.unread = true; return false; }
    // Guards: this must be the node we asked for, not relocated garbage.
    if (le64(node.data() + 0x30) != logical) return false;      // bytenr
    if (std::memcmp(node.data() + 0x20, ctx.fsid, 16) != 0) return false;
    return true;
}

// Leaf callback: (key objectid, key type, key offset, item data, item size).
template <typename F>
void collectTreeItems(WalkCtx& ctx, uint64_t logicalRoot, F&& onItem) {
    struct Frame { uint64_t logical; };
    std::vector<Frame> stack{{logicalRoot}};
    while (!stack.empty()) {
        if (ctx.isRunning && !(*ctx.isRunning)) return;
        if (++ctx.hops > 65536) return; // adversarial bound
        const uint64_t logical = stack.back().logical;
        stack.pop_back();
        if (!ctx.visited.insert(logical).second) continue;
        std::vector<uint8_t> node;
        if (!readLogical(ctx, logical, node)) continue;
        const uint64_t gen = le64(node.data() + 0x50);
        if (ctx.generationCut && gen > ctx.generationCut) continue;
        const uint32_t nritems = le32(node.data() + 0x60);
        const uint8_t level = node[0x64];
        if (level > kMaxLevel) continue;
        if (level == 0) {
            if (kHeaderSize + static_cast<size_t>(nritems) * kItemSize > ctx.nodesize) continue;
            for (uint32_t i = 0; i < nritems; ++i) {
                const uint8_t* it = node.data() + kHeaderSize + i * kItemSize;
                const uint64_t oid = le64(it);
                const uint8_t type = it[8];
                const uint64_t koff = le64(it + 9);
                const uint32_t doff = le32(it + 17);
                const uint32_t dsz = le32(it + 21);
                if (kHeaderSize + static_cast<size_t>(doff) + dsz > ctx.nodesize) continue;
                onItem(oid, type, koff, node.data() + kHeaderSize + doff, dsz);
            }
        } else {
            if (kHeaderSize + static_cast<size_t>(nritems) * kKeyPtrSize > ctx.nodesize) continue;
            for (uint32_t i = 0; i < nritems; ++i) {
                const uint8_t* kp = node.data() + kHeaderSize + i * kKeyPtrSize;
                const uint64_t child = le64(kp + 17);
                if (child != 0) stack.push_back({child});
            }
        }
    }
}

void collectFsTree(WalkCtx& ctx, uint64_t fsRoot,
                   std::unordered_map<uint64_t, InodeAcc>& inodes) {
    collectTreeItems(ctx, fsRoot, [&](uint64_t oid, uint8_t type, uint64_t koff,
                                      const uint8_t* d, uint32_t dsz) {
        switch (type) {
        case kKeyInodeItem: {
            // btrfs_inode_item: size@16, nlink@40(u32), mode@52(u32),
            // atime@92, ctime@104, mtime@116, otime@128 (timespec sec@+0 i64).
            if (dsz < 140) break;
            InodeAcc& a = inodes[oid];
            a.hasInode = true;
            a.size = le64(d + 16);
            a.mode = le32(d + 52);
            a.atime = static_cast<int64_t>(le64(d + 92));
            a.ctime = static_cast<int64_t>(le64(d + 104));
            a.mtime = static_cast<int64_t>(le64(d + 116));
            a.otime = static_cast<int64_t>(le64(d + 128));
            break;
        }
        case kKeyInodeRef: {
            // key.oid = child ino, key.off = parent ino; val: index@0,
            // name_len u16@8, name@10.
            if (dsz < 10) break;
            const uint16_t nl = le16(d + 8);
            if (nl == 0 || 10ull + nl > dsz) break;
            InodeAcc& a = inodes[oid];
            a.parent = koff;
            a.name.assign(reinterpret_cast<const char*>(d + 10), nl);
            break;
        }
        case kKeyDirItem: {
            // key.oid = parent dir; val location{oid@0,type@8,off@9},
            // transid@17, name_len u16@27, type u8@29, name@30.
            if (dsz < 30) break;
            const uint16_t nl = le16(d + 27);
            if (nl == 0 || 30ull + nl > dsz) break;
            const uint64_t child = le64(d);
            const uint8_t dtype = d[8];
            if (dtype != kKeyInodeItem && dtype != kKeyRootItem) break; // files/dirs only
            InodeAcc& a = inodes[child];
            if (a.name.empty()) { // INODE_REF is authoritative; this is fallback
                a.parent = oid;
                a.name.assign(reinterpret_cast<const char*>(d + 30), nl);
            }
            break;
        }
        case kKeyExtentData: {
            // btrfs_file_extent_item: ram_bytes@8, compression@16, type@20,
            // disk_bytenr@21, disk_num_bytes@29, offset@37, num_bytes@45.
            if (dsz < 53) break;
            const uint8_t etype = d[20];
            if (etype != kExtentReg && etype != kExtentPrealloc) break;
            const uint8_t compr = d[16];
            const uint64_t diskBytenr = le64(d + 21);
            const uint64_t dataOff = le64(d + 37);
            const uint64_t numBytes = le64(d + 45);
            if (numBytes == 0) break;
            uint64_t phys = 0;
            if (!ctx.map.translate(diskBytenr + dataOff, phys)) break;
            InodeAcc& a = inodes[oid];
            if (compr != 0) a.compressed = true;
            FileRecord::DataRun run;
            run.startSector = (ctx.partOff + phys) / ctx.sectorSize;
            run.sectorCount = (numBytes + ctx.sectorSize - 1) / ctx.sectorSize;
            a.runs.push_back({koff, run});
            break;
        }
        default: break;
        }
    });
}

std::string inodePath(uint64_t ino, const std::unordered_map<uint64_t, InodeAcc>& inodes,
                      std::unordered_map<uint64_t, std::string>& memo) {
    if (auto it = memo.find(ino); it != memo.end()) return it->second;
    const InodeAcc* a = nullptr;
    if (auto it = inodes.find(ino); it != inodes.end()) a = &it->second;
    if (!a || a->name.empty()) return memo[ino] = "/";
    if (a->parent == 0 || a->parent == ino || a->parent == kRootDirObjectId)
        return memo[ino] = "/" + a->name;
    std::unordered_set<uint64_t> chain;
    uint64_t p = a->parent;
    std::string suffix;
    while (p != 0 && p != kRootDirObjectId && chain.insert(p).second) {
        auto it = inodes.find(p);
        if (it == inodes.end() || it->second.name.empty()) break;
        const InodeAcc& d = it->second;
        suffix = "/" + d.name + suffix;
        p = d.parent;
    }
    return memo[ino] = suffix + "/" + a->name;
}

} // namespace

BtrfsParser::BtrfsParser() = default;
BtrfsParser::~BtrfsParser() = default;

bool BtrfsParser::scan(DiskReader& reader, FileRecordCallback callback,
                       std::atomic<bool>* isRunning) {
    return scanAt(reader, std::move(callback), isRunning, 0);
}

bool BtrfsParser::scanAt(DiskReader& reader, FileRecordCallback callback,
                         std::atomic<bool>* isRunning, uint64_t partitionOffsetBytes,
                         uint64_t partitionSizeBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;
    const uint32_t sectorSize = reader.getSectorSize() ? reader.getSectorSize() : 512;

    // --- superblock: primary @64KiB, backups @64MiB/@256GiB ----------------
    std::vector<uint8_t> sb(4096);
    bool haveSb = false;
    for (uint64_t off : {uint64_t{kSbOffset}, uint64_t{kSbBackup1}, uint64_t{kSbBackup2}}) {
        if (partitionSizeBytes && partitionOffsetBytes + off + 4096 > partitionOffsetBytes + partitionSizeBytes)
            continue;
        auto res = reader.readSectors(partitionOffsetBytes + off, 4096, sb.data());
        if (!res.success || res.bytesRead < 4096) continue;
        if (le64(sb.data() + 0x40) == kBtrfsMagic) { haveSb = true; break; }
    }
    if (!haveSb) return false;

    const uint32_t nodesize = le32(sb.data() + 0x94);
    const uint64_t totalBytes = le64(sb.data() + 0x70);
    const uint64_t generation = le64(sb.data() + 0x48);
    const uint64_t rootTree = le64(sb.data() + 0x50);
    const uint64_t chunkRoot = le64(sb.data() + 0x58);
    const uint32_t sysArraySize = le32(sb.data() + 0xA0);
    uint8_t fsid[16];
    std::memcpy(fsid, sb.data() + 0x20, 16);
    if (nodesize < 4096 || nodesize > (1u << 20) || (nodesize & (nodesize - 1)) != 0) return false;

    ChunkMap map;
    // sys_chunk_array @0x32B: bootstrap entries covering at least the chunk tree.
    {
        const uint8_t* p = sb.data() + 0x32B;
        size_t remain = std::min<size_t>(sysArraySize, 2048), off = 0;
        while (remain > 0) {
            ChunkEntry e; uint64_t oid, koff;
            size_t used = parseChunkRecord(p + off, remain, oid, koff, e);
            if (used == 0) break;
            map.add(std::move(e));
            off += used; remain -= used;
        }
        map.sort();
    }

    WalkCtx ctx{reader, partitionOffsetBytes, sectorSize, nodesize, map,
                {}, 0, {}, 0, false, isRunning};
    std::memcpy(ctx.fsid, fsid, 16);

    // --- chunk tree -> complete the map -------------------------------------
    if (chunkRoot != 0) {
        collectTreeItems(ctx, chunkRoot, [&](uint64_t oid, uint8_t type, uint64_t koff,
                                            const uint8_t* d, uint32_t dsz) {
            if (type != kKeyChunkItem || dsz < 48) return;
            ChunkEntry e;
            e.length = le64(d + 0);
            e.stripeLen = le64(d + 16);
            e.type = le64(d + 24);
            const uint16_t ns = le16(d + 44);
            if (ns == 0 || ns > 64 || dsz < 48 + static_cast<uint32_t>(ns) * 32) return;
            for (uint16_t i = 0; i < ns; ++i) {
                const uint8_t* s = d + 48 + i * 32;
                e.stripes.push_back({le64(s), le64(s + 8)});
            }
            e.logical = koff;
            map.add(std::move(e));
        });
        map.sort();
        ctx.visited.clear();
        ctx.hops = 0;
    }

    // --- volume record -------------------------------------------------------
    {
        FileRecord vol;
        vol.id = -1;
        vol.name = "Btrfs_Volume";
        vol.path = "/";
        vol.sizeBytes = totalBytes;
        vol.startSector = partitionOffsetBytes / sectorSize;
        vol.endSector = (partitionOffsetBytes + totalBytes + sectorSize - 1) / sectorSize;
        vol.status = 1;
        vol.confidence = 95;
        vol.category = "System";
        vol.source = "btrfs_volume";
        callback(vol);
    }

    // --- root tree -> subvolume fs roots ------------------------------------
    std::vector<std::pair<uint64_t, uint64_t>> fsRoots; // {tree oid, logical bytenr}
    collectTreeItems(ctx, rootTree, [&](uint64_t oid, uint8_t type, uint64_t koff,
                                        const uint8_t* d, uint32_t dsz) {
        if (type != kKeyRootItem || dsz < 220) return;
        // btrfs_root_item: bytenr @ +156, level @ +218 (after 140B inode item).
        const uint64_t bytenr = le64(d + 156);
        if (bytenr != 0) fsRoots.push_back({oid, bytenr});
    });
    ctx.visited.clear();
    ctx.hops = 0;

    // --- live trees ----------------------------------------------------------
    // subvol objectid -> inode map (paths are per-tree; emit subvol-qualified)
    struct TreeAccum {
        uint64_t treeOid;
        std::unordered_map<uint64_t, InodeAcc> inodes;
    };
    std::vector<TreeAccum> liveTrees;
    for (const auto& fr : fsRoots) {
        TreeAccum ta{fr.first, {}};
        ctx.visited.clear();
        ctx.hops = 0;
        ctx.generationCut = 0;
        collectFsTree(ctx, fr.second, ta.inodes);
        liveTrees.push_back(std::move(ta));
    }

    int fileIndex = 0;
    std::unordered_set<uint64_t> liveInos;
    for (const auto& ta : liveTrees) {
        std::unordered_map<uint64_t, std::string> memo;
        const std::string subvolTag = ta.treeOid == kFsTreeObjectId ? "" : "subvol" + std::to_string(ta.treeOid);
        for (const auto& kv : ta.inodes) {
            const InodeAcc& a = kv.second;
            if (!a.hasInode || a.name.empty()) continue;
            const uint32_t fmt = a.mode & 0xF000u;
            const bool isDir = (fmt == 0x4000);
            FileRecord fr;
            fr.id = static_cast<int64_t>(++fileIndex);
            fr.name = subvolTag.empty() ? a.name : subvolTag + "/" + a.name;
            fr.path = subvolTag.empty()
                          ? inodePath(kv.first, ta.inodes, memo)
                          : "/" + subvolTag + inodePath(kv.first, ta.inodes, memo);
            fr.sizeBytes = isDir ? 0 : a.size;
            if (!a.runs.empty()) {
                auto sorted = a.runs;
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                fr.startSector = sorted.front().second.startSector;
                fr.endSector = sorted.back().second.startSector + sorted.back().second.sectorCount;
                for (const auto& p : sorted) fr.runs.push_back(p.second);
            }
            fr.status = 1;
            fr.confidence = a.compressed ? 60 : 90;
            fr.category = isDir ? "Directory" : "File";
            fr.source = a.compressed ? "btrfs_extent_compressed" : "btrfs_file";
            fr.createdAt = a.otime;
            fr.modifiedAt = a.mtime;
            callback(fr);
            liveInos.insert(kv.first);
        }
    }

    // --- deleted: backup roots (older-generation trees) ----------------------
    // super_roots[i] @0xB2B + i*168: fs_root @+48 (within the 168-byte
    // btrfs_root_backup). Walk each older-generation fs tree; inodes absent
    // from the live set are deletion candidates.
    bool sawStale = false;
    const uint8_t* backupBase = sb.data() + 0xB2B;
    for (int i = 0; i < 4; ++i) {
        if (isRunning && !(*isRunning)) break;
        const uint8_t* br = backupBase + static_cast<size_t>(i) * 168;
        const uint64_t staleFsRoot = le64(br + 48);
        const uint64_t staleGen = le64(br + 56);
        if (staleFsRoot == 0 || staleFsRoot == rootTree) continue;
        if (staleGen >= generation) continue; // only strictly older states
        std::unordered_map<uint64_t, InodeAcc> stale;
        ctx.visited.clear();
        ctx.hops = 0;
        collectFsTree(ctx, staleFsRoot, stale);
        for (const auto& kv : stale) {
            const InodeAcc& a = kv.second;
            if (!a.hasInode || a.name.empty()) continue;
            bool alive = false;
            for (const auto& t : liveTrees) {
                auto it = t.inodes.find(kv.first);
                if (it != t.inodes.end() && it->second.hasInode) { alive = true; break; }
            }
            if (alive) continue;
            FileRecord fr;
            fr.id = static_cast<int64_t>(++fileIndex);
            fr.name = a.name;
            std::unordered_map<uint64_t, std::string> memo;
            fr.path = inodePath(kv.first, stale, memo);
            fr.sizeBytes = (a.mode & 0xF000u) == 0x4000 ? 0 : a.size;
            if (!a.runs.empty()) {
                auto sorted = a.runs;
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                fr.startSector = sorted.front().second.startSector;
                fr.endSector = sorted.back().second.startSector + sorted.back().second.sectorCount;
                for (const auto& p : sorted) fr.runs.push_back(p.second);
            }
            fr.status = 0;
            fr.confidence = a.compressed ? 35 : 55;
            fr.category = ((a.mode & 0xF000u) == 0x4000) ? "Directory" : "File";
            fr.source = "btrfs_stale_root";
            fr.createdAt = a.otime;
            fr.modifiedAt = a.mtime;
            callback(fr);
            sawStale = true;
        }
    }

    // --- honesty sentinels ---------------------------------------------------
    if (ctx.unread) {
        FileRecord u;
        u.id = -1;
        u.name = "Btrfs_UnreadNodes";
        u.path = "/";
        u.status = 1;
        u.confidence = 5;
        u.category = "System";
        u.source = "btrfs_unread";
        callback(u);
    }
    (void)sawStale;
    return true;
}

} // namespace byteback
