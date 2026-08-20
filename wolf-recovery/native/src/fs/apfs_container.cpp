#include "fs/apfs_container.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace wolf {

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
    return res.success && res.bytesRead >= size;
}

constexpr uint64_t kApfsTypeDirRec = 9;
constexpr uint64_t kApfsTypeFileExtent = 8;
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
                             std::unordered_set<std::string>& seenNames) {
    if (blockSize < 64) return;
    uint32_t otype = readLe32(block + 24) & 0xFFFFu;
    if (otype != kObjBtreeNode) return;
    uint16_t level = static_cast<uint16_t>(block[34] | (block[35] << 8));
    if (level != 0) return;
    uint32_t nkeys = readLe32(block + 36);
    if (nkeys == 0 || nkeys > 4096) return;

    for (uint32_t i = 56; i + 16 < blockSize; i += 8) {
        uint64_t hdr = readLe64(block + i);
        uint64_t typ = hdr >> 48;
        if (typ == kApfsTypeDirRec) {
            uint32_t nlh = readLe32(block + i + 8);
            uint32_t nlen = nlh & 0x3FFu;
            if (nlen < 2 || nlen > 255 || i + 12 + nlen > blockSize) continue;
            if (block[i + 12 + nlen - 1] != 0) continue;
            std::string name(reinterpret_cast<const char*>(block + i + 12), nlen - 1);
            if (name.empty() || seenNames.count(name)) continue;
            seenNames.insert(name);
            FileRecord fr;
            fr.id = fileIndex++;
            fr.name = name;
            fr.path = "/apfs/" + name;
            fr.startSector = blockOff / sectorSize;
            fr.endSector = fr.startSector + 1;
            fr.status = 0;
            fr.confidence = 70;
            fr.category = "Document";
            fr.source = "apfs_file";
            callback(fr);
        } else if (typ == kApfsTypeFileExtent && i + 24 < blockSize) {
            uint64_t lenFlags = readLe64(block + i + 8);
            uint64_t len = lenFlags & ((1ull << 56) - 1);
            uint64_t phys = readLe64(block + i + 16);
            if (phys == 0 || len == 0) continue;
            FileRecord fr;
            fr.id = fileIndex++;
            fr.name = "extent_" + std::to_string(phys);
            fr.path = "/apfs/extent/" + std::to_string(phys);
            fr.sizeBytes = len;
            fr.startSector = (partitionOffsetBytes + phys * blockSize) / sectorSize;
            FileRecord::DataRun run;
            run.startSector = fr.startSector;
            run.sectorCount = (len + sectorSize - 1) / sectorSize;
            if (run.sectorCount == 0) run.sectorCount = 1;
            fr.runs.push_back(run);
            fr.endSector = run.startSector + run.sectorCount;
            fr.status = 0;
            fr.confidence = 60;
            fr.category = "Document";
            fr.source = "apfs_extent";
            callback(fr);
        }
    }
}

bool tryApsbAt(DiskReader& reader, uint64_t off, uint32_t blockSize, uint32_t sectorSize,
               int& volumeIndex, std::unordered_set<uint64_t>& seenVol,
               FileSystemParser::FileRecordCallback& callback) {
    std::vector<uint8_t> block;
    if (!readBlock(reader, off, blockSize, block)) return false;
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
                   std::unordered_set<uint64_t>& visited) {
    if (blockNum == 0 || blockNum >= blockCount || visited.count(blockNum)) return;
    visited.insert(blockNum);
    std::vector<uint8_t> blk;
    if (!readBlock(reader, partitionOffsetBytes + blockNum * blockSize,
                   static_cast<uint32_t>(blockSize), blk)) {
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
            walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount, val, paddrs, visited);
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
    if (!readBlock(reader, partitionOffsetBytes, 4096, nx)) return false;
    if (std::strncmp(reinterpret_cast<char*>(nx.data() + 32), "NXSB", 4) != 0) return false;

    uint32_t blockSize32 = readLe32(nx.data() + 36);
    uint64_t blockSize = blockSize32;
    if (blockSize < 4096 || blockSize > 1024 * 1024) {
        uint64_t legacy = readLe64(nx.data() + 40);
        blockSize = (legacy >= 4096 && legacy <= 1024 * 1024) ? legacy : 4096;
    }
    uint64_t blockCount = readLe64(nx.data() + 40);
    if (blockSize32 >= 4096 && blockSize32 <= 1024 * 1024) {
        blockCount = readLe64(nx.data() + 40);
    }
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

    if (nx.size() >= 184 + 100 * 8) {
        for (int i = 0; i < 100; ++i) {
            if (isRunning && !(*isRunning)) break;
            uint64_t oid = readLe64(nx.data() + 184 + static_cast<size_t>(i) * 8);
            if (oid == 0 || oid >= blockCount) continue;
            tryApsbAt(reader, partitionOffsetBytes + oid * blockSize, static_cast<uint32_t>(blockSize),
                      sectorSize, volumeIndex, seenVol, callback);
        }
    }

    std::vector<uint8_t> block(static_cast<size_t>(blockSize));
    const uint64_t probe = std::min(blockCount, uint64_t{256});
    for (uint64_t i = 0; i < probe; ++i) {
        if (isRunning && !(*isRunning)) break;
        uint64_t off = partitionOffsetBytes + i * blockSize;
        if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) continue;
        if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) == 0) {
            tryApsbAt(reader, off, static_cast<uint32_t>(blockSize), sectorSize, volumeIndex, seenVol, callback);
        }
        scanBtreeNodeForCatalog(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                                off, sectorSize, fileIndex, callback, seenNames);
    }

    uint64_t omapOid = (nx.size() >= 0xA8) ? readLe64(nx.data() + 0xA0) : 0;
    if (omapOid > 0 && omapOid < blockCount) {
        std::vector<uint8_t> omapBlk;
        if (readBlock(reader, partitionOffsetBytes + omapOid * blockSize, static_cast<uint32_t>(blockSize), omapBlk) &&
            omapBlk.size() >= 56) {
            uint64_t treeOid = readLe64(omapBlk.data() + 48);
            if (treeOid == 0 || treeOid >= blockCount) treeOid = omapOid;
            std::vector<uint8_t> treeBlk;
            if (readBlock(reader, partitionOffsetBytes + treeOid * blockSize, static_cast<uint32_t>(blockSize), treeBlk)) {
                std::vector<uint64_t> paddrs;
                std::unordered_set<uint64_t> visitedOmap;
                walkOmapBtree(reader, partitionOffsetBytes, blockSize, blockCount, treeOid, paddrs, visitedOmap);
                for (uint64_t paddr : paddrs) {
                    if (isRunning && !(*isRunning)) break;
                    if (paddr < probe) continue;
                    uint64_t off = partitionOffsetBytes + paddr * blockSize;
                    if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) continue;
                    if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) == 0) {
                        tryApsbAt(reader, off, static_cast<uint32_t>(blockSize), sectorSize, volumeIndex, seenVol, callback);
                    }
                    scanBtreeNodeForCatalog(block.data(), static_cast<uint32_t>(blockSize), partitionOffsetBytes,
                                            off, sectorSize, fileIndex, callback, seenNames);
                }
            }
        }
    }

    FileRecord tick;
    tick.id = -1;
    tick.startSector = (partitionOffsetBytes + blockCount * blockSize) / sectorSize;
    callback(tick);
    return true;
}

} // namespace wolf
