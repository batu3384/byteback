#include "fs/hfs_catalog.h"
#include "byteback_fs.h"
#include "forensic/audit_logger.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
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
    return res.success && res.bytesRead >= size;
}

bool parseFork(const uint8_t* data, HfsFork& fork) {
    fork.logicalSize = be64(data);
    fork.extents.clear();
    const uint8_t* ext = data + 16;
    for (int i = 0; i < 8; ++i) {
        HfsExtent e;
        e.startBlock = be32(ext + i * 12);
        e.blockCount = be32(ext + i * 12 + 4);
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
    HfsFork extentsFileFork;
    bool hasExtentsFile = false;
    int fileCount = 0;
    int maxFiles = 0;
    bool emittedLimit = false;
};

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
    uint16_t recFork = be16(rec + 2);
    uint32_t recFile = be32(rec + 4);
    if (recFile != fileId || recFork != forkType) return true;
    const uint8_t* val = rec + 2 + keyLen;
    uint16_t valLen = recLen - (2 + keyLen);
    if (valLen < 96) return true;
    for (int i = 0; i < 8; ++i) {
        HfsExtent e;
        e.startBlock = be32(val + i * 12);
        e.blockCount = be32(val + i * 12 + 4);
        if (e.blockCount > 0) fork.extents.push_back(e);
    }
    return true;
}

bool walkExtentNode(CatalogCtx& ctx, uint32_t block, int depth,
                    uint32_t fileId, uint16_t forkType, HfsFork& fork) {
    if (ctx.isRunning && !(*ctx.isRunning)) return false;
    if (depth > 24) return true;

    std::vector<uint8_t> node;
    uint64_t off = ctx.partitionOffset + static_cast<uint64_t>(block) * ctx.blockSize;
    if (!readAt(*ctx.reader, off, ctx.blockSize, node)) return true;

    uint8_t kind = node[0];
    uint16_t numRecords = be16(node.data() + 4);
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

bool parseCatalogRecord(CatalogCtx& ctx, const uint8_t* rec, uint16_t recLen) {
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
    if (recType != 1 && recType != 2) return true;
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
        ctx.paths[be32(val + 8)] = fullPath;
        return true;
    }

    // File record — data fork at offset 0x50 from value start (HFSPlusCatalogFile).
    if (valLen < 0x50 + 80) return true;
    uint32_t fileId = be32(val + 8);
    HfsFork dataFork;
    if (!parseFork(val + 0x50, dataFork)) return true;
    appendOverflowExtents(ctx, fileId, kHfsDataForkType, dataFork);

    FileRecord fr;
    fr.id = -1;
    fr.parentId = parentId;
    fr.name = name;
    auto dot = name.find_last_of('.');
    fr.extension = (dot != std::string::npos) ? name.substr(dot + 1) : "";
    fr.path = fullPath;
    fr.sizeBytes = dataFork.logicalSize;
    forkToRuns(dataFork, ctx.blockSize, ctx.partitionOffset, ctx.sectorSize, fr.runs);
    if (!fr.runs.empty()) {
        fr.startSector = fr.runs.front().startSector;
        uint64_t end = fr.startSector;
        for (const auto& r : fr.runs) end = std::max(end, r.startSector + r.sectorCount);
        fr.endSector = end;
    }
    fr.status = 1;
    fr.confidence = 85;
    fr.category = "File";
    fr.source = "hfs_catalog";
    fr.createdAt = 0;
    fr.modifiedAt = 0;
    (*ctx.callback)(fr);
    ++ctx.fileCount;
    return true;
}

bool walkCatalogNode(CatalogCtx& ctx, uint32_t block, int depth) {
    if (ctx.isRunning && !(*ctx.isRunning)) return false;
    if (depth > 24) return true;

    std::vector<uint8_t> node;
    uint64_t off = ctx.partitionOffset + static_cast<uint64_t>(block) * ctx.blockSize;
    if (!readAt(*ctx.reader, off, ctx.blockSize, node)) return true;

    uint8_t kind = node[0];
    uint16_t numRecords = be16(node.data() + 4);
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
            if (!parseCatalogRecord(ctx, node.data() + start, static_cast<uint16_t>(end - start))) return false;
        }
        return true;
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
    if (!readAt(reader, partitionOffsetBytes + 1024, 512, hdrSector)) return false;
    uint16_t magic = be16(hdrSector.data());
    if (magic != 0x482B && magic != 0x5848) return false;

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
    return walkCatalogNode(ctx, rootBlock, 0);
}

} // namespace byteback
