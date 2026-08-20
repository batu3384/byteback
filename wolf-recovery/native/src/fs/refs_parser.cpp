#include "fs/refs_parser.h"
#include "fs/refs_integrity.h"
#include "fs/ntfs_util.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace wolf {

namespace {

constexpr uint64_t kRefsSuperblockCluster = 30;

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

bool readAt(DiskReader& reader, uint64_t offset, uint32_t size, std::vector<uint8_t>& buf) {
    buf.resize(size);
    auto res = reader.readSectors(offset, size, buf.data());
    return res.success && res.bytesRead >= size;
}

std::string sanitizeRefsName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':') out.push_back('_');
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

void scanMetadataPageForEntries(const uint8_t* page, size_t pageSize, uint64_t pageOffsetBytes,
                                uint32_t sectorSize, int& fileIndex,
                                FileSystemParser::FileRecordCallback& callback,
                                std::unordered_set<std::string>& seen) {
    if (pageSize < 64) return;
    // Entry record key: 0x30 0x00, entry type uint16 LE at +2 (1 = file).
    for (size_t i = 0; i + 8 < pageSize; ++i) {
        if (page[i] != 0x30 || page[i + 1] != 0x00) continue;
        uint16_t entryType = readLe16(page + i + 2);
        if (entryType != 1) continue;
        size_t nameOff = i + 4;
        if (nameOff + 4 > pageSize) continue;
        size_t nameEnd = nameOff;
        while (nameEnd + 1 < pageSize && nameEnd - nameOff < 512) {
            if (page[nameEnd] == 0 && page[nameEnd + 1] == 0) break;
            nameEnd += 2;
        }
        if (nameEnd <= nameOff) continue;
        std::string name = ntfs::utf16leToUtf8(reinterpret_cast<const uint16_t*>(page + nameOff),
                                                 (nameEnd - nameOff) / 2);
        name = sanitizeRefsName(name);
        if (name.empty() || seen.count(name)) continue;
        seen.insert(name);

        FileRecord fr;
        fr.id = fileIndex++;
        fr.name = name;
        fr.path = "/" + name;
        fr.sizeBytes = 0;
        fr.startSector = pageOffsetBytes / sectorSize;
        fr.endSector = fr.startSector + std::max<uint64_t>(1, pageSize / sectorSize);
        fr.status = 0;
        fr.confidence = 65;
        fr.category = "Document";
        fr.source = "refs";

        // Optional integrity-stream trailer: +128 checksum, +136 payload len, +144 payload bytes.
        if (i + 144 < pageSize) {
            const uint64_t storedCrc = readLe64(page + i + 128);
            const uint16_t payloadLen = readLe16(page + i + 136);
            if (storedCrc != 0 && payloadLen > 0 &&
                static_cast<size_t>(i) + 144u + payloadLen <= pageSize) {
                fr.residentData.assign(page + i + 144, page + i + 144 + payloadLen);
                fr.sizeBytes = payloadLen;
                fr.integrityChecksum = storedCrc;
                const uint64_t got = refsCrc64Ecma(fr.residentData.data(), fr.residentData.size());
                fr.confidence = (got == storedCrc) ? 92 : 30;
            }
        }
        callback(fr);
    }
}

void walkMinistoreNode(DiskReader& reader, uint64_t partitionOffsetBytes, uint64_t blockSize,
                       uint64_t blockCount, uint64_t blockNum, uint32_t sectorSize, int& fileIndex,
                       FileSystemParser::FileRecordCallback& callback,
                       std::unordered_set<uint64_t>& visitedBlocks,
                       std::unordered_set<std::string>& seenNames) {
    if (blockNum == 0 || blockNum >= blockCount || visitedBlocks.count(blockNum)) return;
    visitedBlocks.insert(blockNum);

    std::vector<uint8_t> page;
    uint64_t off = partitionOffsetBytes + blockNum * blockSize;
    if (!readAt(reader, off, static_cast<uint32_t>(blockSize), page)) return;
    if (page.size() < 8) return;

    scanMetadataPageForEntries(page.data(), page.size(), off, sectorSize, fileIndex, callback, seenNames);

    if (page.size() < 80 || std::memcmp(page.data(), "MSB+", 4) != 0) return;
    uint16_t level = static_cast<uint16_t>(page[34] | (page[35] << 8));
    uint32_t nkeys = readLe32(page.data() + 36);
    if (nkeys == 0 || nkeys > 64) return;

    for (uint32_t k = 0; k < nkeys; ++k) {
        uint32_t recOff = 56 + k * 24;
        if (recOff + 24 > page.size()) break;
        uint64_t child = readLe64(page.data() + recOff + 16);
        if (child == 0 || child >= blockCount || child == blockNum) continue;
        if (level == 0) {
            walkMinistoreNode(reader, partitionOffsetBytes, blockSize, blockCount, child,
                              sectorSize, fileIndex, callback, visitedBlocks, seenNames);
        } else {
            walkMinistoreNode(reader, partitionOffsetBytes, blockSize, blockCount, child,
                              sectorSize, fileIndex, callback, visitedBlocks, seenNames);
        }
    }
}

} // namespace

bool probeRefsBoot(const uint8_t* boot, size_t bootLen, uint32_t& bytesPerSector,
                   uint32_t& sectorsPerCluster) {
    if (bootLen < 40) return false;
    if (std::memcmp(boot + 3, "ReFS\x00\x00\x00\x00", 8) != 0) return false;
    if (std::memcmp(boot + 16, "FSRS", 4) != 0) return false;
    bytesPerSector = readLe32(boot + 32);
    sectorsPerCluster = readLe32(boot + 36);
    if (bytesPerSector < 512 || bytesPerSector > 4096) return false;
    if (sectorsPerCluster == 0 || sectorsPerCluster > 256) return false;
    return true;
}

RefsParser::RefsParser() = default;
RefsParser::~RefsParser() = default;

bool RefsParser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                        uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> boot(sectorSize);
    if (!readAt(reader, partitionOffsetBytes, sectorSize, boot)) return false;

    uint32_t bytesPerSector = 0;
    uint32_t sectorsPerCluster = 0;
    if (!probeRefsBoot(boot.data(), boot.size(), bytesPerSector, sectorsPerCluster)) return false;

    uint64_t clusterSize = static_cast<uint64_t>(bytesPerSector) * sectorsPerCluster;
    uint64_t superOff = partitionOffsetBytes + kRefsSuperblockCluster * clusterSize;

    std::vector<uint8_t> supb;
    if (!readAt(reader, superOff, static_cast<uint32_t>(clusterSize), supb)) return false;
    if (supb.size() < 48 || std::memcmp(supb.data(), "SUPB", 4) != 0) return false;

    int supbIntegrityConf = 90;
    const bool supbChecked = tryVerifyRefsMetadataPage(supb.data(), supb.size(), supbIntegrityConf);

  {
        FileRecord vol;
        vol.id = -1;
        vol.name = "ReFS_Volume";
        vol.path = "/refs/";
        vol.sizeBytes = partitionSizeBytes > 0 ? partitionSizeBytes : reader.getDiskSize();
        vol.startSector = partitionOffsetBytes / sectorSize;
        vol.endSector = vol.startSector + std::max<uint64_t>(1, vol.sizeBytes / sectorSize);
        vol.status = 0;
        vol.confidence = supbChecked ? supbIntegrityConf : 90;
        vol.category = "System";
        vol.source = "refs_volume";
        callback(vol);
    }

    int fileIndex = 0;
    std::unordered_set<std::string> seenNames;
    std::unordered_set<uint64_t> visitedBlocks;

    uint64_t spanBytes = reader.getDiskSize();
    if (partitionSizeBytes > 0) spanBytes = std::min(spanBytes, partitionOffsetBytes + partitionSizeBytes);
    uint64_t blockCount = (spanBytes > partitionOffsetBytes)
                              ? (spanBytes - partitionOffsetBytes) / clusterSize
                              : 0;
    if (blockCount == 0) return true;

    // Checkpoint at SUPB+32 names ministore root blocks for directory trees.
    uint64_t chkOff = readLe32(supb.data() + 32);
    if (chkOff + 16 <= supb.size()) {
        uint64_t chkBlock = readLe64(supb.data() + chkOff);
        if (chkBlock > 0 && chkBlock < blockCount) {
            walkMinistoreNode(reader, partitionOffsetBytes, clusterSize, blockCount, chkBlock,
                              sectorSize, fileIndex, callback, visitedBlocks, seenNames);
        }
    }

    // ponytail: linear metadata probe when checkpoint walk yields nothing.
    if (seenNames.empty()) {
        const uint64_t probe = std::min(blockCount, uint64_t{512});
        for (uint64_t b = 0; b < probe; ++b) {
            if (isRunning && !(*isRunning)) break;
            if (b == kRefsSuperblockCluster) continue;
            walkMinistoreNode(reader, partitionOffsetBytes, clusterSize, blockCount, b,
                              sectorSize, fileIndex, callback, visitedBlocks, seenNames);
        }
    }

    FileRecord tick;
    tick.id = -1;
    tick.startSector = (partitionOffsetBytes + blockCount * clusterSize) / sectorSize;
    callback(tick);
    return true;
}

bool RefsParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0);
}

} // namespace wolf
