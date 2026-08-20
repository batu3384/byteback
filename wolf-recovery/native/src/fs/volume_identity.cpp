#include "fs/volume_identity.h"
#include "fs/partition_scanner.h"
#include <cstring>

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

} // namespace

uint64_t parseVolumeSerial(const uint8_t* boot, size_t n) {
    if (!boot || n < 512) return 0;
    if (std::memcmp(boot + 3, "NTFS    ", 8) == 0) return readLe64(boot + 0x48);
    if (std::memcmp(boot + 3, "EXFAT   ", 8) == 0) return readLe32(boot + 0x64);
    if (n >= 0x56 && std::memcmp(boot + 0x52, "FAT32", 5) == 0) return readLe32(boot + 0x43);
    if (n >= 0x3B && std::memcmp(boot + 0x36, "FAT", 3) == 0) return readLe32(boot + 0x27);
    return 0;
}

std::unordered_set<uint64_t> collectVolumeSerials(DiskReader& reader) {
    std::unordered_set<uint64_t> out;
    if (!reader.isOpen() && !reader.hasRaidBackend()) return out;

    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;

    auto addAt = [&](uint64_t offsetBytes) {
        std::vector<uint8_t> boot(ss);
        if (!reader.readSectors(offsetBytes, ss, boot.data()).success) return;
        uint64_t s = parseVolumeSerial(boot.data(), boot.size());
        if (s != 0) out.insert(s);
    };

    addAt(0);
    PartitionScanner parts(&reader);
    auto all = parts.parseMBR();
    auto gpt = parts.parseGPT();
    all.insert(all.end(), gpt.begin(), gpt.end());
    for (const auto& p : all) {
        addAt(p.startSector * static_cast<uint64_t>(ss));
    }
    return out;
}

} // namespace wolf
