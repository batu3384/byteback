#include "fs/unallocated_map.h"
#include "fs/ntfs_util.h"
#include "wolf_db.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace wolf {

namespace {

void pushRange(std::vector<SectorRange>& out, uint64_t start, uint64_t count) {
    if (count == 0) return;
    out.push_back({start, count});
}

void mergeSectorRangesInPlace(std::vector<SectorRange>& ranges) {
    if (ranges.empty()) return;
    std::sort(ranges.begin(), ranges.end(),
              [](const SectorRange& a, const SectorRange& b) { return a.start < b.start; });
    std::vector<SectorRange> merged;
    merged.reserve(ranges.size());
    SectorRange cur = ranges[0];
    for (size_t i = 1; i < ranges.size(); ++i) {
        const SectorRange& r = ranges[i];
        if (r.start <= cur.start + cur.count) {
            uint64_t end = std::max(cur.start + cur.count, r.start + r.count);
            cur.count = end - cur.start;
        } else {
            merged.push_back(cur);
            cur = r;
        }
    }
    merged.push_back(cur);
    ranges.swap(merged);
}

std::vector<FileRecord::DataRun> ntfsUnnamedDataRuns(const uint8_t* rec, uint32_t recordSize,
                                                     uint64_t volumeStartSector,
                                                     uint32_t sectorsPerCluster) {
    std::vector<FileRecord::DataRun> out;
    if (!rec || recordSize < 64) return out;
#pragma pack(push, 1)
    struct MFT_RecordHeader {
        char signature[4];
        uint16_t updateSequenceOffset;
        uint16_t updateSequenceSize;
        uint64_t logFileSequenceNumber;
        uint16_t sequenceNumber;
        uint16_t hardLinkCount;
        uint16_t firstAttributeOffset;
        uint16_t flags;
        uint32_t usedSize;
        uint32_t allocatedSize;
        uint64_t baseRecordReference;
        uint16_t nextAttributeId;
    };
    struct NTFS_AttributeHeader {
        uint32_t type;
        uint32_t length;
        uint8_t nonResidentFlag;
        uint8_t nameLength;
        uint16_t nameOffset;
        uint16_t flags;
        uint16_t attributeId;
    };
    struct NTFS_NonResidentHeader {
        uint64_t startingVCN;
        uint64_t lastVCN;
        uint16_t dataRunOffset;
        uint16_t compressionUnit;
        uint32_t padding;
        uint64_t allocatedSize;
        uint64_t realSize;
        uint64_t initializedSize;
    };
#pragma pack(pop)

    const auto* header = reinterpret_cast<const MFT_RecordHeader*>(rec);
    uint32_t attrOffset = header->firstAttributeOffset;
    while (attrOffset + sizeof(NTFS_AttributeHeader) <= recordSize) {
        const auto* attr = reinterpret_cast<const NTFS_AttributeHeader*>(rec + attrOffset);
        if (attr->type == ntfs::ATTR_END_MARKER || attr->length == 0) break;
        if (attr->type == ntfs::ATTR_DATA && attr->nonResidentFlag != 0 && attr->nameLength == 0) {
            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_NonResidentHeader) > recordSize)
                break;
            const auto* nonRes = reinterpret_cast<const NTFS_NonResidentHeader*>(
                rec + attrOffset + sizeof(NTFS_AttributeHeader));
            size_t currentRunPos = attrOffset + nonRes->dataRunOffset;
            int64_t previousLcn = 0;
            while (currentRunPos < attrOffset + attr->length && currentRunPos < recordSize) {
                uint8_t headerByte = rec[currentRunPos];
                if (headerByte == 0x00) break;
                uint8_t lenSize = headerByte & 0x0F;
                uint8_t offSize = (headerByte >> 4) & 0x0F;
                currentRunPos++;
                if (currentRunPos + lenSize + offSize > recordSize) break;
                uint64_t clusterCount = 0;
                for (int j = 0; j < lenSize; j++)
                    clusterCount |= static_cast<uint64_t>(rec[currentRunPos + j]) << (j * 8);
                currentRunPos += lenSize;
                bool sparse = (offSize == 0);
                int64_t lcnOffset = 0;
                if (!sparse) {
                    for (int j = 0; j < offSize; j++)
                        lcnOffset |= static_cast<uint64_t>(rec[currentRunPos + j]) << (j * 8);
                    if (rec[currentRunPos + offSize - 1] & 0x80) {
                        for (int j = offSize; j < 8; j++)
                            lcnOffset |= static_cast<int64_t>(0xFF) << (j * 8);
                    }
                    previousLcn += lcnOffset;
                }
                currentRunPos += offSize;
                FileRecord::DataRun run;
                run.startSector = sparse ? UINT64_MAX
                    : volumeStartSector + static_cast<uint64_t>(previousLcn) * sectorsPerCluster;
                run.sectorCount = clusterCount * sectorsPerCluster;
                if (!sparse && run.sectorCount > 0) out.push_back(run);
            }
            break;
        }
        if (attr->length < sizeof(NTFS_AttributeHeader)) break;
        attrOffset += attr->length;
    }
    return out;
}

bool readBytesFromRuns(DiskReader& reader, const std::vector<FileRecord::DataRun>& runs,
                       uint64_t offset, uint32_t size, uint8_t* out) {
    if (!out || size == 0) return false;
    uint64_t filled = 0;
    uint64_t wantOff = offset;
    for (const auto& run : runs) {
        if (run.startSector == UINT64_MAX) continue;
        uint64_t runBytes = run.sectorCount * reader.getSectorSize();
        if (wantOff >= runBytes) {
            wantOff -= runBytes;
            continue;
        }
        uint32_t ss = reader.getSectorSize();
        if (ss == 0) ss = 512;
        uint64_t readOff = run.startSector * ss + wantOff;
        uint64_t avail = runBytes - wantOff;
        uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(size - filled, avail));
        if (!reader.readSectors(readOff, take, out + filled).success) return false;
        filled += take;
        wantOff = 0;
        if (filled >= size) return true;
    }
    return filled >= size;
}

std::vector<SectorRange> buildNtfsUnallocated(DiskReader& reader, uint64_t volOffsetBytes,
                                              uint64_t volSizeBytes) {
    std::vector<SectorRange> out;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t volumeStartSector = volOffsetBytes / sectorSize;

    std::vector<uint8_t> boot(sectorSize);
    if (!reader.readSectors(volOffsetBytes, sectorSize, boot.data()).success) return out;

    uint32_t bps = sectorSize;
    uint32_t spc = 8;
    uint64_t mftLcn = 0;
    uint32_t recBytes = 1024;
    if (!ntfs::parseNtfsBoot(boot.data(), boot.size(), bps, spc, mftLcn, recBytes) || mftLcn == 0) {
        return out;
    }

    const uint64_t clusterBytes = static_cast<uint64_t>(bps) * spc;
    uint64_t volSectors = volSizeBytes > 0 ? volSizeBytes / sectorSize
                                           : reader.getDiskSize() / sectorSize - volumeStartSector;
    uint64_t totalClusters = (volSectors + spc - 1) / spc;

    std::vector<uint8_t> rec(recBytes, 0);
    const uint64_t bitmapOff = volOffsetBytes + mftLcn * clusterBytes + 6ull * recBytes;
    if (!reader.readSectors(bitmapOff, recBytes, rec.data()).success) return out;
    if (std::strncmp(reinterpret_cast<char*>(rec.data()), "FILE", 4) != 0) return out;

    const auto* hdr = reinterpret_cast<const uint16_t*>(rec.data() + 4);
    uint16_t usaOff = hdr[0];
    uint16_t usaCnt = hdr[1];
    ntfs::applyUsaFixup(rec.data(), recBytes, sectorSize, usaOff, usaCnt);

    auto runs = ntfsUnnamedDataRuns(rec.data(), recBytes, volumeStartSector, spc);
    std::vector<uint8_t> bitmap;
    if (!runs.empty()) {
        uint64_t bitmapBytes = (totalClusters + 7) / 8;
        bitmap.resize(static_cast<size_t>(bitmapBytes));
        if (!readBytesFromRuns(reader, runs, 0, static_cast<uint32_t>(bitmapBytes), bitmap.data())) {
            return out;
        }
    } else {
        // Resident $DATA fallback for tiny volumes.
#pragma pack(push, 1)
        struct NTFS_AttributeHeader {
            uint32_t type;
            uint32_t length;
            uint8_t nonResidentFlag;
            uint8_t nameLength;
            uint16_t nameOffset;
            uint16_t flags;
            uint16_t attributeId;
        };
        struct NTFS_ResidentAttributeHeader {
            uint32_t valueLength;
            uint16_t valueOffset;
            uint8_t indexedFlag;
            uint8_t padding;
        };
#pragma pack(pop)
        const auto* mftHdr = reinterpret_cast<const uint8_t*>(rec.data());
        uint32_t attrOffset = *reinterpret_cast<const uint16_t*>(mftHdr + 20);
        while (attrOffset + sizeof(NTFS_AttributeHeader) <= recBytes) {
            const auto* attr = reinterpret_cast<const NTFS_AttributeHeader*>(rec.data() + attrOffset);
            if (attr->type == ntfs::ATTR_END_MARKER || attr->length == 0) break;
            if (attr->type == ntfs::ATTR_DATA && attr->nonResidentFlag == 0 && attr->nameLength == 0) {
                const auto* res = reinterpret_cast<const NTFS_ResidentAttributeHeader*>(
                    rec.data() + attrOffset + sizeof(NTFS_AttributeHeader));
                size_t valOff = attrOffset + res->valueOffset;
                size_t valLen = res->valueLength;
                if (valOff + valLen <= recBytes) {
                    bitmap.assign(rec.data() + valOff, rec.data() + valOff + valLen);
                }
                break;
            }
            if (attr->length < sizeof(NTFS_AttributeHeader)) break;
            attrOffset += attr->length;
        }
    }
    if (bitmap.empty()) return out;

    uint64_t runStart = UINT64_MAX;
    uint64_t runClusters = 0;
    for (uint64_t c = 0; c < totalClusters; ++c) {
        size_t byteIdx = static_cast<size_t>(c / 8);
        uint8_t bit = static_cast<uint8_t>(1u << (c % 8));
        bool allocated = byteIdx < bitmap.size() && (bitmap[byteIdx] & bit);
        if (!allocated) {
            if (runClusters == 0) runStart = c;
            runClusters++;
        } else if (runClusters > 0) {
            pushRange(out, volumeStartSector + runStart * spc, runClusters * spc);
            runClusters = 0;
            runStart = UINT64_MAX;
        }
    }
    if (runClusters > 0) {
        pushRange(out, volumeStartSector + runStart * spc, runClusters * spc);
    }
    mergeSectorRangesInPlace(out);
    return out;
}

#pragma pack(push, 1)
struct FAT_BPB {
    uint8_t  jmp[3];
    char     oemName[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t  numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t  media;
    uint16_t fatSize16;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    uint32_t fatSize32;
    uint16_t extFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;
    uint16_t fsInfo;
    uint16_t bkBootSec;
    uint8_t  reserved[12];
    uint8_t  drvNum;
    uint8_t  reserved1;
    uint8_t  bootSig;
    uint32_t volId;
    char     volLab[11];
    char     filSysType[8];
};
#pragma pack(pop)

std::vector<SectorRange> buildFatUnallocated(DiskReader& reader, uint64_t volOffsetBytes,
                                             uint64_t /*volSizeBytes*/) {
    std::vector<SectorRange> out;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t partitionOffset = volOffsetBytes / sectorSize;

    std::vector<uint8_t> buffer(sectorSize);
    if (!reader.readSectors(volOffsetBytes, sectorSize, buffer.data()).success) return out;
    if (std::memcmp(buffer.data() + 3, "EXFAT   ", 8) == 0) return out; // ponytail: exFAT later

    const auto* bpb = reinterpret_cast<const FAT_BPB*>(buffer.data());
    uint16_t bps = bpb->bytesPerSector;
    if (bps == 0 || (bps & (bps - 1)) != 0) return out;

    uint32_t rootDirSectors = ((bpb->rootEntryCount * 32) + (bps - 1)) / bps;
    uint32_t fatSize = (bpb->fatSize16 != 0) ? bpb->fatSize16 : bpb->fatSize32;
    uint64_t fatStartSector = partitionOffset + bpb->reservedSectorCount;
    uint64_t dataStartSector = fatStartSector + (bpb->numFATs * fatSize) + rootDirSectors;
    uint32_t totalSectors = (bpb->totalSectors16 != 0) ? bpb->totalSectors16 : bpb->totalSectors32;
    uint32_t dataSectors = totalSectors - (bpb->reservedSectorCount + (bpb->numFATs * fatSize) + rootDirSectors);
    uint32_t countOfClusters = dataSectors / bpb->sectorsPerCluster;
    int fatBits = (countOfClusters < 4085) ? 12 : (countOfClusters < 65525 ? 16 : 32);

    auto readFatEntry = [&](uint32_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        uint32_t entryIndex = cluster;
        uint64_t fatOffsetBytes = 0;
        if (fatBits == 12) {
            fatOffsetBytes = fatStartSector * bps + (entryIndex * 3) / 2;
        } else if (fatBits == 16) {
            fatOffsetBytes = fatStartSector * bps + entryIndex * 2;
        } else {
            fatOffsetBytes = fatStartSector * bps + entryIndex * 4;
        }
        uint64_t sectorOff = (fatOffsetBytes / bps) * bps;
        uint32_t inSector = static_cast<uint32_t>(fatOffsetBytes % bps);
        std::vector<uint8_t> sec(bps);
        if (!reader.readSectors(sectorOff, bps, sec.data()).success) return 0xFFFFFFFF;
        const uint8_t* entBuf = sec.data() + inSector;
        if (fatBits == 12) {
            uint16_t word = static_cast<uint16_t>(entBuf[0] | (entBuf[1] << 8));
            if (entryIndex & 1) word >>= 4;
            else word &= 0x0FFF;
            return word;
        }
        if (fatBits == 16) {
            if (inSector + 2 > bps) {
                std::vector<uint8_t> sec2(bps);
                if (!reader.readSectors(sectorOff + bps, bps, sec2.data()).success) return 0xFFFFFFFF;
                uint16_t lo = sec[inSector];
                uint16_t hi = sec2[0];
                return static_cast<uint32_t>(lo | (hi << 8));
            }
            return static_cast<uint32_t>(entBuf[0] | (entBuf[1] << 8));
        }
        if (inSector + 4 > bps) return 0xFFFFFFFF;
        return static_cast<uint32_t>(entBuf[0] | (entBuf[1] << 8) | (entBuf[2] << 16) | (entBuf[3] << 24));
    };

    uint64_t runStartCluster = 0;
    uint64_t runLen = 0;
    const uint32_t spc = bpb->sectorsPerCluster;
    for (uint32_t cluster = 2; cluster < countOfClusters + 2; ++cluster) {
        uint32_t entry = readFatEntry(cluster);
        bool free = (entry == 0);
        if (free) {
            if (runLen == 0) runStartCluster = cluster;
            runLen++;
        } else if (runLen > 0) {
            pushRange(out, dataStartSector + (runStartCluster - 2) * spc, runLen * spc);
            runLen = 0;
        }
    }
    if (runLen > 0) {
        pushRange(out, dataStartSector + (runStartCluster - 2) * spc, runLen * spc);
    }
    mergeSectorRangesInPlace(out);
    return out;
}

} // namespace

void mergeSectorRanges(std::vector<SectorRange>& ranges) {
    mergeSectorRangesInPlace(ranges);
}

std::vector<SectorRange> buildUnallocatedRanges(DiskReader& reader, VolumeFsKind kind,
                                                uint64_t volumeOffsetBytes,
                                                uint64_t volumeSizeBytes) {
    switch (kind) {
        case VolumeFsKind::Ntfs:
            return buildNtfsUnallocated(reader, volumeOffsetBytes, volumeSizeBytes);
        case VolumeFsKind::Fat:
            return buildFatUnallocated(reader, volumeOffsetBytes, volumeSizeBytes);
        default:
            return {};
    }
}

std::vector<SectorRange> collectUnallocatedForScan(DiskReader& reader,
                                                   int64_t partitionStartSector,
                                                   uint64_t partitionSizeSectors) {
    std::vector<SectorRange> out;
    if (!reader.isOpen() && !reader.hasRaidBackend()) return out;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    std::vector<PartitionInfo> parts;
    if (partitionStartSector >= 0 && partitionSizeSectors > 0) {
        PartitionInfo p;
        p.startSector = static_cast<uint64_t>(partitionStartSector);
        p.sizeInSectors = partitionSizeSectors;
        parts.push_back(p);
    } else {
        PartitionScanner scanner(&reader);
        parts = scanner.parseMBR();
        std::vector<PartitionInfo> gpt = scanner.parseGPT();
        if (!gpt.empty()) parts = std::move(gpt);
        if (parts.empty()) {
            PartitionInfo whole;
            whole.startSector = 0;
            whole.sizeInSectors = reader.getDiskSize() / sectorSize;
            parts.push_back(whole);
        }
    }

    for (const auto& part : parts) {
        if (part.sizeInSectors == 0) continue;
        uint64_t offsetBytes = part.startSector * sectorSize;
        uint64_t sizeBytes = part.sizeInSectors * sectorSize;
        VolumeFsKind kind = probeVolumeAt(reader, offsetBytes, sectorSize);
        auto u = buildUnallocatedRanges(reader, kind, offsetBytes, sizeBytes);
        out.insert(out.end(), u.begin(), u.end());
    }
    mergeSectorRangesInPlace(out);
    return out;
}

} // namespace wolf
