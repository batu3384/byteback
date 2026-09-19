#include "fs/partition_scanner.h"
#include "fs/luks.h"
#include "fs/ldm_parser.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>

namespace byteback {

namespace {

bool looksLikeLvm2Label(const uint8_t* s, size_t n) {
    return n >= 32 && std::memcmp(s, "LABELONE", 8) == 0 &&
           std::memcmp(s + 24, "LVM2 001", 8) == 0;
}

} // namespace

VolumeFsKind probeVolumeAt(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t sectorSize) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return VolumeFsKind::Unknown;
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> boot(sectorSize);
    if (!readComplete(reader.readSectors(partitionOffsetBytes, sectorSize, boot.data()), sectorSize)) {
        return VolumeFsKind::Unread;
    }

    if (looksLikeLuks1(boot.data(), boot.size())) return VolumeFsKind::Luks;
    if (looksLikeSpacedb(boot.data(), boot.size())) return VolumeFsKind::Spaces;

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

    // LVM2 PV: label in one of the first 4 × 512-byte sectors, default offset 512.
    if (extBuf.size() >= 512 + 32 && looksLikeLvm2Label(extBuf.data() + 512, extBuf.size() - 512)) {
        return VolumeFsKind::Lvm;
    }

    // ISO 9660 PVD lives at logical sector 16 (2048-byte CD sectors), not LBA 0.
    // Earlier boot reads already succeeded — a short image is Unknown, not Unread.
    // UDF VRS (ECMA-167): type-0 BEA01 then NSR02/NSR03 in the same window.
    // Hybrid ISO+UDF with a type-1 CD001 PVD stays Iso9660 (ISO wins).
    constexpr uint32_t kIsoSec = 2048;
    uint8_t isoBuf[kIsoSec];
    bool udfBea = false;
    bool udfNsr = false;
    for (uint32_t s = 16; s < 32; ++s) {
        const uint64_t off = partitionOffsetBytes + static_cast<uint64_t>(s) * kIsoSec;
        if (!readComplete(reader.readSectors(off, kIsoSec, isoBuf), kIsoSec)) continue;
        if (isoBuf[0] == 255 && std::memcmp(isoBuf + 1, "CD001", 5) == 0) break;
        if (isoBuf[0] == 1 && std::memcmp(isoBuf + 1, "CD001", 5) == 0 && isoBuf[6] == 1) {
            return VolumeFsKind::Iso9660;
        }
        if (isoBuf[0] == 0 && isoBuf[6] == 1) {
            if (std::memcmp(isoBuf + 1, "BEA01", 5) == 0) udfBea = true;
            if (std::memcmp(isoBuf + 1, "NSR02", 5) == 0 || std::memcmp(isoBuf + 1, "NSR03", 5) == 0)
                udfNsr = true;
            if (std::memcmp(isoBuf + 1, "TEA01", 5) == 0) break;
        }
    }
    if (udfBea && udfNsr) return VolumeFsKind::Udf;

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
            case 0x42: info.type = "LDM"; break;
            default: info.type = "Unknown (0x" + std::to_string(entry->sysId) + ")"; break;
        }
        
        partitions.push_back(info);
    }

    return partitions;
}

std::vector<PartitionInfo> PartitionScanner::parseAPM() {
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
    if (buffer[0] != 'E' || buffer[1] != 'R') return partitions;
    const uint16_t blk = static_cast<uint16_t>((buffer[2] << 8) | buffer[3]);
    if ((blk != 512 && blk != 2048) || (blk % sectorSize) != 0) return partitions;
    const uint32_t scale = blk / sectorSize;

    auto res1 = reader_->readSectors(blk, sectorSize, buffer.data());
    if (!readComplete(res1, sectorSize)) {
        tableUnread_ = true;
        return partitions;
    }
    if (buffer[0] != 'P' || buffer[1] != 'M') return partitions;
    const uint32_t mapCnt = (static_cast<uint32_t>(buffer[4]) << 24) |
                            (static_cast<uint32_t>(buffer[5]) << 16) |
                            (static_cast<uint32_t>(buffer[6]) << 8) |
                            static_cast<uint32_t>(buffer[7]);
    if (mapCnt < 2 || mapCnt > 64) return partitions;

    auto skipType = [](const char* t) {
        if (std::strcmp(t, "Apple_partition_map") == 0) return true;
        if (std::strcmp(t, "Apple_Free") == 0) return true;
        if (std::strcmp(t, "Apple_Void") == 0) return true;
        return std::strncmp(t, "Apple_Driver", 12) == 0;
    };

    for (uint32_t i = 0; i < mapCnt; ++i) {
        if (i > 0) {
            auto r = reader_->readSectors(static_cast<uint64_t>(i + 1) * blk, sectorSize,
                                          buffer.data());
            if (!readComplete(r, sectorSize)) {
                tableUnread_ = true;
                return partitions;
            }
            if (buffer[0] != 'P' || buffer[1] != 'M') continue;
        }
        const uint32_t start = (static_cast<uint32_t>(buffer[8]) << 24) |
                               (static_cast<uint32_t>(buffer[9]) << 16) |
                               (static_cast<uint32_t>(buffer[10]) << 8) |
                               static_cast<uint32_t>(buffer[11]);
        const uint32_t cnt = (static_cast<uint32_t>(buffer[12]) << 24) |
                             (static_cast<uint32_t>(buffer[13]) << 16) |
                             (static_cast<uint32_t>(buffer[14]) << 8) |
                             static_cast<uint32_t>(buffer[15]);
        char type[33];
        std::memcpy(type, buffer.data() + 48, 32);
        type[32] = 0;
        if (start == 0 || cnt == 0 || skipType(type)) continue;
        PartitionInfo info;
        info.startSector = static_cast<uint64_t>(start) * scale;
        info.sizeInSectors = static_cast<uint64_t>(cnt) * scale;
        info.type = type;
        info.isActive = false;
        char name[33];
        std::memcpy(name, buffer.data() + 16, 32);
        name[32] = 0;
        info.label = name;
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

    auto tryHeader = [&](uint64_t lba) -> bool {
        auto res = reader_->readSectors(lba * sectorSize, sectorSize, buffer.data());
        if (!readComplete(res, sectorSize)) {
            tableUnread_ = true;
            return false;
        }
        return std::memcmp(buffer.data(), "EFI PART", 8) == 0;
    };

    // Primary at LBA 1; UEFI AlternateLBA (typically last sector) if wiped.
    bool haveHeader = tryHeader(1);
    if (!haveHeader) {
        const uint64_t totalSectors = reader_->getDiskSize() / sectorSize;
        if (totalSectors >= 2) haveHeader = tryHeader(totalSectors - 1);
        if (!haveHeader) return partitions;
    }

    uint64_t partitionEntryLBA = *reinterpret_cast<uint64_t*>(buffer.data() + 72);
    uint32_t numPartitionEntries = *reinterpret_cast<uint32_t*>(buffer.data() + 80);
    uint32_t partitionEntrySize = *reinterpret_cast<uint32_t*>(buffer.data() + 84);
    ReadResult res{};

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
        else if (guid1 == 0xE6D6D379) info.type = "Linux LVM";
        else if (guid1 == 0xC12A7328) info.type = "EFI System";
        else if (guid1 == 0xAF9B60A0) info.type = "LDM Data";
        else if (guid1 == 0x5808C8AA) info.type = "LDM Metadata";
        else if (guid1 == 0xE75CAF8F) info.type = "Storage Spaces";
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

std::vector<PartitionInfo> PartitionScanner::parseTables() {
    auto gpt = parseGPT();
    if (!gpt.empty()) return gpt;
    auto mbr = parseMBR();
    if (!mbr.empty()) return mbr;
    return parseAPM();
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
        else if (looksLikeLvm2Label(buffer.data(), buffer.size())) {
            uint64_t labSec = 0;
            std::memcpy(&labSec, buffer.data() + 8, 8);
            info.startSector = (sector >= labSec) ? sector - labSec : sector;
            info.type = "LVM2 PV";
            found = true;
        }
        else if (looksLikeSpacedb(buffer.data(), buffer.size())) {
            info.type = "Storage Spaces";
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
