#include "fs/unallocated_map.h"
#include "fs/ntfs_util.h"
#include "byteback_db.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace byteback {

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

#pragma pack(push, 1)
struct ExFAT_BPB {
    uint8_t  jmp[3];
    char     oemName[8];
    uint8_t  reserved[53];
    uint64_t partitionOffset;
    uint64_t volumeLength;
    uint32_t fatOffset;
    uint32_t fatLength;
    uint32_t clusterHeapOffset;
    uint32_t clusterCount;
    uint32_t rootDirectoryCluster;
    uint32_t volumeSerialNumber;
    uint16_t fileSystemRevision;
    uint16_t volumeFlags;
    uint8_t  bytesPerSectorShift;
    uint8_t  sectorsPerClusterShift;
    uint8_t  numFats;
    uint8_t  driveSelect;
    uint8_t  percentInUse;
    uint8_t  reserved2[7];
};
#pragma pack(pop)

// CA-030: FAT tables are streamed through a sector-aligned sliding window.
// The old per-cluster path paid one heap allocation + one 512B read PER
// CLUSTER (30M clusters on an 8TB volume = 30M I/Os); the window costs one
// read per kFatWindowBytes. The window size stays a sector multiple so both
// the memory and physical backends accept the reads.
constexpr uint64_t kFatWindowBytes = 1u << 20;

struct FatWindow {
    DiskReader* reader = nullptr;
    uint64_t baseOffset = 0;   // byte offset of the FAT start on disk
    uint64_t tableBytes = 0;   // declared FAT size in bytes
    std::vector<uint8_t> buf;
    uint64_t bufStart = 0;     // table-relative offset of buf[0]
    uint64_t bufLen = 0;

    bool init(DiskReader& r, uint64_t fatBase, uint64_t fatBytes) {
        reader = &r;
        baseOffset = fatBase;
        tableBytes = fatBytes;
        if (fatBytes == 0) return false;
        buf.resize(static_cast<size_t>(std::min<uint64_t>(kFatWindowBytes, fatBytes)));
        return !buf.empty();
    }

    // Returns a pointer valid for at least 1 byte at table offset `idx`, or
    // nullptr past the table end / on a read failure.
    const uint8_t* at(uint64_t idx) {
        if (!reader || idx >= tableBytes) return nullptr;
        if (idx < bufStart || idx + 8 > bufStart + bufLen) {
            const uint64_t winStart = (idx / kFatWindowBytes) * kFatWindowBytes;
            const uint64_t len = std::min<uint64_t>(buf.size(), tableBytes - winStart);
            if (!reader->readSectors(baseOffset + winStart,
                                     static_cast<uint32_t>(len), buf.data()).success) {
                return nullptr;
            }
            bufStart = winStart;
            bufLen = len;
        }
        return buf.data() + (idx - bufStart);
    }
};

std::vector<SectorRange> buildExFatUnallocated(DiskReader& reader, uint64_t volOffsetBytes,
                                               uint64_t /*volSizeBytes*/) {
    std::vector<SectorRange> out;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t partitionOffset = volOffsetBytes / sectorSize;

    std::vector<uint8_t> buffer(sectorSize);
    if (!reader.readSectors(volOffsetBytes, sectorSize, buffer.data()).success) return out;
    if (std::memcmp(buffer.data() + 3, "EXFAT   ", 8) != 0) return out;

    const auto* bpb = reinterpret_cast<const ExFAT_BPB*>(buffer.data());
    // CA-030: the shifts cross a trust boundary — 1u << 255 is UB before the
    // old bytesPerSector check ever ran.
    if (bpb->bytesPerSectorShift > 16 || bpb->sectorsPerClusterShift > 31) return out;
    const uint32_t bytesPerSector = 1u << bpb->bytesPerSectorShift;
    if (bytesPerSector != sectorSize) return out;
    const uint32_t sectorsPerCluster = 1u << bpb->sectorsPerClusterShift;
    if (bpb->clusterCount == 0 || bpb->clusterHeapOffset == 0) return out;

    const uint64_t fatStartSector = partitionOffset + bpb->fatOffset;
    const uint64_t dataStartSector = partitionOffset + bpb->clusterHeapOffset;

    FatWindow fat;
    if (!fat.init(reader, fatStartSector * bytesPerSector,
                  static_cast<uint64_t>(bpb->fatLength) * bytesPerSector)) {
        return out;
    }
    auto readFatEntry = [&](uint64_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        const uint8_t* p = fat.at(cluster * 4);
        if (!p) return 0xFFFFFFFF;
        return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t)(p[3] << 24));
    };

    uint64_t runStartCluster = 0;
    uint64_t runLen = 0;
    const uint64_t lastCluster = (uint64_t)bpb->clusterCount + 1;
    for (uint64_t cluster = 2; cluster <= lastCluster; ++cluster) {
        const uint32_t entry = readFatEntry(cluster);
        const bool free = (entry == 0);
        if (free) {
            if (runLen == 0) runStartCluster = cluster;
            runLen++;
        } else if (runLen > 0) {
            pushRange(out, dataStartSector + (runStartCluster - 2) * sectorsPerCluster,
                      runLen * sectorsPerCluster);
            runLen = 0;
        }
    }
    if (runLen > 0) {
        pushRange(out, dataStartSector + (runStartCluster - 2) * sectorsPerCluster,
                  runLen * sectorsPerCluster);
    }
    mergeSectorRangesInPlace(out);
    return out;
}

#pragma pack(push, 1)
struct Ext4_SuperBlock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
};
struct Ext4_GroupDesc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
};
#pragma pack(pop)

std::vector<SectorRange> buildExt4Unallocated(DiskReader& reader, uint64_t volOffsetBytes,
                                                uint64_t /*volSizeBytes*/) {
    std::vector<SectorRange> out;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    std::vector<uint8_t> boot(sectorSize);
    if (!reader.readSectors(volOffsetBytes, sectorSize, boot.data()).success) return out;

    const uint64_t sbOff = volOffsetBytes + 1024;
    std::vector<uint8_t> sbBuf(sectorSize);
    if (!reader.readSectors(sbOff, sectorSize, sbBuf.data()).success) {
        return out;
    }
    const auto* sb = reinterpret_cast<const Ext4_SuperBlock*>(sbBuf.data());
    if (sb->s_magic != 0xEF53) return out;

    const uint32_t blockSize = 1024u << sb->s_log_block_size;
    if (blockSize == 0 || (blockSize % sectorSize) != 0) return out;
    const uint32_t sectorsPerBlock = blockSize / sectorSize;

    uint64_t blocksCount = sb->s_blocks_count_lo;
    if (blocksCount == 0) return out;
    const uint32_t blocksPerGroup = sb->s_blocks_per_group ? sb->s_blocks_per_group : 8192;
    const uint32_t numGroups =
        static_cast<uint32_t>((blocksCount + blocksPerGroup - 1) / blocksPerGroup);
    const uint32_t descSize = 32u;
    const uint64_t gdtBlock = (blockSize == 1024u) ? 2u
        : (sb->s_first_data_block ? sb->s_first_data_block : 1u);
    const uint64_t gdtOff = volOffsetBytes + gdtBlock * blockSize;

    for (uint32_t g = 0; g < numGroups; ++g) {
        std::vector<uint8_t> gdBuf(sectorSize);
        const uint64_t gdOff = gdtOff + static_cast<uint64_t>(g) * descSize;
        if (gdOff % sectorSize != 0 ||
            !reader.readSectors(gdOff - (gdOff % sectorSize), sectorSize, gdBuf.data()).success) {
            continue;
        }
        const auto* gd = reinterpret_cast<const Ext4_GroupDesc*>(gdBuf.data() + (gdOff % sectorSize));
        if (gd->bg_block_bitmap_lo == 0) continue;

        const uint32_t groupBlockStart = g * blocksPerGroup;
        const uint32_t blocksInGroup =
            static_cast<uint32_t>(std::min<uint64_t>(blocksPerGroup, blocksCount - groupBlockStart));
        const uint64_t bitmapOff = volOffsetBytes + static_cast<uint64_t>(gd->bg_block_bitmap_lo) * blockSize;
        const uint32_t bitmapBytes = (blocksInGroup + 7) / 8;
        const uint32_t readBytes =
            static_cast<uint32_t>(((std::max<uint32_t>(bitmapBytes, blockSize) + sectorSize - 1) / sectorSize) *
                                  sectorSize);
        std::vector<uint8_t> bitmap(readBytes);
        const uint64_t bitmapReadOff = bitmapOff - (bitmapOff % sectorSize);
        if (!reader.readSectors(bitmapReadOff, readBytes, bitmap.data()).success) continue;
        const size_t bitmapSkip = static_cast<size_t>(bitmapOff % sectorSize);

        uint64_t runStartBlock = UINT64_MAX;
        uint64_t runLen = 0;
        for (uint32_t b = 0; b < blocksInGroup; ++b) {
            const size_t byteIdx = bitmapSkip + b / 8;
            const uint8_t mask = static_cast<uint8_t>(1u << (b % 8));
            const bool allocated = byteIdx < bitmap.size() && (bitmap[byteIdx] & mask);
            if (!allocated) {
                if (runLen == 0) runStartBlock = groupBlockStart + b;
                runLen++;
            } else if (runLen > 0) {
                const uint64_t absBlock = runStartBlock;
                pushRange(out, (volOffsetBytes / sectorSize) + absBlock * sectorsPerBlock,
                          runLen * sectorsPerBlock);
                runLen = 0;
                runStartBlock = UINT64_MAX;
            }
        }
        if (runLen > 0) {
            pushRange(out, (volOffsetBytes / sectorSize) + runStartBlock * sectorsPerBlock,
                      runLen * sectorsPerBlock);
        }
    }
    mergeSectorRangesInPlace(out);
    return out;
}

std::vector<SectorRange> buildFatUnallocated(DiskReader& reader, uint64_t volOffsetBytes,
                                             uint64_t /*volSizeBytes*/) {
    std::vector<SectorRange> out;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t partitionOffset = volOffsetBytes / sectorSize;

    std::vector<uint8_t> buffer(sectorSize);
    if (!reader.readSectors(volOffsetBytes, sectorSize, buffer.data()).success) return out;
    if (std::memcmp(buffer.data() + 3, "EXFAT   ", 8) == 0) return out;

    const auto* bpb = reinterpret_cast<const FAT_BPB*>(buffer.data());
    uint16_t bps = bpb->bytesPerSector;
    if (bps == 0 || (bps & (bps - 1)) != 0) return out;
    const uint32_t spc = bpb->sectorsPerCluster;
    // CA-030: a zero or non-power-of-two sectorsPerCluster is a corrupt BPB —
    // the old code divided by it unguarded below.
    if (spc == 0 || (spc & (spc - 1)) != 0) return out;

    const uint64_t rootDirSectors = ((uint64_t)bpb->rootEntryCount * 32 + (bps - 1)) / bps;
    const uint64_t fatSize = (bpb->fatSize16 != 0) ? bpb->fatSize16 : bpb->fatSize32;
    if (fatSize == 0 || bpb->numFATs == 0) return out;
    const uint64_t totalSectors = (bpb->totalSectors16 != 0) ? bpb->totalSectors16 : bpb->totalSectors32;
    // CA-030: compute the metadata span in u64. A hostile BPB (fatSize huge,
    // totalSectors small) made the u32 subtraction wrap to ~4G dataSectors and
    // the cluster loop spin ~536M times with a 512B read each — an effective
    // hang. metaSectors >= totalSectors means the BPB describes no data area
    // at all: honest "unusable BPB" -> empty map (the scan falls back to
    // whole-partition carving via the CA-022 policy in collectUnallocatedForScan).
    const uint64_t metaSectors =
        bpb->reservedSectorCount + (uint64_t)bpb->numFATs * fatSize + rootDirSectors;
    if (totalSectors == 0 || metaSectors >= totalSectors) {
        std::fprintf(stderr,
                     "[byteback] FAT BPB unusable: meta sectors (%llu) >= total sectors (%llu); "
                     "skipping FAT unallocated map\n",
                     static_cast<unsigned long long>(metaSectors),
                     static_cast<unsigned long long>(totalSectors));
        return out;
    }
    const uint64_t fatStartSector = partitionOffset + bpb->reservedSectorCount;
    const uint64_t dataStartSector = fatStartSector + (uint64_t)bpb->numFATs * fatSize + rootDirSectors;
    const uint64_t dataSectors = totalSectors - metaSectors;
    const uint64_t countOfClusters = dataSectors / spc;
    const uint64_t fatBytes = fatSize * bps;
    const int fatBits = (countOfClusters < 4085) ? 12 : (countOfClusters < 65525 ? 16 : 32);

    // CA-030: one sliding-window read per MiB instead of one read per cluster,
    // and every entry index is bounds-checked against the declared table size.
    FatWindow fat;
    if (!fat.init(reader, fatStartSector * bps, fatBytes)) return out;
    auto readFatEntry = [&](uint64_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        uint64_t entryOffsetBytes = 0;
        if (fatBits == 12) {
            entryOffsetBytes = (cluster * 3) / 2;
        } else if (fatBits == 16) {
            entryOffsetBytes = cluster * 2;
        } else {
            entryOffsetBytes = cluster * 4;
        }
        const uint8_t* entBuf = fat.at(entryOffsetBytes);
        if (!entBuf) return 0xFFFFFFFF; // past the table / unreadable: allocated
        if (fatBits == 12) {
            // CA-030: a straddling FAT12 entry's second byte can lie one byte
            // past the table end (sector-straddling entries at the last byte
            // of the FAT overread the heap by one byte in the old path).
            if (entryOffsetBytes + 1 >= fatBytes) return 0xFFFFFFFF;
            uint16_t word = static_cast<uint16_t>(entBuf[0] | (entBuf[1] << 8));
            if (cluster & 1) word >>= 4;
            else word &= 0x0FFF;
            return word;
        }
        if (fatBits == 16) {
            return static_cast<uint32_t>(entBuf[0] | (entBuf[1] << 8));
        }
        return static_cast<uint32_t>(entBuf[0] | (entBuf[1] << 8) | (entBuf[2] << 16) | (uint32_t)(entBuf[3] << 24));
    };

    uint64_t runStartCluster = 0;
    uint64_t runLen = 0;
    for (uint64_t cluster = 2; cluster < countOfClusters + 2; ++cluster) {
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
        case VolumeFsKind::ExFat:
            return buildExFatUnallocated(reader, volumeOffsetBytes, volumeSizeBytes);
        case VolumeFsKind::Ext4:
            return buildExt4Unallocated(reader, volumeOffsetBytes, volumeSizeBytes);
        case VolumeFsKind::Refs:
        case VolumeFsKind::Apfs:
        case VolumeFsKind::Hfs:
        case VolumeFsKind::Unknown:
            return {};
    }
    return {};
}

namespace {
} // namespace

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
    // CA-022: when the unallocated map comes back empty — unsupported FS
    // (ReFS/APFS/HFS), unknown FS, or a corrupt filesystem — fall back to
    // carving the whole partition. The previous `sawUnsupported` gate kept the
    // range set empty, so deep scans on exactly the damaged/unusual volumes
    // this tool exists for carved zero sectors and still reported 100%. The
    // carver's bounded-emission policy keeps the fallback from flooding
    // results with phantom records.
    if (out.empty() && !parts.empty()) {
        for (const auto& part : parts) {
            if (part.sizeInSectors > 0) pushRange(out, part.startSector, part.sizeInSectors);
        }
    }
    mergeSectorRangesInPlace(out);
    return out;
}

uint64_t totalSectorCount(const std::vector<SectorRange>& ranges) {
    uint64_t total = 0;
    for (const auto& r : ranges) total += r.count;
    return total;
}

} // namespace byteback
