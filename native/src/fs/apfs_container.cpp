#include "fs/apfs_container.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace byteback {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

std::string readCString(const uint8_t* p, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < maxLen && p[i] != 0; ++i) out.push_back(static_cast<char>(p[i]));
    return out;
}

bool readBlock(DiskReader& reader, uint64_t offset, uint32_t size, std::vector<uint8_t>& buf) {
    buf.resize(size);
    auto res = reader.readSectors(offset, size, buf.data());
    return readComplete(res, size);
}

// Sector-at-a-time so a faulted NXSB tail does not drop a complete prefix
// (probe already classified APFS from the first 512 bytes).
void readPrefixed(DiskReader& reader, uint64_t offset, uint32_t size, uint32_t sectorSize,
                  std::vector<uint8_t>& buf, bool& unread) {
    if (sectorSize == 0) sectorSize = 512;
    buf.assign(size, 0);
    std::vector<uint8_t> sec(sectorSize);
    for (uint32_t o = 0; o < size; o += sectorSize) {
        const uint32_t take = std::min(sectorSize, size - o);
        auto res = reader.readSectors(offset + o, take, sec.data());
        if (!readComplete(res, take)) {
            unread = true;
            continue;
        }
        std::memcpy(buf.data() + o, sec.data(), take);
    }
}

void emitNxsbUnread(FileSystemParser::FileRecordCallback& callback,
                    uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    FileRecord fr;
    fr.id = -1;
    fr.name = "Apfs_NxsbUnread";
    fr.path = kApfsNxsbUnreadPath;
    fr.source = kApfsNxsbUnreadSource;
    fr.category = "System";
    fr.status = 0;
    fr.confidence = 20;
    fr.startSector = partitionOffsetBytes / sectorSize;
    const uint64_t nsec = (4096u + sectorSize - 1) / sectorSize;
    fr.endSector = fr.startSector + (nsec == 0 ? 1 : nsec);
    callback(fr);
}

void emitBlockUnread(FileSystemParser::FileRecordCallback& callback,
                     uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    FileRecord fr;
    fr.id = -1;
    fr.name = "Apfs_BlockUnread";
    fr.path = kApfsBlockUnreadPath;
    fr.source = kApfsBlockUnreadSource;
    fr.category = "System";
    fr.status = 0;
    fr.confidence = 20;
    fr.startSector = partitionOffsetBytes / sectorSize;
    fr.endSector = fr.startSector + 1;
    callback(fr);
}

void emitCatalogUnread(FileSystemParser::FileRecordCallback& callback,
                       uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    FileRecord fr;
    fr.id = -1;
    fr.name = "Apfs_CatalogUnread";
    fr.path = kApfsCatalogUnreadPath;
    fr.source = kApfsCatalogUnreadSource;
    fr.category = "System";
    fr.status = 0;
    fr.confidence = 20;
    fr.startSector = partitionOffsetBytes / sectorSize;
    fr.endSector = fr.startSector + 1;
    callback(fr);
}

constexpr uint64_t kApfsTypeDirRec = 9;
constexpr uint64_t kApfsTypeFileExtent = 8;
constexpr uint64_t kApfsTypeInode = 3;
constexpr uint32_t kObjBtreeNode = 3; // APFS_OBJECT_TYPE_BTREE_NODE

void emitVolume(FileSystemParser::FileRecordCallback& callback, int volumeIndex,
                const std::string& volName, uint64_t off, uint64_t blockSize, uint32_t sectorSize) {
    FileRecord fr;
    fr.id = volumeIndex;
    fr.name = volName;
    fr.path = "/apfs/" + volName;
    fr.sizeBytes = blockSize;
    fr.startSector = off / sectorSize;
    fr.endSector = fr.startSector + std::max<uint64_t>(1, blockSize / sectorSize);
    fr.status = 0;
    fr.confidence = 90;
    fr.category = "System";
    fr.source = "apfs_volume";
    callback(fr);
}

// ---- Real APFS B-tree layout -------------------------------------------
// obj_phys header: 32 B, o_type u32 @24 (OBJECT_TYPE_MASK 0xFFFF).
// btree_node_phys: btn_flags u16 @32 (leaf=2, fixedKV=4), btn_level u16 @34,
// btn_nkeys u32 @36, table of contents at +56.
//   non-fixed KV (FS catalog): TOC = kvloc{ nloc{off,len}, nloc{off,len} } —
//   8 B entries; key forward from btn_data(+56)+table_space.len+k.off,
//   value BACKWARD from node end (valsStart - v.off, len = v.len).
//   fixed KV (omap): TOC = kvoff{k u16, v u16} — 4 B entries; key forward
//   from keys start + k, value BACKWARD from node end (valsStart - v).
// FS key starts with j_key u64: type = >>60, object id = & OBJ_ID_MASK.
// dir_rec key oid is the PARENT dir inode; value holds the file's inode id.
constexpr uint16_t kBtnRoot = 0x0001;
constexpr uint16_t kBtnLeaf = 0x0002;
constexpr uint16_t kBtnFixedKv = 0x0004;

struct ApfsDirRec {
    uint64_t parentOid = 0;
    uint64_t inodeId = 0;
    std::string name;
    int64_t dateAdded = 0;
    bool fromStaleTree = false;
};
struct ApfsInode {
    uint64_t parentId = 0;
    int64_t created = 0;
    int64_t modified = 0;
    uint64_t size = 0;
    uint16_t mode = 0;
};
struct ApfsAccum {
    std::vector<ApfsDirRec> dirRecs;
    std::unordered_map<uint64_t, ApfsInode> inodes;
    // inode -> (logical offset, run) so extents assemble in file order.
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, FileRecord::DataRun>>> extents;
    std::unordered_set<std::string> emitted; // "inode/name" dedup
};

// Parse ONE leaf node's TOC; accumulate catalog entries. Returns false when
// the block is not an APFS btree leaf (caller ignores it silently).
bool collectCatalogNode(const uint8_t* block, uint32_t blockSize,
                        uint64_t partitionOffsetBytes, uint32_t sectorSize,
                        ApfsAccum& acc, bool fromStaleTree) {
    if (blockSize < 80) return false;
    const uint32_t otype = readLe32(block + 24) & 0xFFFFu;
    if (otype != kObjBtreeNode) return false;
    const uint16_t btnFlags = static_cast<uint16_t>(block[32] | (block[33] << 8));
    if (!(btnFlags & kBtnLeaf)) return false;
    if (block[34] != 0 || block[35] != 0) return false; // btn_level == 0
    const uint32_t nkeys = readLe32(block + 36);
    if (nkeys == 0 || nkeys > 4096) return false;
    const bool fixedKv = (btnFlags & kBtnFixedKv) != 0;
    const size_t tocStride = fixedKv ? 4 : 8;
    if (56 + static_cast<size_t>(nkeys) * tocStride > blockSize) return false;
    // Keys grow forward from the end of the TOC (btn_data + table_space.len);
    // values grow backward from node end (minus the 40 B btree_info if root).
    const uint32_t tableLen = static_cast<uint16_t>(block[0x2A] | (block[0x2B] << 8));
    const uint64_t keysStart = 56ull + tableLen;
    const uint64_t valsStart = blockSize - ((btnFlags & kBtnRoot) ? 40 : 0);

    for (uint32_t i = 0; i < nkeys; ++i) {
        uint64_t keyOff = 0, valOff = 0, valLen = 0;
        if (fixedKv) { // kvoff {k u16, v u16}
            const size_t e = 56 + static_cast<size_t>(i) * 4;
            const uint16_t k = static_cast<uint16_t>(block[e] | (block[e + 1] << 8));
            const uint16_t v = static_cast<uint16_t>(block[e + 2] | (block[e + 3] << 8));
            keyOff = keysStart + k;
            valOff = v != 0xFFFF ? valsStart - v : 0;
            valLen = blockSize;
        } else { // kvloc {k{off,len}, v{off,len}}
            const size_t e = 56 + static_cast<size_t>(i) * 8;
            const uint16_t ko = static_cast<uint16_t>(block[e] | (block[e + 1] << 8));
            const uint16_t vo = static_cast<uint16_t>(block[e + 4] | (block[e + 5] << 8));
            const uint16_t vl = static_cast<uint16_t>(block[e + 6] | (block[e + 7] << 8));
            keyOff = keysStart + ko;
            valOff = vo != 0xFFFF ? valsStart - vo : 0;
            valLen = vl;
        }
        if (keyOff + 8 > blockSize || valOff + 8 > blockSize) continue;
        if (!fixedKv) {
            if (valOff + valLen > valsStart) continue;
            valLen = std::min<uint64_t>(valLen, blockSize - valOff);
        }
        const uint8_t* key = block + keyOff;
        const uint8_t* val = block + valOff;
        const uint64_t jkey = readLe64(key);
        const uint64_t typ = jkey >> 60;
        const uint64_t oid = jkey & ((1ull << 60) - 1);

        if (typ == kApfsTypeDirRec) {
            // apfs_drec_hashed_key: hdr(8) + name_len_and_hash u32 + name.
            // Non-hashed apfs_drec_key uses u16 name_len → name at +10.
            // The +12 guard covers BOTH the hashed u32 at +8 and the plain
            // u16 at +8 (a name that can't fit is rejected by the len check).
            if (keyOff + 12 > blockSize) continue;
            uint32_t nlen = readLe32(key + 8) & 0x3FFu;
            size_t nameOff = keyOff + 12;
            if (nlen == 0 || nlen > 255 || nameOff + nlen > blockSize) {
                nlen = static_cast<uint16_t>(key[8] | (key[9] << 8));
                nameOff = keyOff + 10;
            }
            if (nlen == 0 || nameOff + nlen > blockSize) continue;
            std::string name(reinterpret_cast<const char*>(block + nameOff), nlen);
            while (!name.empty() && name.back() == '\0') name.pop_back();
            if (name.empty()) continue;
            if (valOff + 8 > blockSize) continue;
            ApfsDirRec rec;
            rec.parentOid = oid;
            rec.inodeId = readLe64(val);
            if (valOff + 16 <= blockSize) {
                const uint64_t ns = readLe64(val + 8);
                if (ns >= 1000000000ull) rec.dateAdded = static_cast<int64_t>(ns / 1000000000ull);
            }
            rec.name = std::move(name);
            rec.fromStaleTree = fromStaleTree;
            acc.dirRecs.push_back(std::move(rec));
        } else if (typ == kApfsTypeFileExtent) {
            // key: j_key(8) + logical_addr u64; val: len_flags u64 + phys u64
            if (keyOff + 16 > blockSize || valOff + 16 > blockSize) continue;
            const uint64_t logical = readLe64(key + 8);
            const uint64_t len = readLe64(val) & ((1ull << 56) - 1);
            const uint64_t phys = readLe64(val + 8);
            if (phys == 0 || len == 0) continue;
            FileRecord::DataRun run;
            run.startSector = (partitionOffsetBytes + phys * blockSize) / sectorSize;
            run.sectorCount = (len + sectorSize - 1) / sectorSize;
            if (run.sectorCount == 0) run.sectorCount = 1;
            acc.extents[oid].push_back({logical, run});
        } else if (typ == kApfsTypeInode) {
            // j_inode_val: parent@0, create@16, mod@24, mode u16@80, usize@88
            ApfsInode ino;
            if (valOff + 8 <= blockSize) ino.parentId = readLe64(val);
            if (valOff + 32 <= blockSize) {
                const uint64_t cns = readLe64(val + 16);
                const uint64_t mns = readLe64(val + 24);
                if (cns >= 1000000000ull) ino.created = static_cast<int64_t>(cns / 1000000000ull);
                if (mns >= 1000000000ull) ino.modified = static_cast<int64_t>(mns / 1000000000ull);
            }
            if (valOff + 82 <= blockSize) ino.mode = static_cast<uint16_t>(val[80] | (val[81] << 8));
            if (valOff + 96 <= blockSize) ino.size = readLe64(val + 88);
            acc.inodes[oid] = ino;
        }
    }
    return true;
}

// Emit accumulated catalog as FileRecords with real paths. Directory names
// come from dir_rec values (inode id of the dir); parents from dir_rec keys.
void flushApfsCatalog(ApfsAccum& acc, int& fileIndex,
                      FileSystemParser::FileRecordCallback& callback) {
    // inode -> (parent oid, own name) — only for inodes that are directories
    // named by a dir_rec.
    std::unordered_map<uint64_t, std::pair<uint64_t, std::string>> dirInfo;
    std::unordered_set<uint64_t> dirOids;
    for (const auto& d : acc.dirRecs) {
        if (d.inodeId == 0) continue;
        // Only a dir path needs this; storing all is harmless.
        dirInfo[d.inodeId] = {d.parentOid, d.name};
        dirOids.insert(d.parentOid);
    }
    std::unordered_map<uint64_t, std::string> pathMemo;
    auto dirPath = [&](uint64_t ino) -> std::string {
        auto cached = pathMemo.find(ino);
        if (cached != pathMemo.end()) return cached->second;
        std::vector<std::string> segs;
        std::unordered_set<uint64_t> hops{ino};
        uint64_t node = ino;
        for (int h = 0; h < 64 && node != 0 && node != 2; ++h) {
            auto it = dirInfo.find(node);
            if (it == dirInfo.end()) break;
            segs.push_back(it->second.second);
            node = it->second.first;
            if (!hops.insert(node).second) break;
        }
        std::string out;
        for (auto rit = segs.rbegin(); rit != segs.rend(); ++rit) out += "/" + *rit;
        pathMemo[ino] = out;
        return out;
    };

    for (const auto& d : acc.dirRecs) {
        const std::string dedupKey = std::to_string(d.inodeId) + "\x1f" + d.name;
        if (!acc.emitted.insert(dedupKey).second) continue;

        const auto iit = acc.inodes.find(d.inodeId);
        const ApfsInode* ino = iit != acc.inodes.end() ? &iit->second : nullptr;

        std::vector<FileRecord::DataRun> runs;
        uint64_t size = ino ? ino->size : 0;
        auto eit = acc.extents.find(d.inodeId);
        if (eit != acc.extents.end()) {
            auto ordered = eit->second;
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            uint64_t end = 0;
            for (const auto& lr : ordered) {
                runs.push_back(lr.second);
                end = std::max<uint64_t>(end, lr.first + lr.second.sectorCount * 512ull);
            }
            if (size == 0) size = end;
        }

        FileRecord fr;
        fr.id = fileIndex++;
        fr.parentId = static_cast<int64_t>(d.parentOid);
        fr.name = d.name;
        size_t dotPos = fr.name.find_last_of('.');
        fr.extension = (dotPos != std::string::npos && dotPos + 1 < fr.name.size())
                       ? fr.name.substr(dotPos + 1) : "";
        const bool isDir = (ino && (ino->mode & 0xF000) == 0x4000) ||
                           dirOids.count(d.inodeId) != 0;
        fr.path = dirPath(d.parentOid) + "/" + d.name;
        fr.sizeBytes = isDir ? 0 : size;
        fr.runs = runs;
        if (!runs.empty()) {
            fr.startSector = runs.front().startSector;
            uint64_t total = 0;
            for (const auto& r : runs) total += r.sectorCount;
            fr.endSector = fr.startSector + total;
        }
        fr.status = d.fromStaleTree ? 0 : 1;
        fr.confidence = d.fromStaleTree ? 48 : (runs.empty() ? 70 : 85);
        fr.category = isDir ? "Folder" : "Document";
        fr.source = d.fromStaleTree ? kApfsOmapDeletedSource : "apfs_file";
        fr.createdAt = ino && ino->created ? ino->created : d.dateAdded;
        fr.modifiedAt = ino ? ino->modified : 0;
        callback(fr);
    }

    // Extents whose inode has no dir_rec still carry recoverable data —
    // emit them as unnamed extent records instead of dropping the runs.
    std::unordered_set<uint64_t> namedInodes;
    for (const auto& d : acc.dirRecs) namedInodes.insert(d.inodeId);
    for (const auto& kv : acc.extents) {
        if (namedInodes.count(kv.first)) continue;
        auto ordered = kv.second;
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        FileRecord fr;
        fr.id = fileIndex++;
        fr.name = "extent_" + std::to_string(kv.first);
        fr.path = "/apfs/extent/" + std::to_string(kv.first);
        uint64_t total = 0;
        for (const auto& lr : ordered) {
            fr.runs.push_back(lr.second);
            total += lr.second.sectorCount;
        }
        if (fr.runs.empty()) continue;
        fr.startSector = fr.runs.front().startSector;
        fr.endSector = fr.startSector + total;
        fr.sizeBytes = total * 512ull;
        fr.status = 0;
        fr.confidence = 60;
        fr.category = "Document";
        fr.source = "apfs_extent";
        callback(fr);
    }
}

bool tryApsbAt(DiskReader& reader, uint64_t off, uint32_t blockSize, uint32_t sectorSize,
               int& volumeIndex, std::unordered_set<uint64_t>& seenVol,
               FileSystemParser::FileRecordCallback& callback, bool& unread) {
    std::vector<uint8_t> block;
    if (!readBlock(reader, off, blockSize, block)) {
        unread = true;
        return false;
    }
    if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) != 0) return false;
    if (seenVol.count(off)) return true;
    seenVol.insert(off);
    std::string volName = readCString(block.data() + 0x240, 256);
    if (volName.empty()) volName = readCString(block.data() + 72, 128);
    if (volName.empty()) volName = "Volume_" + std::to_string(volumeIndex + 1);
    emitVolume(callback, volumeIndex++, volName, off, blockSize, sectorSize);
    return true;
}

// Walk an omap b-tree: leaf entries fill oidMap (oid -> paddr), internal
// entries hold child oids that get resolved through the map afterwards.
void walkOmapBtree(DiskReader& reader, uint64_t partitionOffsetBytes, uint64_t blockSize,
                   uint64_t blockCount, uint64_t blockNum,
                   std::unordered_map<uint64_t, uint64_t>& oidMap,
                   std::unordered_set<uint64_t>& visited, bool& unread,
                   std::vector<uint64_t>& pendingChildren) {
    if (blockNum == 0 || blockNum >= blockCount || visited.count(blockNum)) return;
    visited.insert(blockNum);
    std::vector<uint8_t> blk;
    if (!readBlock(reader, partitionOffsetBytes + blockNum * blockSize,
                   static_cast<uint32_t>(blockSize), blk)) {
        unread = true;
        return;
    }
    if (blk.size() < 80) return;
    uint32_t otype = readLe32(blk.data() + 24) & 0xFFFFu;
    if (otype != kObjBtreeNode) return;
    const uint16_t tableLen = static_cast<uint16_t>(blk[0x2A] | (blk[0x2B] << 8));
    const uint64_t keysStart = 56ull + tableLen;
    uint16_t level = static_cast<uint16_t>(blk[34] | (blk[35] << 8));
    uint32_t nkeys = readLe32(blk.data() + 36);
    if (nkeys == 0 || nkeys > 4096) return;
    // omap nodes are fixed-KV: TOC entries are kvoff {k u16, v u16}; keys
    // forward from btn_data+table_space.len, values backward from node end.
    // Leaf value = 16 B apfs_omap_val {flags u32, size u32, paddr u64};
    // non-leaf value = 8 B child oid.
    std::vector<uint64_t> children;
    for (uint32_t i = 0; i < nkeys; ++i) {
        const size_t e = 56 + static_cast<size_t>(i) * 4;
        if (e + 4 > blockSize) break;
        const uint16_t ko = static_cast<uint16_t>(blk[e] | (blk[e + 1] << 8));
        const uint16_t vo = static_cast<uint16_t>(blk[e + 2] | (blk[e + 3] << 8));
        if (vo < 8 || vo > blockSize) continue;
        const uint64_t voff = blockSize - vo;
        if (level == 0) {
            if (voff + 16 > blockSize || keysStart + ko + 8 > blockSize) continue;
            const uint64_t oid = readLe64(blk.data() + keysStart + ko); // omap_key.ok_oid
            const uint64_t paddr = readLe64(blk.data() + voff + 8);     // omap_val.ov_paddr
            if (oid != 0 && paddr > 0 && paddr < blockCount) oidMap[oid] = paddr;
        } else {
            const uint64_t childOid = readLe64(blk.data() + voff);
            if (childOid != 0) children.push_back(childOid);
        }
    }
    // Resolve child oids through the map built so far; unresolved ones are
    // retried by the caller after the whole tree is collected.
    for (uint64_t c : children) {
        auto it = oidMap.find(c);
        if (it != oidMap.end())
            walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount,
                          it->second, oidMap, visited, unread, pendingChildren);
        else
            pendingChildren.push_back(c);
    }
}

} // namespace

void collectOmapLeafPaddrs(const uint8_t* block, uint32_t blockSize, uint64_t blockCount,
                           std::vector<uint64_t>& paddrs) {
    (void)blockCount;
    if (!block || blockSize < 80) return;
    uint32_t otype = readLe32(block + 24) & 0xFFFFu;
    if (otype != kObjBtreeNode) return;
    uint16_t level = static_cast<uint16_t>(block[34] | (block[35] << 8));
    if (level != 0) return;
    uint32_t nkeys = readLe32(block + 36);
    if (nkeys == 0 || nkeys > 4096) return;
    // fixed-KV kvoff: leaf value = apfs_omap_val, paddr at +8, backward
    // from node end.
    for (uint32_t i = 0; i < nkeys; ++i) {
        const size_t e = 56 + static_cast<size_t>(i) * 4;
        if (e + 4 > blockSize) break;
        const uint16_t vo = static_cast<uint16_t>(block[e + 2] | (block[e + 3] << 8));
        if (vo < 16 || vo > blockSize) continue;
        uint64_t paddr = readLe64(block + blockSize - vo + 8);
        if (paddr > 0) paddrs.push_back(paddr);
    }
}

bool walkApfsContainer(DiskReader& reader, uint64_t partitionOffsetBytes,
                       uint64_t partitionSizeBytes,
                       FileSystemParser::FileRecordCallback callback,
                       std::atomic<bool>* isRunning) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t spanBytes = reader.getDiskSize();
    if (partitionSizeBytes > 0) spanBytes = std::min(spanBytes, partitionOffsetBytes + partitionSizeBytes);
    if (spanBytes <= partitionOffsetBytes) return false;

    std::vector<uint8_t> nx;
    bool nxUnread = false;
    readPrefixed(reader, partitionOffsetBytes, 4096, sectorSize, nx, nxUnread);
    auto looksNxsb = [](const std::vector<uint8_t>& b) {
        return b.size() > 36 &&
               std::strncmp(reinterpret_cast<const char*>(b.data() + 32), "NXSB", 4) == 0;
    };
    if (!looksNxsb(nx)) {
        if (nxUnread) {
            emitNxsbUnread(callback, partitionOffsetBytes, sectorSize);
            return true;
        }
        const uint64_t backupOff =
            (spanBytes >= partitionOffsetBytes + 4096) ? spanBytes - 4096 : 0;
        if (backupOff <= partitionOffsetBytes) return false;
        bool backupUnread = false;
        readPrefixed(reader, backupOff, 4096, sectorSize, nx, backupUnread);
        if (!looksNxsb(nx)) return false;
    }

    uint32_t blockSize32 = readLe32(nx.data() + 36);
    uint64_t blockSize = blockSize32;
    if (blockSize < 4096 || blockSize > 1024 * 1024) {
        uint64_t legacy = readLe64(nx.data() + 40);
        blockSize = (legacy >= 4096 && legacy <= 1024 * 1024) ? legacy : 4096;
    }
    // nx_max_file_system_blocks lives at NXSB+48 (offset 40 is nx_xid);
    // keep the legacy read for images written before the spec offset landed.
    uint64_t blockCount = (nx.size() >= 56) ? readLe64(nx.data() + 48) : 0;
    if (blockCount == 0) blockCount = readLe64(nx.data() + 40);
    uint64_t maxBlocks = (spanBytes - partitionOffsetBytes) / blockSize;
    if (blockCount == 0 || blockCount > maxBlocks) blockCount = maxBlocks;
    if (blockCount == 0) return false;

    {
        FileRecord container;
        container.id = -1;
        container.name = "APFS_Container";
        container.path = "/apfs/";
        container.sizeBytes = blockCount * blockSize;
        container.startSector = partitionOffsetBytes / sectorSize;
        container.endSector = (partitionOffsetBytes + container.sizeBytes + sectorSize - 1) / sectorSize;
        container.status = 0;
        container.confidence = 95;
        container.category = "System";
        container.source = "apfs_container";
        callback(container);
    }

    int volumeIndex = 0;
    int fileIndex = 1000;
    std::unordered_set<uint64_t> seenVol;
    ApfsAccum apfs;
    bool blockUnread = false;
    bool catalogUnread = false;

    if (nx.size() >= 184 + 100 * 8) {
        for (int i = 0; i < 100; ++i) {
            if (isRunning && !(*isRunning)) break;
            uint64_t oid = readLe64(nx.data() + 184 + static_cast<size_t>(i) * 8);
            if (oid == 0 || oid >= blockCount) continue;
            tryApsbAt(reader, partitionOffsetBytes + oid * blockSize, static_cast<uint32_t>(blockSize),
                      sectorSize, volumeIndex, seenVol, callback, blockUnread);
        }
    }

    std::vector<uint8_t> block(static_cast<size_t>(blockSize));
    const uint64_t probe = std::min(blockCount, uint64_t{256});
    // ponytail: any unread cluster in the 256-block spray sets the sentinel
    // (including non-APSB data). Upgrade: only nx_fs_oid / APSB / omap targets.
    for (uint64_t i = 0; i < probe; ++i) {
        if (isRunning && !(*isRunning)) break;
        uint64_t off = partitionOffsetBytes + i * blockSize;
        if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) {
            // Spray of unknown clusters: missing APSB uses blockUnread only when
            // no volume was found. After a volume exists, unread is catalog/FN
            // honesty — not “no APSB”.
            if (seenVol.empty()) blockUnread = true;
            else catalogUnread = true;
            continue;
        }
        if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) == 0) {
            tryApsbAt(reader, off, static_cast<uint32_t>(blockSize), sectorSize, volumeIndex, seenVol,
                      callback, blockUnread);
        }
        collectCatalogNode(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                           sectorSize, apfs, false);
    }

    // nx_omap_oid is at NXSB+64; 0xA0 was the pre-spec probe offset — keep it
    // as a fallback so previously-carved images still resolve their omap.
    uint64_t omapOid = (nx.size() >= 0x48) ? readLe64(nx.data() + 0x40) : 0;
    if (omapOid == 0 || omapOid >= blockCount) {
        omapOid = (nx.size() >= 0xA8) ? readLe64(nx.data() + 0xA0) : 0;
    }
    uint64_t oidTreeOid = (nx.size() >= 0xB0) ? readLe64(nx.data() + 0xA8) : 0;
    auto walkBtreeRoot = [&](uint64_t rootOid, bool staleTree) {
        if (rootOid == 0 || rootOid >= blockCount) return;
        std::vector<uint8_t> rootBlk;
        if (!readBlock(reader, partitionOffsetBytes + rootOid * blockSize, static_cast<uint32_t>(blockSize), rootBlk)) {
            blockUnread = true;
            return;
        }
        uint64_t treeOid = rootOid;
        if (rootBlk.size() >= 56) {
            uint64_t named = readLe64(rootBlk.data() + 48);
            if (named > 0 && named < blockCount) treeOid = named;
        }
        std::unordered_map<uint64_t, uint64_t> oidMap;
        std::unordered_set<uint64_t> visitedOmap;
        std::vector<uint64_t> pending;
        walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount, treeOid, oidMap,
                      visitedOmap, blockUnread, pending);
        // Internal entries reference child oids; resolve them now that the
        // leaf map exists. Bounded retries — unresolvable children are lost.
        for (int round = 0; round < 4 && !pending.empty(); ++round) {
            std::vector<uint64_t> next;
            next.swap(pending);
            for (uint64_t c : next) {
                auto it = oidMap.find(c);
                if (it != oidMap.end())
                    walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount,
                                  it->second, oidMap, visitedOmap, blockUnread, pending);
                else
                    pending.push_back(c);
            }
        }
        for (const auto& kv : oidMap) {
            if (isRunning && !(*isRunning)) break;
            uint64_t paddr = kv.second;
            if (isRunning && !(*isRunning)) break;
            if (paddr < probe) continue;
            uint64_t off = partitionOffsetBytes + paddr * blockSize;
            if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) {
                blockUnread = true;
                continue;
            }
            if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) == 0) {
                tryApsbAt(reader, off, static_cast<uint32_t>(blockSize), sectorSize, volumeIndex, seenVol,
                          callback, blockUnread);
            }
            collectCatalogNode(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                               sectorSize, apfs, staleTree);
        }
    };
    walkBtreeRoot(omapOid, false);
    if (oidTreeOid != 0 && oidTreeOid != omapOid) walkBtreeRoot(oidTreeOid, false);

    // nx_xp_desc_blocks u32 @0x68, nx_xp_desc_base u64 @0x70 (apfs.h nx_superblock).
    const uint32_t descBlocks = (nx.size() >= 0x6C) ? readLe32(nx.data() + 0x68) : 0;
    const uint64_t descBase = (nx.size() >= 0x78) ? readLe64(nx.data() + 0x70) : 0;
    if (descBlocks > 0 && descBlocks <= 64 && descBase > 0 && descBase < blockCount) {
        for (uint32_t i = 0; i < descBlocks; ++i) {
            if (isRunning && !(*isRunning)) break;
            const uint64_t bno = descBase + i;
            if (bno >= blockCount) break;
            std::vector<uint8_t> desc;
            if (!readBlock(reader, partitionOffsetBytes + bno * blockSize,
                           static_cast<uint32_t>(blockSize), desc)) {
                blockUnread = true;
                continue;
            }
            if (desc.size() < 0xA8) continue;
            if (std::strncmp(reinterpret_cast<char*>(desc.data() + 32), "NXSB", 4) != 0) continue;
            uint64_t stale = readLe64(desc.data() + 0x40);
            if (stale == 0 || stale >= blockCount) stale = readLe64(desc.data() + 0xA0);
            if (stale == 0 || stale >= blockCount || stale == omapOid) continue;
            walkBtreeRoot(stale, true);
        }
    }

    flushApfsCatalog(apfs, fileIndex, callback);

    if (nxUnread) emitNxsbUnread(callback, partitionOffsetBytes, sectorSize);
    if (blockUnread) emitBlockUnread(callback, partitionOffsetBytes, sectorSize);
    if (catalogUnread) emitCatalogUnread(callback, partitionOffsetBytes, sectorSize);

    FileRecord tick;
    tick.id = -1;
    tick.startSector = (partitionOffsetBytes + blockCount * blockSize) / sectorSize;
    callback(tick);
    return true;
}

} // namespace byteback
