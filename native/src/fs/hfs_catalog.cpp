#include "fs/hfs_catalog.h"
#include "byteback_fs.h"
#include "forensic/audit_logger.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace byteback {

namespace {

constexpr size_t kHfsCatalogForkOff = 272;
constexpr size_t kHfsExtentsForkOff = 192;
constexpr uint16_t kHfsDataForkType = 0;
constexpr uint16_t kHfsResourceForkType = 0xFF;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

int64_t hfsDateToUnix(uint32_t hfs) {
    constexpr uint32_t kHfsUnixDelta = 2082844800u;
    if (hfs < kHfsUnixDelta) return 0;
    return static_cast<int64_t>(hfs - kHfsUnixDelta);
}

struct HfsExtent {
    uint32_t startBlock = 0;
    uint32_t blockCount = 0;
};

struct HfsFork {
    uint64_t logicalSize = 0;
    std::vector<HfsExtent> extents;
};

bool readAt(DiskReader& reader, uint64_t offset, uint32_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    auto res = reader.readSectors(offset, size, out.data());
    return readComplete(res, size);
}

bool parseFork(const uint8_t* data, HfsFork& fork) {
    fork.logicalSize = be64(data);
    fork.extents.clear();
    const uint8_t* ext = data + 16;
    // HFSPlusExtentDescriptor is 8 bytes (startBlock u32 + blockCount u32);
    // HFSPlusForkData holds 8 of them (80 bytes total).
    for (int i = 0; i < 8; ++i) {
        HfsExtent e;
        e.startBlock = be32(ext + i * 8);
        e.blockCount = be32(ext + i * 8 + 4);
        if (e.blockCount > 0) fork.extents.push_back(e);
    }
    return !fork.extents.empty() || fork.logicalSize > 0;
}

std::string utf16BeToUtf8(const uint8_t* data, uint16_t charCount) {
    std::string out;
    out.reserve(charCount);
    for (uint16_t i = 0; i < charCount; ++i) {
        uint16_t ch = be16(data + i * 2);
        if (ch < 0x80) out.push_back(static_cast<char>(ch));
        else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

void forkToRuns(const HfsFork& fork, uint32_t blockSize, uint64_t partitionOffsetBytes,
                uint32_t sectorSize, std::vector<FileRecord::DataRun>& runs) {
    if (sectorSize == 0) sectorSize = 512;
    for (const auto& e : fork.extents) {
        if (e.blockCount == 0) continue;
        uint64_t byteOff = partitionOffsetBytes + static_cast<uint64_t>(e.startBlock) * blockSize;
        uint64_t startSector = byteOff / sectorSize;
        uint64_t sectorCount = (static_cast<uint64_t>(e.blockCount) * blockSize + sectorSize - 1) / sectorSize;
        if (sectorCount == 0) sectorCount = 1;
        runs.push_back({startSector, sectorCount});
    }
}

struct CatalogCtx {
    DiskReader* reader = nullptr;
    uint64_t partitionOffset = 0;
    uint32_t blockSize = 4096;
    uint32_t sectorSize = 512;
    FileSystemParser::FileRecordCallback* callback = nullptr;
    std::atomic<bool>* isRunning = nullptr;
    std::unordered_map<uint32_t, std::string> paths;
    std::unordered_set<uint32_t> seenFileIds;
    HfsFork extentsFileFork;
    bool hasExtentsFile = false;
    int fileCount = 0;
    int maxFiles = 0;
    bool emittedLimit = false;
    bool catalogUnread = false;
    bool emittedVolName = false;
    // ponytail: hard node-visit budget instead of a visited set; a crafted
    // B-tree can otherwise fan out exponentially within the depth cap.
    size_t nodesVisited = 0;
};

constexpr size_t kHfsMaxNodeVisits = 1u << 20;

bool walkExtentNode(CatalogCtx& ctx, uint32_t block, int depth,
                    uint32_t fileId, uint16_t forkType, HfsFork& fork);

bool appendOverflowExtents(CatalogCtx& ctx, uint32_t fileId, uint16_t forkType, HfsFork& fork) {
    if (!ctx.hasExtentsFile || ctx.extentsFileFork.extents.empty()) return false;
    uint64_t mapped = 0;
    for (const auto& e : fork.extents) mapped += static_cast<uint64_t>(e.blockCount) * ctx.blockSize;
    if (fork.logicalSize <= mapped) return false;
    if (fork.extents.size() < 8) return false;
    if (fork.extents.back().blockCount == 0) return false;

    HfsFork extra;
    uint32_t root = ctx.extentsFileFork.extents.front().startBlock;
    if (!walkExtentNode(ctx, root, 0, fileId, forkType, extra)) return false;
    for (const auto& e : extra.extents) {
        if (e.blockCount > 0) fork.extents.push_back(e);
    }
    return !extra.extents.empty();
}

bool parseExtentRecord(const uint8_t* rec, uint16_t recLen,
                       uint32_t fileId, uint16_t forkType, HfsFork& fork) {
    if (recLen < 8) return true;
    uint16_t keyLen = be16(rec);
    if (keyLen < 6 || keyLen + 2 > recLen) return true;
    // HFSPlusExtentKey: forkType u8, pad u8, fileID u32, startBlock u32.
    uint8_t recFork = rec[2];
    uint32_t recFile = be32(rec + 4);
    if (recFile != fileId || recFork != (forkType & 0xFF)) return true;
    const uint8_t* val = rec + 2 + keyLen;
    uint16_t valLen = recLen - (2 + keyLen);
    if (valLen < 64) return true; // HFSPlusExtentRecord = 8 descriptors x 8 bytes
    for (int i = 0; i < 8; ++i) {
        HfsExtent e;
        e.startBlock = be32(val + i * 8);
        e.blockCount = be32(val + i * 8 + 4);
        if (e.blockCount > 0) fork.extents.push_back(e);
    }
    return true;
}

bool walkExtentNode(CatalogCtx& ctx, uint32_t block, int depth,
                    uint32_t fileId, uint16_t forkType, HfsFork& fork) {
    if (ctx.isRunning && !(*ctx.isRunning)) return false;
    if (depth > 24) return true;
    if (++ctx.nodesVisited > kHfsMaxNodeVisits) return false;

    std::vector<uint8_t> node;
    uint64_t off = ctx.partitionOffset + static_cast<uint64_t>(block) * ctx.blockSize;
    if (!readAt(*ctx.reader, off, ctx.blockSize, node)) {
        ctx.catalogUnread = true;
        return true;
    }

    uint8_t kind = node[8];  // BTNodeDescriptor: forwardLink(4) backLink(4) kind(1) height(1) numRecords(2)
    uint16_t numRecords = be16(node.data() + 10);
    if (numRecords == 0) return true;
    if (!hfsOffsetTableFits(ctx.blockSize, numRecords)) return true;

    std::vector<uint16_t> offsets(numRecords + 1);
    size_t offTable = ctx.blockSize - static_cast<size_t>(numRecords + 1) * 2;
    for (uint16_t i = 0; i <= numRecords; ++i) {
        offsets[i] = be16(node.data() + offTable + i * 2);
    }

    if (kind == 0xFF) {
        for (uint16_t i = 0; i < numRecords; ++i) {
            uint16_t start = offsets[i];
            uint16_t end = offsets[i + 1];
            if (end <= start || end > ctx.blockSize) continue;
            if (!parseExtentRecord(node.data() + start, static_cast<uint16_t>(end - start),
                                   fileId, forkType, fork)) {
                return false;
            }
        }
        return true;
    }

    if (kind == 0) {
        for (uint16_t i = 0; i < numRecords; ++i) {
            uint16_t start = offsets[i];
            uint16_t end = offsets[i + 1];
            if (end <= start + 4 || end > ctx.blockSize) continue;
            uint32_t child = be32(node.data() + end - 4);
            if (!walkExtentNode(ctx, child, depth + 1, fileId, forkType, fork)) return false;
        }
    }
    return true;
}

bool walkCatalogNode(CatalogCtx& ctx, uint32_t block, int depth);

bool parseCatalogRecord(CatalogCtx& ctx, const uint8_t* rec, uint16_t recLen, bool unused = false,
                        bool fromJournal = false) {
    if (recLen < 8) return true;
    uint16_t keyLen = be16(rec);
    if (keyLen < 6 || keyLen + 2 > recLen) return true;
    uint32_t parentId = be32(rec + 2);
    uint16_t nameLen = be16(rec + 6);
    if (6 + nameLen > keyLen || 6 + nameLen > recLen) return true;
    std::string name = utf16BeToUtf8(rec + 8, static_cast<uint16_t>(nameLen / 2));
    const uint8_t* val = rec + 2 + keyLen;
    uint16_t valLen = recLen - (2 + keyLen);
    if (valLen < 2) return true;
    uint16_t recType = be16(val);
    if (recType != 1 && recType != 2 && recType != 4) return true;
    if (unused && recType != 2) return true;
    if (ctx.maxFiles > 0 && recType == 2 && ctx.fileCount >= ctx.maxFiles) {
        if (!ctx.emittedLimit && ctx.callback) {
            FileRecord sentinel{};
            sentinel.id = -1;
            sentinel.parentId = -1;
            sentinel.name = "[HFS] catalog truncated at " + std::to_string(ctx.maxFiles);
            sentinel.path = "/";
            sentinel.source = "hfs_limit";
            sentinel.category = "System";
            sentinel.status = 1;
            sentinel.confidence = 100;
            (*ctx.callback)(sentinel);
            ctx.emittedLimit = true;
            forensic::AuditLogger::GetInstance().LogEvent(
                "HFS_CATALOG_TRUNCATED | count=" + std::to_string(ctx.fileCount) +
                " | max=" + std::to_string(ctx.maxFiles));
        }
        return false;
    }

    std::string parentPath = "/";
    auto pit = ctx.paths.find(parentId);
    if (pit != ctx.paths.end()) parentPath = pit->second;
    std::string fullPath = parentPath;
    if (!fullPath.empty() && fullPath.back() != '/') fullPath += '/';
    fullPath += name;

    if (recType == 1) {
        // Folder thread record: the key's "parent" IS the folder CNID and the
        // value carries the real parent + name. Register the folder's path
        // from the value so files key'd under it resolve.
        if (valLen < 8 + 2) return true;
        uint32_t realParent = be32(val + 4);
        uint16_t vNameLen = be16(val + 8);
        if (10 + vNameLen > valLen || vNameLen == 0) return true;
        std::string vName = utf16BeToUtf8(val + 10, static_cast<uint16_t>(vNameLen / 2));
        if (vName.empty()) return true;
        std::string vp = ctx.paths.count(realParent) ? ctx.paths[realParent] : "/";
        if (!vp.empty() && vp.back() != '/') vp += '/';
        ctx.paths[parentId] = vp + vName; // parentId here = folder CNID from the key
        return true;
    }

    // HFSPlusCatalogFolder (4) / HFSPlusCatalogFile (2): recordType(2)
    // reserved(2) flags(4) reserved(4) fileID u32 @12.
    if (valLen < 16) return true;

    if (recType == 4) {
        ctx.paths[be32(val + 12)] = fullPath;
        if (parentId == 1 && !name.empty() && !ctx.emittedVolName && ctx.callback) {
            FileRecord fr;
            fr.id = -1;
            fr.parentId = 1;
            fr.name = name;
            fr.path = "/";
            fr.status = 1;
            fr.confidence = 5;
            fr.category = "System";
            fr.source = kHfsVolNameSource;
            (*ctx.callback)(fr);
            ctx.emittedVolName = true;
        }
        return true;
    }

    // File record — data fork at offset 92 (0x5C), resource fork at 172.
    if (valLen < 92 + 80) return true;
    if (name.empty()) return true;
    uint32_t fileId = be32(val + 12);
    if (fileId == 0 || ctx.seenFileIds.count(fileId)) return true;
    HfsFork dataFork;
    const bool haveData = parseFork(val + 92, dataFork);
    HfsFork rsrcFork;
    const bool haveRsrc = valLen >= 92 + 80 + 80 && parseFork(val + 172, rsrcFork);
    if (!haveData && !haveRsrc) return true;
    const int64_t createdAt = (valLen >= 20) ? hfsDateToUnix(be32(val + 16)) : 0;
    const int64_t modifiedAt = (valLen >= 24) ? hfsDateToUnix(be32(val + 20)) : 0;

    auto emitFork = [&](HfsFork& fork, uint16_t forkType, const std::string& recName,
                        const std::string& recPath) {
        if (ctx.maxFiles > 0 && ctx.fileCount >= ctx.maxFiles) return;
        appendOverflowExtents(ctx, fileId, forkType, fork);
        FileRecord fr;
        fr.id = -1;
        fr.parentId = parentId;
        fr.name = recName;
        auto dot = recName.find_last_of('.');
        fr.extension = (dot != std::string::npos && dot + 1 < recName.size())
            ? recName.substr(dot + 1) : "";
        fr.path = recPath;
        fr.sizeBytes = fork.logicalSize;
        forkToRuns(fork, ctx.blockSize, ctx.partitionOffset, ctx.sectorSize, fr.runs);
        if (!fr.runs.empty()) {
            fr.startSector = fr.runs.front().startSector;
            uint64_t end = fr.startSector;
            for (const auto& r : fr.runs) end = std::max(end, r.startSector + r.sectorCount);
            fr.endSector = end;
        }
        if (fromJournal) {
            fr.status = 0;
            fr.confidence = 50;
            fr.source = kHfsJournalSource;
        } else if (unused) {
            fr.status = 0;
            fr.confidence = 48;
            fr.source = kHfsCatalogUnusedSource;
        } else if (fr.runs.empty() && fork.logicalSize > 0) {
            fr.status = 0;
            fr.confidence = 30;
            fr.source = "hfs_catalog";
        } else {
            fr.status = 1;
            fr.confidence = 85;
            fr.source = "hfs_catalog";
        }
        fr.category = "File";
        fr.createdAt = createdAt;
        fr.modifiedAt = modifiedAt;
        (*ctx.callback)(fr);
        ++ctx.fileCount;
    };

    if (haveData) emitFork(dataFork, kHfsDataForkType, name, fullPath);
    if (haveRsrc) emitFork(rsrcFork, kHfsResourceForkType, name + ".rsrc", fullPath + ".rsrc");
    ctx.seenFileIds.insert(fileId);
    return true;
}

bool scanUnusedLeafSlack(CatalogCtx& ctx, const uint8_t* node, uint32_t blockSize,
                         uint16_t numRecords, const std::vector<uint16_t>& offsets) {
    const size_t offTable = blockSize - static_cast<size_t>(numRecords + 1) * 2;
    size_t usedEnd = 14;
    for (uint16_t off : offsets) {
        if (off > usedEnd && off <= offTable) usedEnd = off;
    }
    size_t p = usedEnd;
    while (p + 10 < offTable) {
        const uint8_t* rec = node + p;
        uint16_t keyLen = be16(rec);
        const size_t recLen = static_cast<size_t>(2) + keyLen + 92 + 80;
        if (keyLen < 6 || p + recLen > offTable) {
            p += 2;
            continue;
        }
        if (be16(rec + 2 + keyLen) != 2) {
            p += 2;
            continue;
        }
        if (!parseCatalogRecord(ctx, rec, static_cast<uint16_t>(recLen), true)) return false;
        p += recLen;
        if (p & 1) ++p;
    }
    return true;
}

bool walkCatalogNode(CatalogCtx& ctx, uint32_t block, int depth) {
    if (ctx.isRunning && !(*ctx.isRunning)) return false;
    if (depth > 24) return true;
    if (++ctx.nodesVisited > kHfsMaxNodeVisits) return false;

    std::vector<uint8_t> node;
    uint64_t off = ctx.partitionOffset + static_cast<uint64_t>(block) * ctx.blockSize;
    if (!readAt(*ctx.reader, off, ctx.blockSize, node)) {
        ctx.catalogUnread = true;
        return true;
    }

    uint8_t kind = node[8];  // BTNodeDescriptor: kind at 8, numRecords at 10
    uint16_t numRecords = be16(node.data() + 10);
    if (!hfsOffsetTableFits(ctx.blockSize, numRecords)) return true;

    std::vector<uint16_t> offsets(numRecords + 1);
    size_t offTable = ctx.blockSize - static_cast<size_t>(numRecords + 1) * 2;
    for (uint16_t i = 0; i <= numRecords; ++i) {
        offsets[i] = be16(node.data() + offTable + i * 2);
    }

    if (kind == 0xFF) {
        for (uint16_t i = 0; i < numRecords; ++i) {
            uint16_t start = offsets[i];
            uint16_t end = offsets[i + 1];
            if (end <= start || end > ctx.blockSize) continue;
            if (!parseCatalogRecord(ctx, node.data() + start, static_cast<uint16_t>(end - start))) return false;
        }
        return scanUnusedLeafSlack(ctx, node.data(), ctx.blockSize, numRecords, offsets);
    }

    if (kind == 0) {
        for (uint16_t i = 0; i < numRecords; ++i) {
            uint16_t start = offsets[i];
            uint16_t end = offsets[i + 1];
            if (end <= start + 4 || end > ctx.blockSize) continue;
            uint32_t child = be32(node.data() + end - 4);
            if (!walkCatalogNode(ctx, child, depth + 1)) return false;
        }
    }
    return true;
}

uint16_t jnl16(const uint8_t* p, bool be) {
    return be ? be16(p) : static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t jnl32(const uint8_t* p, bool be) {
    return be ? be32(p)
              : static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t jnl64(const uint8_t* p, bool be) {
    if (be) return be64(p);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t appleJournalCksum(const uint8_t* p, uint32_t n, uint32_t ckOff) {
    uint32_t cksum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t b = (i >= ckOff && i < ckOff + 4) ? 0 : p[i];
        cksum = (cksum << 8) ^ (cksum + b);
    }
    return ~cksum;
}

bool readJournalSlice(DiskReader& reader, uint64_t journalAbs, uint64_t jSize, uint32_t jhdrSize,
                      uint64_t pos, uint32_t n, std::vector<uint8_t>& out) {
    if (n == 0 || jSize <= jhdrSize) return false;
    out.assign(n, 0);
    uint32_t got = 0;
    while (got < n) {
        if (pos >= jSize) {
            if (pos < jhdrSize) return false;
            pos = jhdrSize + (pos - jSize);
        }
        if (pos < jhdrSize || pos >= jSize) return false;
        const uint64_t room = jSize - pos;
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(room, n - got));
        std::vector<uint8_t> part;
        if (!readAt(reader, journalAbs + pos, chunk, part)) return false;
        std::memcpy(out.data() + got, part.data(), chunk);
        got += chunk;
        pos += chunk;
    }
    return true;
}

void parseJournalCatalogNode(CatalogCtx& ctx, const uint8_t* node, uint32_t nodeSize) {
    if (nodeSize < 14 || node[8] != 0xFF) return;
    const uint16_t numRecords = be16(node + 10);
    if (!hfsOffsetTableFits(nodeSize, numRecords)) return;
    std::vector<uint16_t> offsets(numRecords + 1);
    const size_t offTable = nodeSize - static_cast<size_t>(numRecords + 1) * 2;
    for (uint16_t i = 0; i <= numRecords; ++i) {
        offsets[i] = be16(node + offTable + i * 2);
    }
    for (uint16_t i = 0; i < numRecords; ++i) {
        const uint16_t start = offsets[i];
        const uint16_t end = offsets[i + 1];
        if (end <= start || end > nodeSize) continue;
        parseCatalogRecord(ctx, node + start, static_cast<uint16_t>(end - start), false, true);
    }
}

void replayHfsJournal(CatalogCtx& ctx, const uint8_t* hdrSector) {
    constexpr uint32_t kJournaledMask = 0x00002000;
    constexpr uint32_t kJiInFs = 0x00000001;
    constexpr uint32_t kJiOtherDev = 0x00000002;
    constexpr uint32_t kJiNeedInit = 0x00000004;
    constexpr uint32_t kJnlMagic = 0x4A4E4C78;
    constexpr uint32_t kJnlEndian = 0x12345678;
    constexpr uint64_t kMaxJournal = 16ull * 1024 * 1024;
    constexpr int kMaxTxn = 64;

    if ((be32(hdrSector + 4) & kJournaledMask) == 0) return;
    const uint32_t jibBlock = be32(hdrSector + 12);
    if (jibBlock == 0) return;

    std::vector<uint8_t> jib;
    const uint64_t jibOff = ctx.partitionOffset + static_cast<uint64_t>(jibBlock) * ctx.blockSize;
    if (!readAt(*ctx.reader, jibOff, 512, jib)) return;
    const uint32_t flags = be32(jib.data());
    if ((flags & kJiInFs) == 0 || (flags & kJiOtherDev) != 0 || (flags & kJiNeedInit) != 0) return;
    const uint64_t jRel = be64(jib.data() + 36);
    const uint64_t jSize = be64(jib.data() + 44);
    if (jSize < 512 || jSize > kMaxJournal) return;

    std::vector<uint8_t> hdr;
    const uint64_t journalAbs = ctx.partitionOffset + jRel;
    if (!readAt(*ctx.reader, journalAbs, 512, hdr)) return;

    bool be = true;
    if (be32(hdr.data()) == kJnlMagic && be32(hdr.data() + 4) == kJnlEndian) be = true;
    else if (jnl32(hdr.data(), false) == kJnlMagic && jnl32(hdr.data() + 4, false) == kJnlEndian) be = false;
    else return;

    const uint64_t start = jnl64(hdr.data() + 8, be);
    const uint64_t end = jnl64(hdr.data() + 16, be);
    const uint64_t size = jnl64(hdr.data() + 24, be);
    const uint32_t blhdrSize = jnl32(hdr.data() + 32, be);
    const uint32_t jhdrSize = jnl32(hdr.data() + 40, be);
    if (size != jSize || jhdrSize < 44 || jhdrSize > 4096 || jhdrSize > size) return;
    if (blhdrSize < 48 || blhdrSize > 4096 || blhdrSize > size - jhdrSize) return;
    if (jnl32(hdr.data() + 36, be) != appleJournalCksum(hdr.data(), jhdrSize, 36)) return;
    if (start == end) return;
    if (start < jhdrSize || start >= size || end < jhdrSize || end > size) return;

    uint64_t pos = start;
    for (int txn = 0; txn < kMaxTxn && pos != end; ++txn) {
        if (ctx.isRunning && !(*ctx.isRunning)) return;
        std::vector<uint8_t> bl;
        if (!readJournalSlice(*ctx.reader, journalAbs, size, jhdrSize, pos, blhdrSize, bl)) return;
        if (jnl32(bl.data() + 8, be) != appleJournalCksum(bl.data(), blhdrSize, 8)) return;
        const uint16_t maxBlocks = jnl16(bl.data(), be);
        const uint16_t numBlocks = jnl16(bl.data() + 2, be);
        const uint32_t bytesUsed = jnl32(bl.data() + 4, be);
        if (maxBlocks < 2 || numBlocks < 2 || numBlocks > maxBlocks) return;
        if (static_cast<uint32_t>(16 + numBlocks * 16) > blhdrSize) return;
        if (bytesUsed < blhdrSize) return;

        uint64_t dataPos = pos + blhdrSize;
        for (uint16_t i = 1; i < numBlocks; ++i) {
            const uint8_t* info = bl.data() + 16 + i * 16;
            const uint32_t bsize = jnl32(info + 8, be);
            if (bsize == 0 || bsize > 65536) return;
            std::vector<uint8_t> block;
            if (!readJournalSlice(*ctx.reader, journalAbs, size, jhdrSize, dataPos, bsize, block)) return;
            parseJournalCatalogNode(ctx, block.data(), bsize);
            dataPos += bsize;
        }

        const uint64_t nextBl = jnl64(bl.data() + 16, be);
        if (nextBl != 0) {
            pos = nextBl;
        } else {
            pos += bytesUsed;
            if (pos >= size) pos = jhdrSize + (pos - size);
        }
    }
}

} // namespace

bool scanHfsPlusCatalog(DiskReader& reader, uint64_t partitionOffsetBytes,
                        uint64_t partitionSizeBytes,
                        FileSystemParser::FileRecordCallback callback,
                        std::atomic<bool>* isRunning,
                        int maxFiles) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> hdrSector;
    auto isHfsMagic = [](const std::vector<uint8_t>& s) {
        if (s.size() < 2) return false;
        const uint16_t m = be16(s.data());
        return m == 0x482B || m == 0x5848;
    };
    const bool primaryOk = readAt(reader, partitionOffsetBytes + 1024, 512, hdrSector) && isHfsMagic(hdrSector);
    if (!primaryOk) {
        uint64_t volBytes = partitionSizeBytes;
        if (volBytes == 0) volBytes = reader.getDiskSize();
        if (volBytes < 1024 + 512) return false;
        if (!readAt(reader, partitionOffsetBytes + volBytes - 1024, 512, hdrSector)) return false;
        if (!isHfsMagic(hdrSector)) return false;
    }

    uint32_t blockSize = be32(hdrSector.data() + 40);
    if (blockSize < 512 || blockSize > 65536) blockSize = 4096;

    HfsFork catalogFork;
    if (!parseFork(hdrSector.data() + kHfsCatalogForkOff, catalogFork)) return false;
    if (catalogFork.extents.empty()) return false;

    CatalogCtx ctx;
    ctx.reader = &reader;
    ctx.partitionOffset = partitionOffsetBytes;
    ctx.blockSize = blockSize;
    ctx.sectorSize = sectorSize;
    ctx.callback = &callback;
    ctx.isRunning = isRunning;
    ctx.maxFiles = maxFiles;
    ctx.paths[2] = "/";

    HfsFork extentsFork;
    if (parseFork(hdrSector.data() + kHfsExtentsForkOff, extentsFork) && !extentsFork.extents.empty()) {
        ctx.hasExtentsFile = true;
        ctx.extentsFileFork = std::move(extentsFork);
    }

    uint32_t rootBlock = catalogFork.extents.front().startBlock;
    const bool ok = walkCatalogNode(ctx, rootBlock, 0);
    replayHfsJournal(ctx, hdrSector.data());
    if (ctx.catalogUnread && ctx.callback) {
        FileRecord sentinel{};
        sentinel.id = -1;
        sentinel.parentId = -1;
        sentinel.name = "Hfs_CatalogUnread";
        sentinel.path = kHfsCatalogUnreadPath;
        sentinel.source = kHfsCatalogUnreadSource;
        sentinel.category = "System";
        sentinel.status = 0;
        sentinel.confidence = 20;
        const uint64_t byteOff = ctx.partitionOffset + static_cast<uint64_t>(rootBlock) * ctx.blockSize;
        sentinel.startSector = byteOff / ctx.sectorSize;
        const uint64_t nsec = (static_cast<uint64_t>(ctx.blockSize) + ctx.sectorSize - 1) / ctx.sectorSize;
        sentinel.endSector = sentinel.startSector + (nsec == 0 ? 1 : nsec);
        (*ctx.callback)(sentinel);
    }
    return ok;
}

} // namespace byteback
