#include "byteback_fs.h"
#include "fs/ntfs_util.h"
#include "fs/fat_chain.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <unordered_set>

#pragma pack(push, 1)

// FAT12/16/32 Boot Sector (BPB)
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
    // FAT32 specific
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

struct FAT_DirEntry {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntRes;
    uint8_t  crtTimeTenth;
    uint16_t crtTime;
    uint16_t crtDate;
    uint16_t lstAccDate;
    uint16_t fstClusHI;
    uint16_t wrtTime;
    uint16_t wrtDate;
    uint16_t fstClusLO;
    uint32_t fileSize;
};

struct FAT_LFNEntry {
    uint8_t  ord;
    uint16_t name1[5];
    uint8_t  attr; // 0x0F
    uint8_t  type; // 0x00
    uint8_t  chksum;
    uint16_t name2[6];
    uint16_t fstClusLO;
    uint16_t name3[2];
};

// exFAT Directory Entries
struct ExFAT_GenericEntry {
    uint8_t entryType;
    uint8_t data[31];
};

#pragma pack(pop)

namespace byteback {


namespace {
// CA-002 fix: convert a FAT/exFAT cluster chain into physical sector runs.
// The chain math (EOC/bad thresholds, cycle guard, sector mapping) lives in
// fs/fat_chain.cpp and is unit-tested; this adapter only reads FAT entries
// off the disk and translates run shapes.
std::vector<FileRecord::DataRun> buildRunsFromChain(
    DiskReader& reader, uint64_t fatStartSector, uint32_t bytesPerSector,
    int fatBits, uint32_t firstCluster, uint32_t sectorsPerCluster,
    uint64_t dataStartSector, size_t maxClusters, bool* chainUnread = nullptr) {

    fat::FatEntryReader readEntry = [&](uint32_t cluster) -> uint32_t {
        uint64_t byteOffset = (fatBits == 12) ? (uint64_t)cluster * 3 / 2
                          : (fatBits == 16) ? (uint64_t)cluster * 2
                          : (uint64_t)cluster * 4;
        uint64_t sector = fatStartSector + byteOffset / bytesPerSector;
        uint32_t off = (uint32_t)(byteOffset % bytesPerSector);
        if (off + 4 > bytesPerSector && fatBits == 32) return fat::kFatUnread;
        std::vector<uint8_t> sec(bytesPerSector);
        auto res = reader.readSectors(sector * bytesPerSector, bytesPerSector, sec.data());
        if (!readComplete(res, bytesPerSector)) return fat::kFatUnread;
        if (fatBits == 32) return (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8) | ((uint32_t)sec[off+2] << 16) | ((uint32_t)sec[off+3] << 24);
        if (fatBits == 16) return (uint32_t)((uint16_t)sec[off] | ((uint16_t)sec[off+1] << 8));
        // FAT12
        uint32_t v = (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8);
        return (cluster & 1) ? (v >> 4) : (v & 0xFFF);
    };

    bool unread = false;
    auto chain = fat::chainRuns(std::move(readEntry), fatBits, firstCluster,
                                sectorsPerCluster, dataStartSector, maxClusters, &unread);
    if (chainUnread && unread) *chainUnread = true;
    std::vector<FileRecord::DataRun> runs;
    runs.reserve(chain.size());
    for (const auto& r : chain) {
        runs.push_back({r.startSector, r.sectorCount});
    }
    return runs;
}

uint64_t fatRunBytes(const std::vector<FileRecord::DataRun>& runs, uint32_t bytesPerSector) {
    uint64_t n = 0;
    for (const auto& r : runs) n += r.sectorCount * bytesPerSector;
    return n;
}

uint32_t readFatEntry(DiskReader& reader, uint64_t fatStartSector, uint32_t bytesPerSector,
                      int fatBits, uint32_t cluster) {
    if (cluster < 2 || bytesPerSector == 0) return 0;
    uint64_t byteOffset = (fatBits == 12) ? (uint64_t)cluster * 3 / 2
                      : (fatBits == 16) ? (uint64_t)cluster * 2
                      : (uint64_t)cluster * 4;
    uint64_t sector = fatStartSector + byteOffset / bytesPerSector;
    uint32_t off = (uint32_t)(byteOffset % bytesPerSector);
    if (off + 4 > bytesPerSector && fatBits == 32) return fat::kFatUnread;
    std::vector<uint8_t> sec(bytesPerSector);
    auto res = reader.readSectors(sector * bytesPerSector, bytesPerSector, sec.data());
    if (!readComplete(res, bytesPerSector)) return fat::kFatUnread;
    if (fatBits == 32) return (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8) |
                               ((uint32_t)sec[off+2] << 16) | ((uint32_t)sec[off+3] << 24);
    if (fatBits == 16) return (uint32_t)((uint16_t)sec[off] | ((uint16_t)sec[off+1] << 8));
    uint32_t v = (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8);
    return (cluster & 1) ? (v >> 4) : (v & 0xFFF);
}

bool fatFirstClusterFreed(uint32_t fatVal, int fatBits) {
    if (fatVal == fat::kFatUnread) return false;
    if (fatBits == 32) return (fatVal & 0x0FFFFFFFu) == 0;
    if (fatBits == 16) return (fatVal & 0xFFFFu) == 0;
    return (fatVal & 0xFFFu) == 0;
}

void applyFatDeletedChainHint(FileRecord& fr, uint64_t fileSize, uint32_t bytesPerSector, bool deleted) {
    if (!deleted || fileSize == 0) return;
    const uint64_t covered = fatRunBytes(fr.runs, bytesPerSector);
    if (covered + bytesPerSector < fileSize) {
        fr.confidence = std::min(fr.confidence, 35);
    }
}

void setRunsFromChain(FileRecord& fr, std::vector<FileRecord::DataRun> runs) {
    fr.runs = std::move(runs);
    if (fr.runs.empty()) return;
    fr.startSector = fr.runs.front().startSector;
    uint64_t end = fr.startSector;
    for (const auto& r : fr.runs) {
        const uint64_t runEnd = r.startSector + r.sectorCount;
        if (runEnd > end) end = runEnd;
    }
    fr.endSector = end;
}

bool clusterStartsNewFile(DiskReader& reader, uint64_t startSector, uint32_t bytesPerSector) {
    if (bytesPerSector < 8) return false;
    std::vector<uint8_t> sec(bytesPerSector);
    auto res = reader.readSectors(startSector * bytesPerSector, bytesPerSector, sec.data());
    if (ioUnread(res, 8)) return true;
    if (sec[0] == 0xFF && sec[1] == 0xD8) return true;
    if (sec[0] == 0x89 && sec[1] == 'P' && sec[2] == 'N' && sec[3] == 'G') return true;
    if (sec[0] == 'P' && sec[1] == 'K') return true;
    if (sec[0] == '%' && sec[1] == 'P' && sec[2] == 'D' && sec[3] == 'F') return true;
    return false;
}

bool applyBackupFatChain(FileRecord& fr, DiskReader& reader, uint64_t fatStartSector,
                         uint32_t fatSizeSectors, uint8_t numFats, uint32_t bytesPerSector,
                         int fatBits, uint32_t firstCluster, uint32_t sectorsPerCluster,
                         uint64_t dataStartSector, uint64_t fileSize, bool* unread) {
    if (numFats < 2 || fatSizeSectors == 0 || fileSize == 0) return false;
    for (uint8_t i = 1; i < numFats; ++i) {
        bool u = false;
        auto runs = buildRunsFromChain(reader,
                                       fatStartSector + static_cast<uint64_t>(i) * fatSizeSectors,
                                       bytesPerSector, fatBits, firstCluster, sectorsPerCluster,
                                       dataStartSector, 1u << 20, &u);
        if (fatRunBytes(runs, bytesPerSector) < fileSize) continue;
        setRunsFromChain(fr, std::move(runs));
        if (u && unread) *unread = true;
        return true;
    }
    return false;
}

// Deleted dirent still names firstCluster + size; FAT often already 0. chainRuns
// then stops after one cluster. Contiguous undelete from the first cluster,
// but stop before a cluster that looks like a different file (JPEG SOI, etc).
void fillDeletedFatRuns(FileRecord& fr, DiskReader& reader, uint32_t firstCluster,
                        uint64_t fileSize, uint32_t sectorsPerCluster,
                        uint64_t dataStartSector, uint32_t bytesPerSector) {
    if (firstCluster < 2 || fileSize == 0 || sectorsPerCluster == 0 || bytesPerSector == 0) return;
    const uint64_t clusterBytes = static_cast<uint64_t>(sectorsPerCluster) * bytesPerSector;
    uint64_t nClus = (fileSize + clusterBytes - 1) / clusterBytes;
    if (nClus == 0) nClus = 1;
    if (nClus > (1u << 20)) nClus = 1u << 20;
    uint64_t keep = 1;
    for (uint64_t i = 1; i < nClus; ++i) {
        const uint32_t cl = firstCluster + static_cast<uint32_t>(i);
        const uint64_t sec = dataStartSector + static_cast<uint64_t>(cl - 2) * sectorsPerCluster;
        if (clusterStartsNewFile(reader, sec, bytesPerSector)) break;
        keep = i + 1;
    }
    const uint64_t startSec = dataStartSector + static_cast<uint64_t>(firstCluster - 2) * sectorsPerCluster;
    setRunsFromChain(fr, {{startSec, keep * sectorsPerCluster}});
}
} // namespace

FATParser::FATParser() {}
FATParser::~FATParser() {}

static std::string formatFATName(const uint8_t name[11]) {
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (name[i] == ' ' || name[i] == 0) break;
        s += static_cast<char>(name[i]);
    }
    std::string ext;
    for (int i = 8; i < 11; ++i) {
        if (name[i] == ' ' || name[i] == 0) break;
        ext += static_cast<char>(name[i]);
    }
    if (!ext.empty()) s += "." + ext;
    return s;
}

static std::string formatFatVolumeLabel(const uint8_t name[11]) {
    int n = 11;
    while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == 0)) --n;
    if (n <= 0) return {};
    return std::string(reinterpret_cast<const char*>(name), static_cast<size_t>(n));
}

// VFAT short-name checksum (used to validate that a chain of LFN entries
// belongs to the following 8.3 entry). Algorithm per Microsoft FAT spec.
static uint8_t lfnChecksum(const uint8_t shortName[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = ((sum & 1) << 7) + (sum >> 1) + shortName[i];
    }
    return sum;
}

// Reassemble UTF-16 LFN fragments collected in reverse order into a UTF-8-ish
// string. We only emit the BMP subset that fits in a single byte (ASCII range
// and Latin-1); other code points are replaced with '?' to stay compatible with
// the existing ASCII-only pipeline. A full UTF-8 transcoder is on the roadmap
// (Faz 1, NTFS UTF-16 names).
static std::string lfnToString(const std::vector<uint16_t>& utf16) {
    size_t n = utf16.size();
    while (n > 0 && (utf16[n - 1] == 0 || utf16[n - 1] == 0xFFFF)) --n;
    if (n == 0) return {};
    return ntfs::utf16leToUtf8(utf16.data(), n);
}

struct LfnAcc {
    std::map<uint8_t, std::array<uint16_t, 13>> fragments;
    uint8_t checksumSeen = 0;
    std::vector<std::array<uint16_t, 13>> deletedSeq;

    void clear() {
        fragments.clear();
        deletedSeq.clear();
        checksumSeen = 0;
    }

    void add(const FAT_LFNEntry* lfn, bool deleted) {
        std::array<uint16_t, 13> chars{};
        for (int i = 0; i < 5; ++i) chars[i] = lfn->name1[i];
        for (int i = 0; i < 6; ++i) chars[5 + i] = lfn->name2[i];
        for (int i = 0; i < 2; ++i) chars[11 + i] = lfn->name3[i];
        if (deleted) {
            deletedSeq.push_back(chars);
            return;
        }
        fragments[lfn->ord & 0x3F] = chars;
        checksumSeen = lfn->chksum;
    }

    std::string resolve(const uint8_t shortName[11], bool deleted) const {
        if (!fragments.empty() && lfnChecksum(shortName) == checksumSeen) {
            std::vector<uint16_t> full;
            for (const auto& kv : fragments)
                for (uint16_t c : kv.second) full.push_back(c);
            std::string s = lfnToString(full);
            if (!s.empty()) return s;
        }
        if (deleted && !deletedSeq.empty()) {
            std::vector<uint16_t> full;
            for (auto it = deletedSeq.rbegin(); it != deletedSeq.rend(); ++it)
                for (uint16_t c : *it) full.push_back(c);
            return lfnToString(full);
        }
        return {};
    }
};

void emitFatVolumeLabel(FileSystemParser::FileRecordCallback& callback,
                        const uint8_t name[11], uint64_t startSector, int status) {
    std::string label = formatFatVolumeLabel(name);
    if (label.empty()) return;
    FileRecord fr;
    fr.id = -1;
    fr.name = std::move(label);
    fr.path = "/";
    fr.status = status;
    fr.confidence = 5;
    fr.category = "System";
    fr.source = "fat_vol_label";
    fr.startSector = startSector;
    fr.endSector = startSector + 1;
    callback(fr);
}

void emitFatDirUnread(FileSystemParser::FileRecordCallback& callback, uint64_t startSector) {
    FileRecord fr;
    fr.id = -1;
    fr.name = "FAT_DirectoryUnread";
    fr.path = "/fat-dir-unread/";
    fr.status = 0;
    fr.confidence = 20;
    fr.category = "System";
    fr.source = "fat_dir_unread";
    fr.startSector = startSector;
    fr.endSector = startSector + 1;
    callback(fr);
}

void emitFatChainUnread(FileSystemParser::FileRecordCallback& callback, uint64_t startSector) {
    FileRecord fr;
    fr.id = -1;
    fr.name = "FAT_ChainUnread";
    fr.path = "/fat-chain-unread/";
    fr.status = 0;
    fr.confidence = 20;
    fr.category = "System";
    fr.source = "fat_chain_unread";
    fr.startSector = startSector;
    fr.endSector = startSector + 1;
    callback(fr);
}

bool FATParser::scan(byteback::DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0);
}

bool FATParser::scanAt(byteback::DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning,
                       uint64_t partitionOffsetBytes) {
    if (!reader.isOpen()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    // The internal parseFAT/parseExFAT routines work in partition-relative
    // SECTOR units; convert the caller-supplied byte offset once here.
    uint64_t partitionOffset = partitionOffsetBytes / sectorSize;

    std::vector<uint8_t> buffer(sectorSize);

    auto res = reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    if (!readComplete(res, sectorSize)) return false;

    auto looksExfat = [](const uint8_t* b) {
        return std::memcmp(b + 3, "EXFAT   ", 8) == 0;
    };
    if (looksExfat(buffer.data())) {
        parseExFAT(reader, partitionOffset, callback, isRunning);
    } else {
        std::vector<uint8_t> backup(sectorSize);
        const uint64_t backupSec = partitionOffset + 12;
        if (readComplete(reader.readSectors(backupSec * sectorSize, sectorSize, backup.data()),
                         sectorSize) &&
            looksExfat(backup.data())) {
            parseExFAT(reader, partitionOffset, callback, isRunning);
        } else {
            parseFAT(reader, partitionOffset, callback, isRunning);
        }
    }
    return true;
}

void FATParser::parseFAT(byteback::DiskReader& reader, uint64_t partitionOffset, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    auto loadBpb = [&](uint64_t absSector) {
        return readComplete(reader.readSectors(absSector * sectorSize, sectorSize, buffer.data()),
                            sectorSize);
    };
    if (!loadBpb(partitionOffset)) return;

    auto bpbOk = [&]() {
        const auto* b = reinterpret_cast<const FAT_BPB*>(buffer.data());
        const uint16_t bps = b->bytesPerSector;
        return bps != 0 && (bps & (bps - 1)) == 0 && b->sectorsPerCluster != 0;
    };
    if (!bpbOk()) {
        uint16_t backupSec = reinterpret_cast<const FAT_BPB*>(buffer.data())->bkBootSec;
        if (backupSec == 0 || backupSec > 256) backupSec = 6;
        if (backupSec == 0 || !loadBpb(partitionOffset + backupSec) || !bpbOk()) return;
    }

    FAT_BPB* bpb = reinterpret_cast<FAT_BPB*>(buffer.data());
    
    uint16_t bps = bpb->bytesPerSector;
    if (bps == 0 || (bps & (bps - 1)) != 0) return; // Invalid BPS
    if (bpb->sectorsPerCluster == 0) return; // div-by-zero on malformed BPB
    
    uint32_t rootDirSectors = ((bpb->rootEntryCount * 32) + (bps - 1)) / bps;
    uint32_t fatSize = (bpb->fatSize16 != 0) ? bpb->fatSize16 : bpb->fatSize32;
    
    uint64_t fatStartSector = partitionOffset + bpb->reservedSectorCount;
    uint64_t rootDirStartSector = fatStartSector + (bpb->numFATs * fatSize);
    uint64_t dataStartSector = rootDirStartSector + rootDirSectors;
    
    uint32_t totalSectors = (bpb->totalSectors16 != 0) ? bpb->totalSectors16 : bpb->totalSectors32;
    uint32_t dataSectors = totalSectors - (bpb->reservedSectorCount + (bpb->numFATs * fatSize) + rootDirSectors);
    uint32_t countOfClusters = dataSectors / bpb->sectorsPerCluster;
    
    bool isFat32 = countOfClusters >= 65525;
    bool chainUnread = false;
    
    std::vector<uint32_t> dirClusters;
    if (isFat32) {
        dirClusters.push_back(bpb->rootCluster);
    } else {
        // Parse FAT16 root dir sequentially. Long File Name (LFN) entries
        // precede their 8.3 short entry in reverse ordinal order; we buffer
        // them and validate via checksum when the short entry arrives.
        std::vector<uint8_t> rootBuf(rootDirSectors * bps);
        auto rootRes = reader.readSectors(rootDirStartSector * bps, rootDirSectors * bps, rootBuf.data());
        if (!readComplete(rootRes, rootBuf.size())) {
            emitFatDirUnread(callback, rootDirStartSector);
            return;
        }

        // LFN accumulation state. Indexed by ordinal so fragments reassemble in
        // forward order regardless of how many 13-char segments were needed.
        LfnAcc lfn;

        for (uint32_t offset = 0; offset < rootBuf.size(); offset += 32) {
            FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(rootBuf.data() + offset);
            if (entry->name[0] == 0x00) break;

            bool deleted = (entry->name[0] == 0xE5);

            if (entry->attr == 0x0F) {
                lfn.add(reinterpret_cast<FAT_LFNEntry*>(entry), deleted);
                continue;
            }

            if ((entry->attr & 0x08) != 0 && (entry->attr & 0x10) == 0) {
                int volStatus = deleted ? 0 : 1;
                if (deleted) entry->name[0] = '_';
                emitFatVolumeLabel(callback, entry->name, rootDirStartSector, volStatus);
                lfn.clear();
                continue;
            }

            int status = 1;
            int confidence = 100;
            std::string lfnName = lfn.resolve(entry->name, deleted);
            if (deleted) {
                status = 0;
                confidence = 60;
                entry->name[0] = '_';
            }

            std::string name = lfnName.empty() ? formatFATName(entry->name) : lfnName;
            lfn.clear();

            uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
            if (entry->attr & 0x10) {
                if (name != "." && name != "..") dirClusters.push_back(firstCluster);
            } else {
                // CA-002: emit real sector runs from the FAT chain instead of
                // leaking the raw cluster number into startSector.
                int fatBits = (countOfClusters < 4085) ? 12 : 16;
                bool thisUnread = false;
                auto runs16 = buildRunsFromChain(reader, fatStartSector, bps, fatBits,
                                                 firstCluster, bpb->sectorsPerCluster,
                                                 dataStartSector, 65536, &thisUnread);
                FileRecord fr;
                fr.id = -1; // Will be set by DB
                fr.parentId = 0;
                fr.name = name;
                fr.sizeBytes = entry->fileSize;
                fr.startSector = runs16.empty()
                    ? dataStartSector + (uint64_t)(std::max<uint32_t>(firstCluster, 2) - 2) * bpb->sectorsPerCluster
                    : runs16.front().startSector;
                fr.endSector = fr.startSector + (uint32_t)((entry->fileSize + bps * bpb->sectorsPerCluster - 1) / (bps * bpb->sectorsPerCluster)) * bpb->sectorsPerCluster;
                fr.runs = std::move(runs16);
                fr.path = "/";
                fr.status = status;
                fr.confidence = confidence;
                if (deleted && fatRunBytes(fr.runs, bps) < entry->fileSize &&
                    fatFirstClusterFreed(readFatEntry(reader, fatStartSector, bps, fatBits, firstCluster), fatBits)) {
                    if (!applyBackupFatChain(fr, reader, fatStartSector, fatSize, bpb->numFATs, bps,
                                             fatBits, firstCluster, bpb->sectorsPerCluster,
                                             dataStartSector, entry->fileSize, &thisUnread)) {
                        fillDeletedFatRuns(fr, reader, firstCluster, entry->fileSize,
                                           bpb->sectorsPerCluster, dataStartSector, bps);
                    }
                }
                applyFatDeletedChainHint(fr, entry->fileSize, bps, deleted);
                if (thisUnread) {
                    chainUnread = true;
                    fr.confidence = std::min(fr.confidence, 35);
                }
                fr.category = "Unknown";
                fr.source = "fat";
                fr.createdAt = fat::dosTimestampToUnix(entry->crtDate, entry->crtTime);
                fr.modifiedAt = fat::dosTimestampToUnix(entry->wrtDate, entry->wrtTime);
                callback(fr);
            }
        }
    }
    
    // Simplistic directory traversal for FAT32
    // Normally we'd do a recursive traversal using the FAT table
    // For brevity in this stub, we report it.
    if (isFat32) {
        bool dirUnread = false;
        // FAT table lookup lambda
        auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
            if (cluster < 2) return 0x0FFFFFFF;
            uint32_t fatOffset = cluster * 4;
            uint32_t fatSector = fatStartSector + (fatOffset / bps);
            uint32_t entOffset = fatOffset % bps;
            
            std::vector<uint8_t> secBuf(bps);
            if (!readComplete(reader.readSectors(fatSector * bps, bps, secBuf.data()), bps)) {
                dirUnread = true;
                return 0x0FFFFFFF;
            }
            
            uint32_t next = *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
            return next & 0x0FFFFFFF;
        };
        
        uint32_t bytesPerCluster = bpb->sectorsPerCluster * bps;
        std::vector<uint8_t> clusterBuf(bytesPerCluster);

        // LFN fragments can span cluster boundaries within a single directory,
        // so the accumulation state lives outside the per-cluster loop.
        LfnAcc lfn32;

        // A cyclic FAT chain (corrupt/deliberate) must not loop this walk
        // forever: remember the clusters of the chain we are already in, and
        // never enter a directory cluster we processed before.
        std::unordered_set<uint32_t> visitedDirs;
        while (!dirClusters.empty()) {
            uint32_t currentCluster = dirClusters.back();
            dirClusters.pop_back();
            if (!visitedDirs.insert(currentCluster).second) continue;

            std::vector<uint32_t> chainSeen;
            uint32_t clus = currentCluster;
            while (clus >= 2 && clus < 0x0FFFFFF8) {
                if (std::find(chainSeen.begin(), chainSeen.end(), clus) != chainSeen.end()) break;
                chainSeen.push_back(clus);
                uint64_t sec = dataStartSector + (clus - 2) * bpb->sectorsPerCluster;
                auto clusRes = reader.readSectors(sec * bps, bytesPerCluster, clusterBuf.data());
                if (!readComplete(clusRes, bytesPerCluster)) {
                    dirUnread = true;
                    clus = getNextCluster(clus);
                    continue;
                }

                for (uint32_t offset = 0; offset < bytesPerCluster; offset += 32) {
                    FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(clusterBuf.data() + offset);
                    if (entry->name[0] == 0x00) break;

                    bool deleted = (entry->name[0] == 0xE5);

                    if (entry->attr == 0x0F) {
                        lfn32.add(reinterpret_cast<FAT_LFNEntry*>(entry), deleted);
                        continue;
                    }

                    if ((entry->attr & 0x08) != 0 && (entry->attr & 0x10) == 0) {
                        int volStatus = deleted ? 0 : 1;
                        if (deleted) entry->name[0] = '_';
                        emitFatVolumeLabel(callback, entry->name, sec, volStatus);
                        lfn32.clear();
                        continue;
                    }

                    int status = 1;
                    int confidence = 100;
                    std::string lfnName = lfn32.resolve(entry->name, deleted);
                    if (deleted) {
                        status = 0;
                        confidence = 60;
                        entry->name[0] = '_';
                    }

                    std::string name = lfnName.empty() ? formatFATName(entry->name) : lfnName;
                    lfn32.clear();

                    if (name == "." || name == "..") continue;

                    uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
                    if (entry->attr & 0x10) {
                        dirClusters.push_back(firstCluster);
                    } else {
                        bool thisUnread = false;
                        auto runs32 = buildRunsFromChain(reader, fatStartSector, bps, 32,
                                                         firstCluster, bpb->sectorsPerCluster,
                                                         dataStartSector, 65536, &thisUnread);
                        FileRecord fr;
                        fr.id = -1;
                        fr.parentId = 0;
                        fr.name = name;
                        fr.sizeBytes = entry->fileSize;
                        fr.startSector = runs32.empty()
                            ? dataStartSector + (uint64_t)(std::max<uint32_t>(firstCluster, 2) - 2) * bpb->sectorsPerCluster
                            : runs32.front().startSector;
                        fr.endSector = fr.startSector + (uint32_t)((entry->fileSize + bps * bpb->sectorsPerCluster - 1) / (bps * bpb->sectorsPerCluster)) * bpb->sectorsPerCluster;
                        fr.runs = std::move(runs32);
                        fr.path = "/";
                        fr.status = status;
                        fr.confidence = confidence;
                        if (deleted && fatRunBytes(fr.runs, bps) < entry->fileSize &&
                            fatFirstClusterFreed(readFatEntry(reader, fatStartSector, bps, 32, firstCluster), 32)) {
                            if (!applyBackupFatChain(fr, reader, fatStartSector, fatSize, bpb->numFATs, bps,
                                                     32, firstCluster, bpb->sectorsPerCluster,
                                                     dataStartSector, entry->fileSize, &thisUnread)) {
                                fillDeletedFatRuns(fr, reader, firstCluster, entry->fileSize,
                                                   bpb->sectorsPerCluster, dataStartSector, bps);
                            }
                        }
                        applyFatDeletedChainHint(fr, entry->fileSize, bps, deleted);
                        if (thisUnread) {
                            chainUnread = true;
                            fr.confidence = std::min(fr.confidence, 35);
                        }
                        fr.category = "Unknown";
                        fr.source = "fat";
                        fr.createdAt = fat::dosTimestampToUnix(entry->crtDate, entry->crtTime);
                        fr.modifiedAt = fat::dosTimestampToUnix(entry->wrtDate, entry->wrtTime);
                        callback(fr);
                    }
                }

                clus = getNextCluster(clus);
            }
            // A directory boundary invalidates any half-collected LFN chain.
            lfn32.clear();
        }
        if (dirUnread) emitFatDirUnread(callback, dataStartSector);
    }
    if (chainUnread) emitFatChainUnread(callback, fatStartSector);
}

void FATParser::parseExFAT(byteback::DiskReader& reader, uint64_t partitionOffset, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    auto loadExfatBpb = [&](uint64_t absSector) {
        return readComplete(reader.readSectors(absSector * sectorSize, sectorSize, buffer.data()),
                            sectorSize);
    };
    if (!loadExfatBpb(partitionOffset) || std::memcmp(buffer.data() + 3, "EXFAT   ", 8) != 0) {
        if (!loadExfatBpb(partitionOffset + 12) ||
            std::memcmp(buffer.data() + 3, "EXFAT   ", 8) != 0) {
            return;
        }
    }

    ExFAT_BPB* bpb = reinterpret_cast<ExFAT_BPB*>(buffer.data());

    // Shift fields are attacker-controlled bytes; shifts >= 32 are UB and a
    // combined shift past ~25 makes clusterBuf absurdly large.
    if (bpb->bytesPerSectorShift > 12 || bpb->sectorsPerClusterShift > 25 ||
        bpb->bytesPerSectorShift + bpb->sectorsPerClusterShift > 25) {
        return;
    }

    uint32_t bytesPerSector = 1u << bpb->bytesPerSectorShift;
    uint32_t bytesPerCluster = bytesPerSector << bpb->sectorsPerClusterShift;

    uint64_t fatStartSector = partitionOffset + bpb->fatOffset;
    uint64_t dataStartSector = partitionOffset + bpb->clusterHeapOffset;
    bool dirUnread = false;
    bool chainUnread = false;

    auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        uint64_t fatOffset = static_cast<uint64_t>(cluster) * 4;
        uint64_t fatSector = fatStartSector + (fatOffset / bytesPerSector);
        uint32_t entOffset = static_cast<uint32_t>(fatOffset % bytesPerSector);

        std::vector<uint8_t> secBuf(bytesPerSector);
        if (!readComplete(reader.readSectors(fatSector * bytesPerSector, bytesPerSector, secBuf.data()),
                          bytesPerSector)) {
            dirUnread = true;
            return 0xFFFFFFFF;
        }

        return *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
    };
    
    struct DirJob {
        uint32_t cluster = 0;
        std::string path;
    };
    std::vector<DirJob> dirJobs = {{ bpb->rootDirectoryCluster, "/" }};
    std::vector<uint8_t> clusterBuf(bytesPerCluster);

    // exFAT entry-set state: match members by SetChecksum (offset 2) so unrelated
    // deleted/allocated entries between fragments do not break reassembly.
    struct ExfatPending {
        bool active = false;
        bool isDir = false;
        bool inUse = true;
        uint64_t created = 0;
        uint64_t modified = 0;
        uint32_t firstCluster = 0;
        uint64_t dataLength = 0;
        uint8_t nameLength = 0;
        uint16_t setChecksum = 0;
        bool haveStream = false;
        std::vector<uint16_t> name;
    } pending;

    auto dosTimestampToUnix = [](uint16_t date, uint16_t time) -> int64_t {
        return fat::dosTimestampToUnix(date, time);
    };

    auto emitPending = [&](const std::string& currentPath) {
        if (!pending.active || pending.name.empty()) {
            pending = ExfatPending{};
            return;
        }
        std::string name = ntfs::utf16leToUtf8(pending.name.data(), pending.name.size());
        // sanitize: strip separators/control chars (same policy as NTFS names)
        for (char& c : name) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || c == '/' || c == '\\' || c == ':') c = '_';
        }
        if (name.empty()) {
            pending = ExfatPending{};
            return;
        }

        if (pending.isDir) {
            if (pending.firstCluster >= 2) {
                std::string child = currentPath;
                if (child.empty() || child.back() != '/') child += '/';
                child += name;
                if (child.back() != '/') child += '/';
                dirJobs.push_back({pending.firstCluster, std::move(child)});
            }
        } else {
            bool thisUnread = false;
            const uint32_t spc = (1u << bpb->sectorsPerClusterShift);
            auto runsex = buildRunsFromChain(reader, fatStartSector, bytesPerSector, 32,
                                             pending.firstCluster, spc,
                                             dataStartSector, 1 << 20, &thisUnread);
            FileRecord fr;
            fr.id = -1;
            fr.parentId = 0;
            fr.name = name;
            fr.sizeBytes = pending.dataLength;
            fr.startSector = runsex.empty()
                ? dataStartSector + (uint64_t)(std::max<uint32_t>(pending.firstCluster, 2) - 2) * spc
                : runsex.front().startSector;
            fr.endSector = fr.startSector + (uint32_t)((pending.dataLength + bytesPerCluster - 1) / bytesPerCluster) * spc;
            fr.runs = std::move(runsex);
            fr.path = currentPath;
            fr.status = pending.inUse ? 1 : 0;
            fr.confidence = pending.inUse ? 100 : 70;
            if (!pending.inUse && fatRunBytes(fr.runs, bytesPerSector) < pending.dataLength) {
                const uint32_t fatVal = getNextCluster(pending.firstCluster);
                if (fatVal == 0) {
                    if (!applyBackupFatChain(fr, reader, fatStartSector, bpb->fatLength, bpb->numFats,
                                             bytesPerSector, 32, pending.firstCluster, spc,
                                             dataStartSector, pending.dataLength, &thisUnread)) {
                        fillDeletedFatRuns(fr, reader, pending.firstCluster, pending.dataLength, spc,
                                           dataStartSector, bytesPerSector);
                    }
                }
            }
            applyFatDeletedChainHint(fr, pending.dataLength, bytesPerSector, !pending.inUse);
            if (thisUnread) {
                chainUnread = true;
                fr.confidence = std::min(fr.confidence, 35);
            }
            fr.category = "Unknown";
            fr.source = "exfat";
            fr.createdAt = pending.created;
            fr.modifiedAt = pending.modified;
            callback(fr);
        }
        pending = ExfatPending{};
    };

    auto maybeEmitCompleteSet = [&](const std::string& currentPath) {
        if (pending.active && pending.haveStream && pending.nameLength > 0 &&
            pending.name.size() >= pending.nameLength) {
            emitPending(currentPath);
        }
    };

    // Same cyclic-chain guards as the FAT32 walk: a corrupt FAT that loops a
    // directory chain (or two directories pointing at each other) must not
    // spin this loop forever.
    std::unordered_set<uint32_t> visitedDirs;
    while (!dirJobs.empty()) {
        DirJob job = dirJobs.back();
        dirJobs.pop_back();
        if (!visitedDirs.insert(job.cluster).second) continue;
        const std::string currentPath = job.path;

        std::vector<uint32_t> chainSeen;
        uint32_t clus = job.cluster;
        while (clus >= 2 && clus <= 0xFFFFFFF6) {
            if (std::find(chainSeen.begin(), chainSeen.end(), clus) != chainSeen.end()) break;
            chainSeen.push_back(clus);
            uint64_t sec = dataStartSector + (clus - 2) * (1u << bpb->sectorsPerClusterShift);
            auto clusRes = reader.readSectors(sec * bytesPerSector, bytesPerCluster, clusterBuf.data());
            if (!readComplete(clusRes, bytesPerCluster)) {
                dirUnread = true;
                clus = getNextCluster(clus);
                continue;
            }

            for (uint32_t offset = 0; offset + 32 <= bytesPerCluster; offset += 32) {
                const uint8_t* e = clusterBuf.data() + offset;
                uint8_t type = e[0];

                if (type == 0x00) {
                    emitPending(currentPath);
                    offset = bytesPerCluster;
                    break;
                }

                uint8_t typeCode = type & 0x7F;
                bool inUse = (type & 0x80) != 0;

                // Allocation bitmap (0x81/0x01), up-case table (0x82/0x02).
                if (typeCode == 0x03) {
                    uint8_t nch = e[1];
                    if (nch > 11) nch = 11;
                    if (nch > 0) {
                        std::vector<uint16_t> u(nch);
                        for (uint8_t i = 0; i < nch; ++i)
                            u[i] = static_cast<uint16_t>(e[2 + i * 2]) |
                                   (static_cast<uint16_t>(e[3 + i * 2]) << 8);
                        const std::string label = ntfs::utf16leToUtf8(u.data(), nch);
                        if (!label.empty()) {
                            FileRecord fr;
                            fr.id = -1;
                            fr.name = label;
                            fr.path = "/";
                            fr.status = inUse ? 1 : 0;
                            fr.confidence = 5;
                            fr.category = "System";
                            fr.source = "exfat_vol_label";
                            callback(fr);
                        }
                    }
                    continue;
                }
                if (typeCode == 0x02 || (typeCode == 0x01 && !pending.active)) {
                    continue;
                }

                if (typeCode == 0x05) {
                    uint16_t newChk = static_cast<uint16_t>(e[2]) | (static_cast<uint16_t>(e[3]) << 8);
                    if (pending.active && (!pending.haveStream || pending.name.empty()) &&
                        newChk != pending.setChecksum) {
                        continue;
                    }
                    emitPending(currentPath);
                    pending.active = true;
                    pending.inUse = inUse;
                    pending.setChecksum = newChk;
                    pending.haveStream = false;
                    pending.name.clear();
                    uint16_t attr = static_cast<uint16_t>(e[4]) | (static_cast<uint16_t>(e[5]) << 8);
                    pending.isDir = (attr & 0x10) != 0;
                    pending.created = dosTimestampToUnix(
                        static_cast<uint16_t>(e[8]) | (static_cast<uint16_t>(e[9]) << 8),
                        static_cast<uint16_t>(e[10]) | (static_cast<uint16_t>(e[11]) << 8));
                    pending.modified = dosTimestampToUnix(
                        static_cast<uint16_t>(e[12]) | (static_cast<uint16_t>(e[13]) << 8),
                        static_cast<uint16_t>(e[14]) | (static_cast<uint16_t>(e[15]) << 8));
                } else if (typeCode == 0x40 && pending.active) {
                    // File Stream Extension dentry (0xC0 in-use / 0x40 deleted).
                    // Secondary entries carry no SetChecksum; per spec:
                    // FileNameLength is byte 3, FirstCluster 20..23, DataLength
                    // (LE64) 24..31.
                    pending.nameLength = e[3];
                    pending.firstCluster = static_cast<uint32_t>(e[20]) |
                                           (static_cast<uint32_t>(e[21]) << 8) |
                                           (static_cast<uint32_t>(e[22]) << 16) |
                                           (static_cast<uint32_t>(e[23]) << 24);
                    uint64_t len = 0;
                    for (int i = 7; i >= 0; --i) len = (len << 8) | e[24 + i];
                    pending.dataLength = len;
                    pending.haveStream = true;
                    maybeEmitCompleteSet(currentPath);
                } else if (typeCode == 0x41 && pending.active) {
                    // File Name dentry (0xC1/0x41): 15 UTF-16LE chars at byte 2.
                    for (int i = 0; i < 15; ++i) {
                        if (pending.nameLength > 0 && pending.name.size() >= pending.nameLength) break;
                        uint16_t ch = static_cast<uint16_t>(e[2 + i * 2]) |
                                      (static_cast<uint16_t>(e[3 + i * 2]) << 8);
                        if (ch == 0) break;
                        pending.name.push_back(ch);
                    }
                    maybeEmitCompleteSet(currentPath);
                }
            }
            clus = getNextCluster(clus);
        }
        emitPending(currentPath);
    }
    if (dirUnread) emitFatDirUnread(callback, dataStartSector);
    if (chainUnread) emitFatChainUnread(callback, fatStartSector);
}

} // namespace byteback
