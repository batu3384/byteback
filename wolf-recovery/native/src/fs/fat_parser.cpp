#include "wolf_fs.h"
#include "fs/ntfs_util.h"
#include "fs/fat_chain.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <array>
#include <map>

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

namespace wolf {


namespace {
// CA-002 fix: convert a FAT/exFAT cluster chain into physical sector runs.
// The chain math (EOC/bad thresholds, cycle guard, sector mapping) lives in
// fs/fat_chain.cpp and is unit-tested; this adapter only reads FAT entries
// off the disk and translates run shapes.
std::vector<FileRecord::DataRun> buildRunsFromChain(
    DiskReader& reader, uint64_t fatStartSector, uint32_t bytesPerSector,
    int fatBits, uint32_t firstCluster, uint32_t sectorsPerCluster,
    uint64_t dataStartSector, size_t maxClusters) {

    fat::FatEntryReader readEntry = [&](uint32_t cluster) -> uint32_t {
        uint64_t byteOffset = (fatBits == 12) ? (uint64_t)cluster * 3 / 2
                          : (fatBits == 16) ? (uint64_t)cluster * 2
                          : (uint64_t)cluster * 4;
        uint64_t sector = fatStartSector + byteOffset / bytesPerSector;
        uint32_t off = (uint32_t)(byteOffset % bytesPerSector);
        if (off + 4 > bytesPerSector && fatBits == 32) return 0xFFFFFFF8; // entry straddles: treat as EOC
        std::vector<uint8_t> sec(bytesPerSector);
        if (!reader.readSectors(sector * bytesPerSector, bytesPerSector, sec.data()).success) return 0xFFFFFFF8;
        if (fatBits == 32) return (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8) | ((uint32_t)sec[off+2] << 16) | ((uint32_t)sec[off+3] << 24);
        if (fatBits == 16) return (uint32_t)((uint16_t)sec[off] | ((uint16_t)sec[off+1] << 8));
        // FAT12
        uint32_t v = (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8);
        return (cluster & 1) ? (v >> 4) : (v & 0xFFF);
    };

    auto chain = fat::chainRuns(std::move(readEntry), fatBits, firstCluster,
                                sectorsPerCluster, dataStartSector, maxClusters);
    std::vector<FileRecord::DataRun> runs;
    runs.reserve(chain.size());
    for (const auto& r : chain) {
        runs.push_back({r.startSector, r.sectorCount});
    }
    return runs;
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
    std::string out;
    out.reserve(utf16.size());
    for (uint16_t c : utf16) {
        if (c == 0 || c == 0xFFFF) break; // terminator / padding
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x100) out += static_cast<char>(c); // Latin-1
        else out += '?';
    }
    return out;
}

bool FATParser::scan(wolf::DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0);
}

bool FATParser::scanAt(wolf::DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning,
                       uint64_t partitionOffsetBytes) {
    if (!reader.isOpen()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    // The internal parseFAT/parseExFAT routines work in partition-relative
    // SECTOR units; convert the caller-supplied byte offset once here.
    uint64_t partitionOffset = partitionOffsetBytes / sectorSize;

    std::vector<uint8_t> buffer(sectorSize);

    auto res = reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    if (!res.success) return false;

    if (memcmp(&buffer[3], "EXFAT   ", 8) == 0) {
        parseExFAT(reader, partitionOffset, callback, isRunning);
    } else {
        parseFAT(reader, partitionOffset, callback, isRunning);
    }
    return true;
}

void FATParser::parseFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    
    FAT_BPB* bpb = reinterpret_cast<FAT_BPB*>(buffer.data());
    
    uint16_t bps = bpb->bytesPerSector;
    if (bps == 0 || (bps & (bps - 1)) != 0) return; // Invalid BPS
    
    uint32_t rootDirSectors = ((bpb->rootEntryCount * 32) + (bps - 1)) / bps;
    uint32_t fatSize = (bpb->fatSize16 != 0) ? bpb->fatSize16 : bpb->fatSize32;
    
    uint64_t fatStartSector = partitionOffset + bpb->reservedSectorCount;
    uint64_t rootDirStartSector = fatStartSector + (bpb->numFATs * fatSize);
    uint64_t dataStartSector = rootDirStartSector + rootDirSectors;
    
    uint32_t totalSectors = (bpb->totalSectors16 != 0) ? bpb->totalSectors16 : bpb->totalSectors32;
    uint32_t dataSectors = totalSectors - (bpb->reservedSectorCount + (bpb->numFATs * fatSize) + rootDirSectors);
    uint32_t countOfClusters = dataSectors / bpb->sectorsPerCluster;
    
    bool isFat32 = countOfClusters >= 65525;
    
    std::vector<uint32_t> dirClusters;
    if (isFat32) {
        dirClusters.push_back(bpb->rootCluster);
    } else {
        // Parse FAT16 root dir sequentially. Long File Name (LFN) entries
        // precede their 8.3 short entry in reverse ordinal order; we buffer
        // them and validate via checksum when the short entry arrives.
        std::vector<uint8_t> rootBuf(rootDirSectors * bps);
        reader.readSectors(rootDirStartSector * bps, rootDirSectors * bps, rootBuf.data());

        // LFN accumulation state. Indexed by ordinal so fragments reassemble in
        // forward order regardless of how many 13-char segments were needed.
        std::map<uint8_t, std::array<uint16_t, 13>> lfnFragments;
        uint8_t lfnChecksumSeen = 0;

        for (uint32_t offset = 0; offset < rootBuf.size(); offset += 32) {
            FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(rootBuf.data() + offset);
            if (entry->name[0] == 0x00) break;

            bool deleted = (entry->name[0] == 0xE5);

            if (entry->attr == 0x0F) {
                // VFAT long-name entry. Deleted LFN entries (0xE5 first byte)
                // still belong to a possibly-deleted file, but the ord field
                // has been clobbered to 0xE5 — skip those; we cannot trust the
                // ordinal and would corrupt a following valid chain.
                if (deleted) continue;
                FAT_LFNEntry* lfn = reinterpret_cast<FAT_LFNEntry*>(entry);
                uint8_t ord = lfn->ord & 0x3F; // mask the "last" bit (0x40)
                std::array<uint16_t, 13> chars{};
                for (int i = 0; i < 5; ++i) chars[i] = lfn->name1[i];
                for (int i = 0; i < 6; ++i) chars[5 + i] = lfn->name2[i];
                for (int i = 0; i < 2; ++i) chars[11 + i] = lfn->name3[i];
                lfnFragments[ord] = chars;
                lfnChecksumSeen = lfn->chksum;
                continue;
            }

            int status = 1;
            int confidence = 100;
            if (deleted) {
                status = 0;
                confidence = 60;
                entry->name[0] = '_';
            }

            // Resolve the display name: prefer the LFN if the checksum matches.
            std::string name = formatFATName(entry->name);
            if (!lfnFragments.empty() && lfnChecksum(entry->name) == lfnChecksumSeen) {
                std::vector<uint16_t> full;
                for (auto it = lfnFragments.begin(); it != lfnFragments.end(); ++it) {
                    for (uint16_t c : it->second) full.push_back(c);
                }
                std::string lfnName = lfnToString(full);
                if (!lfnName.empty()) name = lfnName;
            }
            lfnFragments.clear();

            uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
            if (entry->attr & 0x10) {
                if (name != "." && name != "..") dirClusters.push_back(firstCluster);
            } else {
                // CA-002: emit real sector runs from the FAT chain instead of
                // leaking the raw cluster number into startSector.
                int fatBits = (countOfClusters < 4085) ? 12 : 16;
                auto runs16 = buildRunsFromChain(reader, fatStartSector, bps, fatBits,
                                                 firstCluster, bpb->sectorsPerCluster,
                                                 dataStartSector, 65536);
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
                fr.category = "Unknown";
                fr.source = "fat";
                callback(fr);
            }
        }
    }
    
    // Simplistic directory traversal for FAT32
    // Normally we'd do a recursive traversal using the FAT table
    // For brevity in this stub, we report it.
    if (isFat32) {
        // FAT table lookup lambda
        auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
            if (cluster < 2) return 0x0FFFFFFF;
            uint32_t fatOffset = cluster * 4;
            uint32_t fatSector = fatStartSector + (fatOffset / bps);
            uint32_t entOffset = fatOffset % bps;
            
            std::vector<uint8_t> secBuf(bps);
            if (!reader.readSectors(fatSector * bps, bps, secBuf.data()).success) return 0x0FFFFFFF;
            
            uint32_t next = *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
            return next & 0x0FFFFFFF;
        };
        
        uint32_t bytesPerCluster = bpb->sectorsPerCluster * bps;
        std::vector<uint8_t> clusterBuf(bytesPerCluster);

        // LFN fragments can span cluster boundaries within a single directory,
        // so the accumulation state lives outside the per-cluster loop.
        std::map<uint8_t, std::array<uint16_t, 13>> lfnFragments;
        uint8_t lfnChecksumSeen = 0;

        while (!dirClusters.empty()) {
            uint32_t currentCluster = dirClusters.back();
            dirClusters.pop_back();

            uint32_t clus = currentCluster;
            while (clus >= 2 && clus < 0x0FFFFFF8) {
                uint64_t sec = dataStartSector + (clus - 2) * bpb->sectorsPerCluster;
                if (!reader.readSectors(sec * bps, bytesPerCluster, clusterBuf.data()).success) break;

                for (uint32_t offset = 0; offset < bytesPerCluster; offset += 32) {
                    FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(clusterBuf.data() + offset);
                    if (entry->name[0] == 0x00) break;

                    bool deleted = (entry->name[0] == 0xE5);

                    if (entry->attr == 0x0F) {
                        if (deleted) continue; // ordinal clobbered, unsafe to use
                        FAT_LFNEntry* lfn = reinterpret_cast<FAT_LFNEntry*>(entry);
                        uint8_t ord = lfn->ord & 0x3F;
                        std::array<uint16_t, 13> chars{};
                        for (int i = 0; i < 5; ++i) chars[i] = lfn->name1[i];
                        for (int i = 0; i < 6; ++i) chars[5 + i] = lfn->name2[i];
                        for (int i = 0; i < 2; ++i) chars[11 + i] = lfn->name3[i];
                        lfnFragments[ord] = chars;
                        lfnChecksumSeen = lfn->chksum;
                        continue;
                    }

                    int status = 1;
                    int confidence = 100;
                    if (deleted) {
                        status = 0;
                        confidence = 60;
                    }

                    std::string name = formatFATName(entry->name);
                    if (!lfnFragments.empty() && lfnChecksum(entry->name) == lfnChecksumSeen) {
                        std::vector<uint16_t> full;
                        for (auto it = lfnFragments.begin(); it != lfnFragments.end(); ++it) {
                            for (uint16_t c : it->second) full.push_back(c);
                        }
                        std::string lfnName = lfnToString(full);
                        if (!lfnName.empty()) name = lfnName;
                    }
                    lfnFragments.clear();

                    if (name == "." || name == "..") continue;

                    uint32_t firstCluster = entry->fstClusLO | (entry->fstClusHI << 16);
                    if (entry->attr & 0x10) {
                        dirClusters.push_back(firstCluster);
                    } else {
                        auto runs32 = buildRunsFromChain(reader, fatStartSector, bps, 32,
                                                         firstCluster, bpb->sectorsPerCluster,
                                                         dataStartSector, 65536);
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
                        fr.category = "Unknown";
                        fr.source = "fat";
                        callback(fr);
                    }
                }

                clus = getNextCluster(clus);
            }
            // A directory boundary invalidates any half-collected LFN chain.
            lfnFragments.clear();
        }
    }
}

void FATParser::parseExFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    reader.readSectors(partitionOffset * sectorSize, sectorSize, buffer.data());
    
    ExFAT_BPB* bpb = reinterpret_cast<ExFAT_BPB*>(buffer.data());
    
    uint32_t bytesPerSector = 1 << bpb->bytesPerSectorShift;
    uint32_t bytesPerCluster = bytesPerSector << bpb->sectorsPerClusterShift;
    
    uint64_t fatStartSector = partitionOffset + bpb->fatOffset;
    uint64_t dataStartSector = partitionOffset + bpb->clusterHeapOffset;
    
    auto getNextCluster = [&](uint32_t cluster) -> uint32_t {
        if (cluster < 2) return 0xFFFFFFFF;
        uint32_t fatOffset = cluster * 4;
        uint32_t fatSector = fatStartSector + (fatOffset / bytesPerSector);
        uint32_t entOffset = fatOffset % bytesPerSector;
        
        std::vector<uint8_t> secBuf(bytesPerSector);
        if (!reader.readSectors(fatSector * bytesPerSector, bytesPerSector, secBuf.data()).success) return 0xFFFFFFFF;
        
        return *reinterpret_cast<uint32_t*>(secBuf.data() + entOffset);
    };
    
    std::vector<uint32_t> dirClusters = { bpb->rootDirectoryCluster };
    std::vector<uint8_t> clusterBuf(bytesPerCluster);

    // ---- exFAT entry-set state machine ----
    // An exFAT directory is a flat sequence of 32-byte entries. A file is a
    // "set": one File entry (type code 0x05), followed by a Stream Extension
    // entry (0x45) carrying the cluster/length, followed by one or more File
    // Name entries (0x41) each holding up to 15 UTF-16LE characters. The old
    // parser compared masked type codes against the wrong constants (0x0C and
    // 0x01), so stream/name entries never matched and no file was ever
    // reported. Bit 0x80 = "in use"; clear means deleted.
    //
    // A pending set accumulates until the next File entry (or scan end), so
    // sets that span a cluster boundary survive the FAT chain walk.
    struct ExfatPending {
        bool active = false;
        bool isDir = false;
        bool inUse = true;
        uint64_t created = 0;
        uint64_t modified = 0;
        uint32_t firstCluster = 0;
        uint64_t dataLength = 0;
        uint8_t nameLength = 0;       // in UTF-16 code units
        std::vector<uint16_t> name;   // accumulated name units
    } pending;

    // DOS date/time conversion lives in fs/fat_chain.cpp (unit-tested).
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
            if (pending.firstCluster >= 2) dirClusters.push_back(pending.firstCluster);
        } else {
            auto runsex = buildRunsFromChain(reader, fatStartSector, bytesPerSector, 32,
                                             pending.firstCluster,
                                             (1u << bpb->sectorsPerClusterShift),
                                             dataStartSector, 1 << 20);
            FileRecord fr;
            fr.id = -1;
            fr.parentId = 0;
            fr.name = name;
            fr.sizeBytes = pending.dataLength;
            fr.startSector = runsex.empty()
                ? dataStartSector + (uint64_t)(std::max<uint32_t>(pending.firstCluster, 2) - 2) * (1u << bpb->sectorsPerClusterShift)
                : runsex.front().startSector;
            fr.endSector = fr.startSector + (uint32_t)((pending.dataLength + bytesPerCluster - 1) / bytesPerCluster) * (1u << bpb->sectorsPerClusterShift);
            fr.runs = std::move(runsex);
            fr.path = currentPath;
            fr.status = pending.inUse ? 1 : 0;
            // Deleted sets: the FAT chain is cleared on delete, so recovery
            // assumes the data was contiguous from the first cluster — score
            // it accordingly.
            fr.confidence = pending.inUse ? 100 : 70;
            fr.category = "Unknown";
            fr.source = "exfat";
            fr.createdAt = pending.created;
            fr.modifiedAt = pending.modified;
            callback(fr);
        }
        pending = ExfatPending{};
    };

    // Track the current directory's display path for nested results (best
    // effort — exFAT does not store parent ids, so paths are reconstructed
    // per recursion level by the caller-side list order).
    std::string currentPath = "/";

    while (!dirClusters.empty()) {
        uint32_t currentCluster = dirClusters.back();
        dirClusters.pop_back();

        uint32_t clus = currentCluster;
        while (clus >= 2 && clus <= 0xFFFFFFF6) {
            uint64_t sec = dataStartSector + (clus - 2) * (1u << bpb->sectorsPerClusterShift);
            if (!reader.readSectors(sec * bytesPerSector, bytesPerCluster, clusterBuf.data()).success) break;

            for (uint32_t offset = 0; offset + 32 <= bytesPerCluster; offset += 32) {
                const uint8_t* e = clusterBuf.data() + offset;
                uint8_t type = e[0];

                if (type == 0x00) {
                    // End-of-directory marker — everything after is slack.
                    offset = bytesPerCluster; // stop scanning this cluster
                    break;
                }

                uint8_t typeCode = type & 0x7F;
                bool inUse = (type & 0x80) != 0;

                // Volume label (0x83/0x03), allocation bitmap (0x81/0x01),
                // up-case table (0x82/0x02) — structural entries, skip.
                if (typeCode == 0x03 || typeCode == 0x02 || (typeCode == 0x01 && !pending.active)) {
                    continue;
                }

                if (typeCode == 0x05) {
                    // File directory entry: starts a new set. Emit whatever
                    // was pending first.
                    emitPending(currentPath);
                    pending.active = true;
                    pending.inUse = inUse;
                    // +0x04 FileAttributes (2 bytes LE); 0x10 = directory.
                    uint16_t attr = static_cast<uint16_t>(e[4]) | (static_cast<uint16_t>(e[5]) << 8);
                    pending.isDir = (attr & 0x10) != 0;
                    // +0x08 Created / +0x0C LastModified — 4 bytes each:
                    // 16-bit DOS date then 16-bit DOS time (LE).
                    pending.created = dosTimestampToUnix(
                        static_cast<uint16_t>(e[8]) | (static_cast<uint16_t>(e[9]) << 8),
                        static_cast<uint16_t>(e[10]) | (static_cast<uint16_t>(e[11]) << 8));
                    pending.modified = dosTimestampToUnix(
                        static_cast<uint16_t>(e[12]) | (static_cast<uint16_t>(e[13]) << 8),
                        static_cast<uint16_t>(e[14]) | (static_cast<uint16_t>(e[15]) << 8));
                } else if (typeCode == 0x45 && pending.active) {
                    // Stream extension: +0x03 FileNameLength, +0x14 FirstCluster
                    // (u32), +0x18 DataLength (u64).
                    pending.nameLength = e[3];
                    pending.firstCluster = static_cast<uint32_t>(e[20]) |
                                           (static_cast<uint32_t>(e[21]) << 8) |
                                           (static_cast<uint32_t>(e[22]) << 16) |
                                           (static_cast<uint32_t>(e[23]) << 24);
                    uint64_t len = 0;
                    for (int i = 7; i >= 0; --i) len = (len << 8) | e[24 + i];
                    pending.dataLength = len;
                } else if (typeCode == 0x41 && pending.active) {
                    // File name: +0x02..0x1F = 15 UTF-16LE code units.
                    for (int i = 0; i < 15; ++i) {
                        if (pending.nameLength > 0 &&
                            pending.name.size() >= pending.nameLength) break;
                        uint16_t ch = static_cast<uint16_t>(e[2 + i * 2]) |
                                      (static_cast<uint16_t>(e[3 + i * 2]) << 8);
                        if (ch == 0) break;
                        pending.name.push_back(ch);
                    }
                }
            }
            clus = getNextCluster(clus);
        }
    }
    // Flush a set that ended exactly at the end of the directory.
    emitPending(currentPath);
}

} // namespace wolf
