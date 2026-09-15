#include "fs/partition_scanner.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>

namespace byteback {

VolumeFsKind probeVolumeAt(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return VolumeFsKind::Unknown;
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> boot(sectorSize);
    if (!readComplete(reader.readSectors(partitionOffsetBytes, sectorSize, boot.data()), sectorSize)) {
        return VolumeFsKind::Unread;
    }

    if (boot.size() >= 40 && std::memcmp(boot.data() + 3, "ReFS\x00\x00\x00\x00", 8) == 0 &&
        std::memcmp(boot.data() + 16, "FSRS", 4) == 0) {
        return VolumeFsKind::Refs;
    }
    if (boot.size() >= 16 && std::memcmp(boot.data() + 3, "NTFS    ", 8) == 0 &&
        boot[510] == 0x55 && boot[511] == 0xAA) {
        return VolumeFsKind::Ntfs;
    }
    if (boot.size() >= 16 && std::memcmp(boot.data() + 3, "EXFAT   ", 8) == 0 &&
        boot.size() >= 512 && boot[510] == 0x55 && boot[511] == 0xAA) {
        return VolumeFsKind::ExFat;
    }
    if (boot.size() >= 90 && std::memcmp(boot.data() + 82, "FAT32   ", 8) == 0 &&
        boot[510] == 0x55 && boot[511] == 0xAA) {
        return VolumeFsKind::Fat;
    }
    if (boot.size() >= 62 && std::memcmp(boot.data() + 54, "FAT16   ", 8) == 0 &&
        boot[510] == 0x55 && boot[511] == 0xAA) {
        return VolumeFsKind::Fat;
    }
    if (boot.size() >= 62 && std::memcmp(boot.data() + 54, "FAT12   ", 8) == 0) {
        return VolumeFsKind::Fat;
    }

    if (boot.size() >= 36 && std::memcmp(boot.data() + 32, "NXSB", 4) == 0) {
        return VolumeFsKind::Apfs;
    }

    // XFS superblock starts at byte 0 of the partition ("XFSB", xfs_dsb).
    if (boot.size() >= 4 && std::memcmp(boot.data(), "XFSB", 4) == 0) {
        return VolumeFsKind::Xfs;
    }

    uint32_t hfsRead = ((1024 + 2 + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> hfsBuf(hfsRead);
    if (!readComplete(reader.readSectors(partitionOffsetBytes, hfsRead, hfsBuf.data()), hfsRead)) {
        return VolumeFsKind::Unread;
    }
    if (hfsBuf.size() >= 1026) {
        if (hfsBuf[1024] == 0x48 && (hfsBuf[1025] == 0x2B || hfsBuf[1025] == 0x58)) {
            return VolumeFsKind::Hfs;
        }
    }

    uint32_t sbRead = ((2048 + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> extBuf(sbRead);
    if (!readComplete(reader.readSectors(partitionOffsetBytes, sbRead, extBuf.data()), sbRead)) {
        return VolumeFsKind::Unread;
    }
    if (extBuf.size() >= 1026) {
        uint16_t magic = *reinterpret_cast<uint16_t*>(extBuf.data() + 1024 + 0x38);
        if (magic == 0xEF53) return VolumeFsKind::Ext4;
    }

    return VolumeFsKind::Unknown;
}

#pragma pack(push, 1)
struct MBRPartitionEntry {
    uint8_t bootIndicator;
    uint8_t startCHS[3];
    uint8_t sysId;
    uint8_t endCHS[3];
    uint32_t startLBA;
    uint32_t sizeLBA;
};

struct GPTPartitionEntry {
    uint8_t typeGUID[16];
    uint8_t uniqueGUID[16];
    uint64_t startingLBA;
    uint64_t endingLBA;
    uint64_t attributes;
    char16_t partitionName[36];
};
#pragma pack(pop)

PartitionScanner::PartitionScanner(DiskReader* reader) : reader_(reader) {}

std::vector<PartitionInfo> PartitionScanner::parseMBR() {
    std::vector<PartitionInfo> partitions;
    if (!reader_ || !reader_->isOpen()) return partitions;

    uint32_t sectorSize = reader_->getSectorSize();
    if (sectorSize < 512) return partitions;

    std::vector<uint8_t> buffer(sectorSize);
    auto res = reader_->readSectors(0, sectorSize, buffer.data());
    if (!readComplete(res, sectorSize)) {
        tableUnread_ = true;
        return partitions;
    }

    if (buffer[510] != 0x55 || buffer[511] != 0xAA) return partitions;

    for (int i = 0; i < 4; ++i) {
        MBRPartitionEntry* entry = reinterpret_cast<MBRPartitionEntry*>(buffer.data() + 0x1BE + i * 16);
        if (entry->sysId == 0) continue;

        PartitionInfo info;
        info.startSector = entry->startLBA;
        info.sizeInSectors = entry->sizeLBA;
        info.isActive = (entry->bootIndicator == 0x80);
        
        switch (entry->sysId) {
            case 0x07: info.type = "NTFS/exFAT"; break;
            case 0x0B:
            case 0x0C: info.type = "FAT32"; break;
            case 0x83: info.type = "EXT"; break;
            case 0xEE: info.type = "GPT Protective"; break;
            default: info.type = "Unknown (0x" + std::to_string(entry->sysId) + ")"; break;
        }
        
        partitions.push_back(info);
    }

    return partitions;
}

std::vector<PartitionInfo> PartitionScanner::parseGPT() {
    std::vector<PartitionInfo> partitions;
    if (!reader_ || !reader_->isOpen()) return partitions;

    uint32_t sectorSize = reader_->getSectorSize();
    if (sectorSize < 512) return partitions;
    std::vector<uint8_t> buffer(sectorSize);

    auto res = reader_->readSectors(sectorSize, sectorSize, buffer.data());
    if (!readComplete(res, sectorSize)) {
        tableUnread_ = true;
        return partitions;
    }

    if (std::memcmp(buffer.data(), "EFI PART", 8) != 0) return partitions;

    uint64_t partitionEntryLBA = *reinterpret_cast<uint64_t*>(buffer.data() + 72);
    uint32_t numPartitionEntries = *reinterpret_cast<uint32_t*>(buffer.data() + 80);
    uint32_t partitionEntrySize = *reinterpret_cast<uint32_t*>(buffer.data() + 84);

    if (partitionEntrySize < sizeof(GPTPartitionEntry) || partitionEntrySize == 0) return partitions;
    if (numPartitionEntries == 0) return partitions;

    auto consumeEntry = [&](const uint8_t* raw) {
        const GPTPartitionEntry* entry = reinterpret_cast<const GPTPartitionEntry*>(raw);
        bool isEmpty = true;
        for (int j = 0; j < 16; ++j) {
            if (entry->typeGUID[j] != 0) {
                isEmpty = false;
                break;
            }
        }
        if (isEmpty) return;

        PartitionInfo info;
        info.startSector = entry->startingLBA;
        info.sizeInSectors = entry->endingLBA - entry->startingLBA + 1;
        info.isActive = false;

        uint32_t guid1 = *reinterpret_cast<const uint32_t*>(entry->typeGUID);
        if (guid1 == 0xEBD0A0A2) info.type = "Windows Basic Data";
        else if (guid1 == 0x0FC63DAF) info.type = "Linux Data";
        else if (guid1 == 0xC12A7328) info.type = "EFI System";
        else info.type = "Unknown GUID";

        std::string label;
        for (int j = 0; j < 36; ++j) {
            if (entry->partitionName[j] == 0) break;
            label += static_cast<char>(entry->partitionName[j]);
        }
        info.label = label;
        partitions.push_back(info);
    };

    uint32_t entriesPerSector = sectorSize / partitionEntrySize;
    if (entriesPerSector == 0) {
        const uint64_t bytes = static_cast<uint64_t>(numPartitionEntries) * partitionEntrySize;
        const uint64_t aligned = ((bytes + sectorSize - 1) / sectorSize) * sectorSize;
        if (aligned > UINT32_MAX) {
            tableUnread_ = true;
            return partitions;
        }
        std::vector<uint8_t> entryBuffer(static_cast<size_t>(aligned));
        res = reader_->readSectors(partitionEntryLBA * sectorSize,
                                   static_cast<uint32_t>(aligned), entryBuffer.data());
        if (!readComplete(res, aligned)) {
            tableUnread_ = true;
            const uint64_t walk = res.success ? std::min<uint64_t>(res.bytesRead, aligned) : 0;
            uint32_t n = static_cast<uint32_t>(walk / partitionEntrySize);
            if (n > numPartitionEntries) n = numPartitionEntries;
            for (uint32_t i = 0; i < n; ++i) {
                consumeEntry(entryBuffer.data() + i * partitionEntrySize);
            }
            return partitions;
        }
        for (uint32_t i = 0; i < numPartitionEntries; ++i) {
            consumeEntry(entryBuffer.data() + i * partitionEntrySize);
        }
        return partitions;
    }

    // Sector-at-a-time: a faulted later LBA must not drop already-read entries
    // as “empty GUID / no partitions”.
    const uint32_t sectorsToRead = (numPartitionEntries + entriesPerSector - 1) / entriesPerSector;
    std::vector<uint8_t> sector(sectorSize);
    uint32_t parsed = 0;
    for (uint32_t s = 0; s < sectorsToRead && parsed < numPartitionEntries; ++s) {
        res = reader_->readSectors((partitionEntryLBA + s) * sectorSize, sectorSize, sector.data());
        if (!readComplete(res, sectorSize)) {
            tableUnread_ = true;
            parsed += entriesPerSector;
            continue;
        }
        for (uint32_t e = 0; e < entriesPerSector && parsed < numPartitionEntries; ++e, ++parsed) {
            consumeEntry(sector.data() + e * partitionEntrySize);
        }
    }

    return partitions;
}

std::vector<PartitionInfo> PartitionScanner::scanForPartitions(uint32_t stepSectors, ProgressCallback progressCallback) {
    std::vector<PartitionInfo> partitions;
    scanUnread_ = false;
    if (!reader_ || !reader_->isOpen()) return partitions;

    uint64_t totalSectors = reader_->getDiskSize() / reader_->getSectorSize();
    uint32_t sectorSize = reader_->getSectorSize();

    std::vector<uint8_t> buffer(sectorSize);
    std::vector<uint8_t> extBuffer(2048); // For reading 2 sectors for EXT superblock

    for (uint64_t sector = 0; sector < totalSectors; sector += stepSectors) {
        if (progressCallback) {
            progressCallback(sector, totalSectors);
        }

        auto res = reader_->readSectors(sector * sectorSize, sectorSize, buffer.data());
        if (!readComplete(res, sectorSize)) {
            scanUnread_ = true;
            continue;
        }

        bool found = false;
        PartitionInfo info;
        info.startSector = sector;
        info.sizeInSectors = 0;
        info.isActive = false;

        // NTFS: 'NTFS' at +3, 0x55AA at +510
        if (std::memcmp(buffer.data() + 3, "NTFS    ", 8) == 0 && 
            buffer[510] == 0x55 && buffer[511] == 0xAA) {
            info.type = "NTFS";
            found = true;
        }
        // exFAT: 'EXFAT' at +3
        else if (std::memcmp(buffer.data() + 3, "EXFAT   ", 8) == 0) {
            info.type = "exFAT";
            found = true;
        }
        // FAT32: 'FAT32' at +82, 0x55AA at +510
        else if (std::memcmp(buffer.data() + 82, "FAT32   ", 8) == 0 && 
                 buffer[510] == 0x55 && buffer[511] == 0xAA) {
            info.type = "FAT32";
            found = true;
        }
        // FAT16: 'FAT16' at +54, 0x55AA at +510
        else if (std::memcmp(buffer.data() + 54, "FAT16   ", 8) == 0 && 
                 buffer[510] == 0x55 && buffer[511] == 0xAA) {
            info.type = "FAT16";
            found = true;
        }
        else {
            // Check EXT superblock (requires reading at least 1024 bytes)
            if (sectorSize >= 2048) {
                uint16_t magic = *reinterpret_cast<uint16_t*>(buffer.data() + 1024 + 0x38);
                if (magic == 0xEF53) {
                    info.type = "EXT";
                    found = true;
                }
            } else {
                auto extRes = reader_->readSectors(sector * sectorSize, 2048, extBuffer.data());
                if (!readComplete(extRes, 2048)) {
                    scanUnread_ = true;
                } else {
                    uint16_t magic = *reinterpret_cast<uint16_t*>(extBuffer.data() + 1024 + 0x38);
                    if (magic == 0xEF53) {
                        info.type = "EXT";
                        found = true;
                    }
                }
            }
        }

        if (found) {
            partitions.push_back(info);
            // Optionally could advance sector by a large amount if we can read volume size from boot sector
        }
    }
    
    if (progressCallback) {
        progressCallback(totalSectors, totalSectors);
    }

    return partitions;
}

} // namespace byteback
