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
constexpr uint32_t kObjBtreeNode = 2;

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

void scanBtreeNodeForCatalog(const uint8_t* block, uint32_t blockSize, uint64_t partitionOffsetBytes,
                             uint64_t blockOff, uint32_t sectorSize, int& fileIndex,
                             FileSystemParser::FileRecordCallback& callback,
                             std::unordered_set<std::string>& seenNames,
                             const char* source) {
    if (blockSize < 64) return;
    uint32_t otype = readLe32(block + 24) & 0xFFFFu;
    if (otype != kObjBtreeNode) return;
    uint16_t level = static_cast<uint16_t>(block[34] | (block[35] << 8));
    if (level != 0) return;
    uint32_t nkeys = readLe32(block + 36);
    if (nkeys == 0 || nkeys > 4096) return;

    struct DirHit {
        uint64_t oid = 0;
        std::string name;
    };
    std::vector<DirHit> dirs;
    std::unordered_map<uint64_t, std::vector<FileRecord::DataRun>> extents;
    struct InodeTimes {
        int64_t created = 0;
        int64_t modified = 0;
    };
    std::unordered_map<uint64_t, InodeTimes> times;

    for (uint32_t i = 56; i + 16 < blockSize; i += 8) {
        uint64_t hdr = readLe64(block + i);
        uint64_t typ = hdr >> 48;
        uint64_t oid = hdr & ((1ull << 48) - 1);
        if (typ == kApfsTypeDirRec) {
            uint32_t nlh = readLe32(block + i + 8);
            uint32_t nlen = nlh & 0x3FFu;
            if (nlen < 2 || nlen > 255 || i + 12 + nlen > blockSize) continue;
            if (block[i + 12 + nlen - 1] != 0) continue;
            std::string name(reinterpret_cast<const char*>(block + i + 12), nlen - 1);
            if (name.empty() || seenNames.count(name)) continue;
            dirs.push_back({oid, name});
        } else if (typ == kApfsTypeFileExtent && i + 24 < blockSize) {
            uint64_t lenFlags = readLe64(block + i + 8);
            uint64_t len = lenFlags & ((1ull << 56) - 1);
            uint64_t phys = readLe64(block + i + 16);
            if (phys == 0 || len == 0) continue;
            FileRecord::DataRun run;
            run.startSector = (partitionOffsetBytes + phys * blockSize) / sectorSize;
            run.sectorCount = (len + sectorSize - 1) / sectorSize;
            if (run.sectorCount == 0) run.sectorCount = 1;
            extents[oid].push_back(run);
        } else if (typ == kApfsTypeInode && i + 40 <= blockSize) {
            InodeTimes t;
            const uint64_t cns = readLe64(block + i + 24);
            const uint64_t mns = readLe64(block + i + 32);
            if (cns >= 1000000000ull) t.created = static_cast<int64_t>(cns / 1000000000ull);
            if (mns >= 1000000000ull) t.modified = static_cast<int64_t>(mns / 1000000000ull);
            if (t.created != 0 || t.modified != 0) times[oid] = t;
        }
    }

    for (const auto& d : dirs) {
        seenNames.insert(d.name);
        FileRecord fr;
        fr.id = fileIndex++;
        fr.name = d.name;
        fr.path = "/apfs/" + d.name;
        fr.startSector = blockOff / sectorSize;
        fr.endSector = fr.startSector + 1;
        fr.status = 0;
        fr.confidence = 70;
        fr.category = "Document";
        fr.source = source ? source : "apfs_file";
        if (fr.source == kApfsOmapDeletedSource) fr.confidence = 48;
        auto tt = times.find(d.oid);
        if (tt != times.end()) {
            fr.createdAt = tt->second.created;
            fr.modifiedAt = tt->second.modified;
        }
        auto it = extents.find(d.oid);
        if (it != extents.end() && !it->second.empty()) {
            fr.runs = it->second;
            fr.startSector = fr.runs.front().startSector;
            uint64_t total = 0;
            for (const auto& r : fr.runs) total += r.sectorCount;
            fr.endSector = fr.startSector + total;
            uint64_t bytes = 0;
            for (const auto& r : fr.runs) bytes += static_cast<uint64_t>(r.sectorCount) * sectorSize;
            fr.sizeBytes = bytes;
            fr.confidence = 85;
        }
        callback(fr);
    }

    for (const auto& kv : extents) {
        bool attached = false;
        for (const auto& d : dirs) {
            if (d.oid == kv.first) {
                attached = true;
                break;
            }
        }
        if (attached) continue;
        const auto& runs = kv.second;
        if (runs.empty()) continue;
        FileRecord fr;
        fr.id = fileIndex++;
        fr.name = "extent_" + std::to_string(kv.first);
        fr.path = "/apfs/extent/" + std::to_string(kv.first);
        fr.sizeBytes = 0;
        for (const auto& r : runs) fr.sizeBytes += static_cast<uint64_t>(r.sectorCount) * sectorSize;
        fr.runs = runs;
        fr.startSector = runs.front().startSector;
        uint64_t total = 0;
        for (const auto& r : runs) total += r.sectorCount;
        fr.endSector = fr.startSector + total;
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

void walkOmapBtree(DiskReader& reader, uint64_t partitionOffsetBytes, uint64_t blockSize,
                   uint64_t blockCount, uint64_t blockNum, std::vector<uint64_t>& paddrs,
                   std::unordered_set<uint64_t>& visited, bool& unread) {
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
    uint16_t level = static_cast<uint16_t>(blk[34] | (blk[35] << 8));
    uint32_t nkeys = readLe32(blk.data() + 36);
    if (nkeys == 0 || nkeys > 4096) return;
    for (uint32_t i = 0; i < nkeys; ++i) {
        uint32_t off = 56 + i * 24;
        if (off + 24 > blockSize) break;
        uint64_t val = readLe64(blk.data() + off + 16);
        if (val == 0 || val >= blockCount) continue;
        if (level == 0) {
            paddrs.push_back(val);
        } else {
            walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount, val, paddrs, visited, unread);
        }
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
    for (uint32_t i = 0; i < nkeys; ++i) {
        uint32_t off = 56 + i * 24;
        if (off + 24 > blockSize) break;
        uint64_t paddr = readLe64(block + off + 16);
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
    std::unordered_set<std::string> seenNames;
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
        scanBtreeNodeForCatalog(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                                off, sectorSize, fileIndex, callback, seenNames, "apfs_file");
    }

    // nx_omap_oid is at NXSB+64; 0xA0 was the pre-spec probe offset — keep it
    // as a fallback so previously-carved images still resolve their omap.
    uint64_t omapOid = (nx.size() >= 0x48) ? readLe64(nx.data() + 0x40) : 0;
    if (omapOid == 0 || omapOid >= blockCount) {
        omapOid = (nx.size() >= 0xA8) ? readLe64(nx.data() + 0xA0) : 0;
    }
    uint64_t oidTreeOid = (nx.size() >= 0xB0) ? readLe64(nx.data() + 0xA8) : 0;
    auto walkBtreeRoot = [&](uint64_t rootOid, const char* catSource) {
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
        std::vector<uint64_t> paddrs;
        std::unordered_set<uint64_t> visitedOmap;
        walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount, treeOid, paddrs, visitedOmap,
                      blockUnread);
        for (uint64_t paddr : paddrs) {
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
            scanBtreeNodeForCatalog(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                                    off, sectorSize, fileIndex, callback, seenNames, catSource);
        }
    };
    walkBtreeRoot(omapOid, "apfs_file");
    if (oidTreeOid != 0 && oidTreeOid != omapOid) walkBtreeRoot(oidTreeOid, "apfs_file");

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
            walkBtreeRoot(stale, kApfsOmapDeletedSource);
        }
    }

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
