#include "fs/apfs_container.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace wolf {

namespace {

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

    uint64_t blockSize = readLe64(nx.data() + 40);
    if (blockSize < 4096 || blockSize > 1024 * 1024) blockSize = 4096;
    uint64_t blockCount = readLe64(nx.data() + 48);
    if (blockCount == 0) blockCount = (spanBytes - partitionOffsetBytes) / blockSize;

    uint64_t maxBlocks = (spanBytes - partitionOffsetBytes) / blockSize;
    blockCount = std::min(blockCount, maxBlocks);
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

    std::vector<uint8_t> block(static_cast<size_t>(blockSize));
    int volumeIndex = 0;
    const uint64_t scanCap = std::min<uint64_t>(blockCount, 4096);

    for (uint64_t i = 0; i < scanCap; ++i) {
        if (isRunning && !(*isRunning)) break;
        uint64_t off = partitionOffsetBytes + i * blockSize;
        if (!readBlock(reader, off, static_cast<uint32_t>(blockSize), block)) continue;
        if (std::strncmp(reinterpret_cast<char*>(block.data() + 32), "APSB", 4) != 0) continue;

        std::string volName = readCString(block.data() + 72, 128);
        if (volName.empty()) volName = "Volume_" + std::to_string(volumeIndex + 1);

        FileRecord fr;
        fr.id = volumeIndex++;
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

    FileRecord tick;
    tick.id = -1;
    tick.startSector = (partitionOffsetBytes + scanCap * blockSize) / sectorSize;
    callback(tick);
    return true;
}

} // namespace wolf
