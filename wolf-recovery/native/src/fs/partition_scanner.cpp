#include "fs/partition_scanner.h"
#include <cstring>
#include <iostream>

namespace wolf {

VolumeFsKind probeVolumeAt(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return VolumeFsKind::Unknown;
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> boot(sectorSize);
    if (!reader.readSectors(partitionOffsetBytes, sectorSize, boot.data()).success) {
        return VolumeFsKind::Unknown;
    }

    if (boot.size() >= 16 && std::memcmp(boot.data() + 3, "NTFS    ", 8) == 0 &&
        boot[510] == 0x55 && boot[511] == 0xAA) {
        return VolumeFsKind::Ntfs;
    }
    if (boot.size() >= 16 && std::memcmp(boot.data() + 3, "EXFAT   ", 8) == 0) {
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

    uint32_t sbRead = ((2048 + sectorSize - 1) / sectorSize) * sectorSize;
    std::vector<uint8_t> extBuf(sbRead);
    if (reader.readSectors(partitionOffsetBytes, sbRead, extBuf.data()).success &&
        extBuf.size() >= 1026) {
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
    if (!res.success) return partitions;

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
    std::vector<uint8_t> buffer(sectorSize);
    
    // Read LBA 1
    auto res = reader_->readSectors(sectorSize, sectorSize, buffer.data());
    if (!res.success) return partitions;

    if (std::memcmp(buffer.data(), "EFI PART", 8) != 0) return partitions;

    uint64_t partitionEntryLBA = *reinterpret_cast<uint64_t*>(buffer.data() + 72);
    uint32_t numPartitionEntries = *reinterpret_cast<uint32_t*>(buffer.data() + 80);
    uint32_t partitionEntrySize = *reinterpret_cast<uint32_t*>(buffer.data() + 84);

    if (partitionEntrySize < sizeof(GPTPartitionEntry)) return partitions;

    uint32_t entriesPerSector = sectorSize / partitionEntrySize;
    uint32_t sectorsToRead = (numPartitionEntries + entriesPerSector - 1) / entriesPerSector;

    std::vector<uint8_t> entryBuffer(sectorsToRead * sectorSize);
    res = reader_->readSectors(partitionEntryLBA * sectorSize, sectorsToRead * sectorSize, entryBuffer.data());
    if (!res.success) return partitions;

    for (uint32_t i = 0; i < numPartitionEntries; ++i) {
        GPTPartitionEntry* entry = reinterpret_cast<GPTPartitionEntry*>(entryBuffer.data() + i * partitionEntrySize);
        
        bool isEmpty = true;
        for (int j = 0; j < 16; ++j) {
            if (entry->typeGUID[j] != 0) {
                isEmpty = false;
                break;
            }
        }
        if (isEmpty) continue;

        PartitionInfo info;
        info.startSector = entry->startingLBA;
        info.sizeInSectors = entry->endingLBA - entry->startingLBA + 1;
        info.isActive = false; // GPT uses attributes, but typically not a simple boot flag like MBR
        
        // Very basic GUID check (first dword)
        uint32_t guid1 = *reinterpret_cast<uint32_t*>(entry->typeGUID);
        if (guid1 == 0xEBD0A0A2) info.type = "Windows Basic Data"; // NTFS/FAT/exFAT
        else if (guid1 == 0x0FC63DAF) info.type = "Linux Data"; // EXT/XFS
        else if (guid1 == 0xC12A7328) info.type = "EFI System";
        else info.type = "Unknown GUID";

        std::string label;
        for (int j = 0; j < 36; ++j) {
            if (entry->partitionName[j] == 0) break;
            label += static_cast<char>(entry->partitionName[j]);
        }
        info.label = label;

        partitions.push_back(info);
    }

    return partitions;
}

std::vector<PartitionInfo> PartitionScanner::scanForPartitions(uint32_t stepSectors, ProgressCallback progressCallback) {
    std::vector<PartitionInfo> partitions;
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
        if (!res.success) continue;

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
                if (extRes.success) {
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

} // namespace wolf
