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

void scanBtreeNodeForCatalog(const uint8_t* block, uint32_t blockSize, uint64_t blockOff,
                             uint32_t sectorSize, int& fileIndex,
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
            uint64_t phys = readLe64(block + i + 16);
            (void)phys;
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

} // namespace

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
    for (uint64_t i = 0; i < blockCount; ++i) {
        if (isRunning && !(*isRunning)) break;
        uint64_t off = partitionOffsetBytes + i * blockSize;
        if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) continue;
        if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) == 0) {
            tryApsbAt(reader, off, static_cast<uint32_t>(blockSize), sectorSize, volumeIndex, seenVol, callback);
        }
        scanBtreeNodeForCatalog(block.data(), static_cast<uint32_t>(blockSize), off, sectorSize,
                                fileIndex, callback, seenNames);
    }

    FileRecord tick;
    tick.id = -1;
    tick.startSector = (partitionOffsetBytes + blockCount * blockSize) / sectorSize;
    callback(tick);
    return true;
}

} // namespace wolf
