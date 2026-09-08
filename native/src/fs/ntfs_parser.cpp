#include "byteback_fs.h"
#include "byteback_memory.h"
#include "db/runs_codec.h"
#include "fs/ntfs_util.h"
#include "fs/ntfs_logfile.h"
#include "fs/ntfs_path_rebuild.h"
#include "fs/ntfs_recycle.h"
#include "fs/ntfs_thumbcache.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace byteback {

NTFSParser::NTFSParser() {}
NTFSParser::~NTFSParser() {}

#pragma pack(push, 1)
struct MFT_RecordHeader {
    char signature[4]; // "FILE"
    uint16_t updateSequenceOffset;
    uint16_t updateSequenceSize;
    uint64_t logFileSequenceNumber;
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags; // 0x01 = In Use, 0x02 = Directory
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

struct NTFS_ResidentAttributeHeader {
    uint32_t valueLength;
    uint16_t valueOffset;
    uint8_t indexedFlag;
    uint8_t padding;
};

struct NTFS_FileNameAttribute {
    uint64_t parentDirectory;
    uint64_t creationTime;
    uint64_t changeTime;
    uint64_t mftChangeTime;
    uint64_t accessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t flags;
    uint32_t er;
    uint8_t nameLength;
    uint8_t nameType;
    uint16_t name[1]; // UTF-16 LE
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

namespace {
// Sanitize a decoded UTF-8 filename for safe storage/display. Path separators,
// control chars and reserved NTFS characters are replaced with '_' while
// preserving legitimate non-ASCII bytes (Turkish, CJK, etc.). Runs over UTF-8
// so it never splits a multibyte sequence: it only touches bytes < 0x80 that
// are explicitly forbidden, leaving all >= 0x80 (continuation / lead) intact.
std::string sanitizeUtf8Name(std::string name) {
    for (char& c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) { c = '_'; continue; }                  // control chars
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<': case '>': case '|':
                c = '_';
                break;
            default:
                break;
        }
    }
    return name;
}

// Decode an NTFS $FILE_NAME attribute's UTF-16LE name into a sanitized UTF-8
// std::string, returning empty if the bounds look unsafe. Lives below the
// packed struct definitions so NTFS_FileNameAttribute is complete.
std::string decodeNtfsName(const NTFS_FileNameAttribute* fnAttr, size_t availableBytes) {
    size_t headerOff = offsetof(NTFS_FileNameAttribute, name);
    if (availableBytes < headerOff) return {};
    size_t nameBytes = static_cast<size_t>(fnAttr->nameLength) * sizeof(uint16_t);
    if (nameBytes == 0 || nameBytes > availableBytes - headerOff) return {};
    std::string raw = ntfs::utf16leToUtf8(fnAttr->name, fnAttr->nameLength);
    return sanitizeUtf8Name(raw);
}

// CA-010 fix: derive the category from the extension instead of labelling
// every NTFS record "Document". Keeps the ResultsView category filters and
// report statistics honest for MFT-sourced files.
std::string categoryForName(const std::string& name) {
    size_t dot = name.find_last_of('.');
    std::string ext = (dot != std::string::npos && dot + 1 < name.size())
                      ? name.substr(dot + 1) : "";
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    auto in = [&](const char* list) {
        std::string l(list);
        size_t pos = 0;
        while (pos < l.size()) {
            size_t comma = l.find(',', pos);
            std::string item = l.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (item == ext) return true;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return false;
    };
    if (in("jpg,jpeg,png,gif,bmp,tiff,webp,heic,heif,avif,svg,raf,jp2,cr3")) return "Image";
    if (in("mp4,avi,mkv,mov,flv,wmv,mpg,mpeg,3gp,webm,m4v")) return "Video";
    if (in("mp3,wav,flac,ogg,aac,wma,m4a,opus")) return "Audio";
    if (in("doc,docx,pdf,txt,xls,xlsx,ppt,pptx,rtf,odt")) return "Document";
    if (in("zip,rar,7z,gz,tar,iso,cab")) return "Archive";
    if (in("exe,dll,sys,elf")) return "Executable";
    return "Other";
}
} // namespace

bool NTFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    return scanAt(reader, callback, isRunning, 0, 0, true);
}

std::vector<FileRecord::DataRun> unnamedDataRunsFromRecord(
    const uint8_t* rec, uint32_t recordSize,
    uint64_t volumeStartSector, uint32_t sectorsPerCluster) {
    std::vector<FileRecord::DataRun> out;
    if (!rec || recordSize < 64) return out;
    const auto* header = reinterpret_cast<const MFT_RecordHeader*>(rec);
    uint32_t attrOffset = header->firstAttributeOffset;
    while (attrOffset + sizeof(NTFS_AttributeHeader) <= recordSize) {
        const auto* attr = reinterpret_cast<const NTFS_AttributeHeader*>(rec + attrOffset);
        if (attr->type == ntfs::ATTR_END_MARKER || attr->length == 0) break;
        if (attr->type == ntfs::ATTR_DATA && attr->nonResidentFlag != 0 && attr->nameLength == 0) {
            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_NonResidentHeader) > recordSize)
                break;
            // Length must fit inside the record, or attrOffset += length can wrap
            // uint32 and cycle the loop forever on a crafted record.
            if (static_cast<uint64_t>(attrOffset) + attr->length > recordSize) break;
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
        if (static_cast<uint64_t>(attrOffset) + attr->length > recordSize) break; // no uint32 wrap
        attrOffset += attr->length;
    }
    return out;
}

// ---------------------------------------------------------------------------
// FAZ 1.1: shared MFT record parser. Both passes call this function:
//   - Pass-1 uses the path-index fields (name/parent/flags), the $INDEX_ROOT
//     hints and the $UsnJrnl:$J ADS entries, and stores no file data.
//   - Pass-2 re-parses the same record bytes (re-read from disk) and uses the
//     full result to assemble the emitted FileRecord — the resident $DATA is
//     extracted here from the single record that is in memory (C4).
// Field semantics are byte-identical to the pre-refactor in-line parse.
// ---------------------------------------------------------------------------
namespace {

struct ParsedMftAds {
    std::string streamName;
    uint64_t size = 0;
    std::vector<FileRecord::DataRun> runs;
    uint64_t startSector = 0;
    std::vector<uint8_t> residentData;
};

struct ParsedMftRecord {
    // Raw record header facts.
    uint8_t flags = 0;            // MFT_RecordHeader::flags (0x01 in-use, 0x02 dir)
    bool usaOk = false;
    // Assembled view (same values the pre-refactor pass built per record).
    std::string filename;
    uint64_t fileSize = 0;
    uint64_t parentMft = 0;
    std::vector<FileRecord::DataRun> dataRuns;
    uint64_t finalStartSector = 0;
    uint64_t finalEndSector = 0;
    int64_t createdAt = 0;
    int64_t modifiedAt = 0;
    bool dataCompressed = false;
    bool mainStreamResident = false;
    std::vector<uint8_t> residentBytes;
    std::vector<ParsedMftAds> adsEntries;
    std::vector<ntfs::IndexNameHint> i30Hints;
};

void parseMftRecord(uint8_t* rec, uint32_t recordSize, uint32_t sectorSize,
                    uint64_t volumeStartSector, uint32_t sectorsPerCluster,
                    ParsedMftRecord& out) {
    auto* header = reinterpret_cast<MFT_RecordHeader*>(rec);
    out.flags = static_cast<uint8_t>(header->flags);

    // Apply the NTFS Update Sequence Array (USA) fixup before any attribute
    // parsing: NTFS overwrites the last two bytes of each sector in a
    // multi-sector record with a sequence number and stores the originals in
    // the USA. A failed fixup (partial overwrite / stale slack) does not skip
    // the record — we parse on a best-effort basis and lower confidence
    // instead.
    out.usaOk = ntfs::applyUsaFixup(rec, recordSize, sectorSize,
                                    header->updateSequenceOffset,
                                    header->updateSequenceSize);

    // Alternate Data Streams (ADS): a record may carry several $DATA
    // attributes; only the unnamed one is the file's main content. Named
    // $DATA streams (e.g. Zone.Identifier) are collected here and reported as
    // separate recoverable records using the "filename:streamname" convention.
    bool mainStreamResident = false;
    std::vector<uint8_t> residentBytes;
    bool dataCompressed = false;
    bool nameFound = false;
    uint64_t fileSize = header->usedSize;
    uint64_t fileParentMftId = 0;
    int64_t createdAt = 0;
    int64_t modifiedAt = 0;
    std::vector<FileRecord::DataRun> dataRuns;
    uint64_t finalStartSector = 0;
    uint64_t finalEndSector = 0;

    uint32_t attrOffset = header->firstAttributeOffset;
    while (attrOffset + sizeof(NTFS_AttributeHeader) <= recordSize) {
        NTFS_AttributeHeader* attr = reinterpret_cast<NTFS_AttributeHeader*>(rec + attrOffset);
        if (attr->type == ntfs::ATTR_END_MARKER || attr->length == 0) break; // End of attributes or corrupt

        // $FILE_NAME attribute (0x30). A record may carry multiple
        // (POSIX / Win32 long / DOS 8.3). Prefer the long name; fall
        // back to DOS only if nothing else was captured.
        if (attr->type == ntfs::ATTR_FILE_NAME && attr->nonResidentFlag == 0) {
            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(rec + attrOffset + sizeof(NTFS_AttributeHeader));

                size_t nameStructOffset = attrOffset + resAttr->valueOffset;
                if (nameStructOffset + offsetof(NTFS_FileNameAttribute, name) <= recordSize) {
                    NTFS_FileNameAttribute* fnAttr = reinterpret_cast<NTFS_FileNameAttribute*>(rec + nameStructOffset);

                    size_t available = recordSize - nameStructOffset;
                    if (fnAttr->nameLength > 0 && fnAttr->nameLength < 255) {
                        std::string decoded = decodeNtfsName(fnAttr, available);
                        if (!decoded.empty()) {
                            // Prefer a long (Win32) name over DOS 8.3
                            // unless we have no name yet.
                            uint8_t nt = fnAttr->nameType;
                            bool prefer = (nt == ntfs::NAME_TYPE_POSIX || nt == ntfs::NAME_TYPE_LONG || !nameFound);
                            if (prefer) {
                                out.filename = decoded;
                                fileSize = fnAttr->realSize;
                                fileParentMftId = fnAttr->parentDirectory & 0x0000FFFFFFFFFFFFULL;
                                // $FILE_NAME carries its own MACB set;
                                // use modification time for the store.
                                modifiedAt = ntfs::filetimeToUnix(fnAttr->changeTime);
                                if (createdAt == 0) createdAt = ntfs::filetimeToUnix(fnAttr->creationTime);
                                nameFound = true;
                            }
                        }
                    }
                }
            }
        }

        // $STANDARD_INFORMATION (0x10) — authoritative timestamps.
        // Overrides the $FILE_NAME times when present (the SI attr
        // is what Explorer / forensic tools report).
        if (attr->type == ntfs::ATTR_STANDARD_INFORMATION && attr->nonResidentFlag == 0) {
            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(rec + attrOffset + sizeof(NTFS_AttributeHeader));
                size_t siOff = attrOffset + resAttr->valueOffset;
                // SI is at least 48 bytes (creation, modified, mft-change, access).
                if (siOff + 4 * sizeof(uint64_t) <= recordSize) {
                    const uint64_t* times = reinterpret_cast<const uint64_t*>(rec + siOff);
                    createdAt  = ntfs::filetimeToUnix(times[0]);
                    modifiedAt = ntfs::filetimeToUnix(times[1]);
                }
            }
        }

        if (attr->type == ntfs::ATTR_DATA) {
            // Determine whether this is the unnamed main $DATA or
            // a named Alternate Data Stream. attr->nameOffset is
            // relative to the attribute start; the name is UTF-16LE.
            bool isAds = (attr->nameLength > 0);
            std::string adsName;
            if (isAds) {
                size_t namePos = attrOffset + attr->nameOffset;
                if (namePos + static_cast<size_t>(attr->nameLength) * sizeof(uint16_t) <= recordSize) {
                    const uint16_t* nameUnits = reinterpret_cast<const uint16_t*>(rec + namePos);
                    adsName = sanitizeUtf8Name(ntfs::utf16leToUtf8(nameUnits, attr->nameLength));
                }
            }

            if (attr->nonResidentFlag == 0) {
                if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                    NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(rec + attrOffset + sizeof(NTFS_AttributeHeader));
                    std::vector<uint8_t> inlineBytes;
                    const size_t valOff = static_cast<size_t>(attrOffset) + resAttr->valueOffset;
                    const size_t valLen = resAttr->valueLength;
                    if (valLen > 0 && valOff + valLen <= recordSize) {
                        inlineBytes.assign(rec + valOff, rec + valOff + valLen);
                    }
                    if (isAds) {
                        ParsedMftAds ads;
                        ads.streamName = std::move(adsName);
                        ads.size = resAttr->valueLength;
                        ads.residentData = std::move(inlineBytes);
                        out.adsEntries.push_back(std::move(ads));
                    } else {
                        fileSize = resAttr->valueLength;
                        dataRuns.clear();
                        mainStreamResident = resAttr->valueLength > 0;
                        residentBytes = std::move(inlineBytes);
                    }
                }
            } else {
                if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_NonResidentHeader) <= recordSize) {
                    NTFS_NonResidentHeader* nonResAttr = reinterpret_cast<NTFS_NonResidentHeader*>(rec + attrOffset + sizeof(NTFS_AttributeHeader));

                    // Parse the data runs into a local list so an
                    // ADS does not clobber the main stream's runs.
                    uint16_t runOffset = nonResAttr->dataRunOffset;
                    size_t currentRunPos = attrOffset + runOffset;
                    int64_t previousLcn = 0;
                    std::vector<FileRecord::DataRun> localRuns;
                    uint64_t localStart = UINT64_MAX;

                    while (currentRunPos < attrOffset + attr->length && currentRunPos < recordSize) {
                        uint8_t headerByte = (uint8_t)rec[currentRunPos];
                        if (headerByte == 0x00) break;

                        uint8_t lenSize = headerByte & 0x0F;
                        uint8_t offSize = (headerByte >> 4) & 0x0F;
                        currentRunPos++;

                        if (currentRunPos + lenSize + offSize > recordSize) break;

                        uint64_t clusterCount = 0;
                        for (int j = 0; j < lenSize; j++) {
                            clusterCount |= (uint64_t)((uint8_t)rec[currentRunPos + j]) << (j * 8);
                        }
                        currentRunPos += lenSize;

                        bool sparse = (offSize == 0);
                        int64_t lcnOffset = 0;
                        if (!sparse) {
                            for (int j = 0; j < offSize; j++) {
                                lcnOffset |= (uint64_t)((uint8_t)rec[currentRunPos + j]) << (j * 8);
                            }
                            if ((uint8_t)rec[currentRunPos + offSize - 1] & 0x80) {
                                for (int j = offSize; j < 8; j++) {
                                    lcnOffset |= (uint64_t)0xFF << (j * 8);
                                }
                            }
                            previousLcn += lcnOffset;
                        }
                        currentRunPos += offSize;

                        FileRecord::DataRun run;
                        run.startSector = sparse ? UINT64_MAX
                            : volumeStartSector + previousLcn * sectorsPerCluster;
                        run.sectorCount = clusterCount * sectorsPerCluster;
                        localRuns.push_back(run);
                        if (!sparse && localStart == UINT64_MAX) {
                            localStart = run.startSector;
                        }
                    }

                    if (nonResAttr->compressionUnit != 0) dataCompressed = true;
                    if (isAds) {
                        ParsedMftAds ads;
                        ads.streamName = std::move(adsName);
                        ads.size = nonResAttr->realSize;
                        ads.runs = std::move(localRuns);
                        ads.startSector = (localStart != UINT64_MAX) ? localStart : 0;
                        out.adsEntries.push_back(std::move(ads));
                    } else {
                        fileSize = nonResAttr->realSize;
                        dataRuns = std::move(localRuns);
                        finalStartSector = (localStart != UINT64_MAX) ? localStart : 0;
                        finalEndSector = dataRuns.empty() ? 0
                            : (dataRuns.back().startSector == UINT64_MAX ? 0 : dataRuns.back().startSector + dataRuns.back().sectorCount);
                    }
                }
            }
        }

        if (attr->type == ntfs::ATTR_INDEX_ROOT && attr->nonResidentFlag == 0) {
            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                auto* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(
                    rec + attrOffset + sizeof(NTFS_AttributeHeader));
                const size_t valOff = static_cast<size_t>(attrOffset) + resAttr->valueOffset;
                const size_t valLen = resAttr->valueLength;
                if (valLen > 0 && valOff + valLen <= recordSize) {
                    auto hints = ntfs::parseIndexRoot(rec + valOff, valLen);
                    out.i30Hints.insert(out.i30Hints.end(), hints.begin(), hints.end());
                }
            }
        }

        // Corrupt attribute: too short to hold a header, or long
        // enough to wrap uint32 attrOffset back into the record
        // (attrOffset += length cycles forever otherwise).
        if (attr->length < sizeof(NTFS_AttributeHeader)) break;
        if (static_cast<uint64_t>(attrOffset) + attr->length > recordSize) break;
        attrOffset += attr->length;
    }

    out.fileSize = fileSize;
    out.parentMft = fileParentMftId;
    out.dataRuns = std::move(dataRuns);
    out.finalStartSector = finalStartSector;
    out.finalEndSector = finalEndSector;
    out.createdAt = createdAt;
    out.modifiedAt = modifiedAt;
    out.dataCompressed = dataCompressed;
    out.mainStreamResident = mainStreamResident;
    out.residentBytes = std::move(residentBytes);
}

} // namespace

bool NTFSParser::scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                        uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes, bool carveOrphanMft) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    const uint64_t volumeStartSector = partitionOffsetBytes / sectorSize;

    uint32_t sectorsPerCluster = 8;
    uint64_t mftStartCluster = 0;
    uint32_t mftRecordBytes = 1024;
    std::vector<uint8_t> bootSector(sectorSize);
    if (reader.readSectors(partitionOffsetBytes, sectorSize, bootSector.data()).success) {
        uint32_t bootBps = sectorSize;
        uint32_t recBytes = 1024;
        uint64_t lcn = 0;
        uint32_t spc = 8;
        if (ntfs::parseNtfsBoot(bootSector.data(), bootSector.size(), bootBps, spc, lcn, recBytes)) {
            sectorsPerCluster = spc;
            mftStartCluster = lcn;
            mftRecordBytes = recBytes;
        } else {
            sectorsPerCluster = bootSector[0x0D];
            if (sectorsPerCluster == 0) sectorsPerCluster = 8;
        }
    }

    uint64_t scanEndSector = diskSize / sectorSize;
    if (partitionSizeBytes > 0) {
        scanEndSector = std::min(scanEndSector, volumeStartSector + partitionSizeBytes / sectorSize);
    }

    struct ScanRange { uint64_t startSector; uint64_t sectorCount; };
    struct MftByteRange { uint64_t startByte; uint64_t countBytes; };
    std::vector<ScanRange> ranges;
    std::vector<MftByteRange> mftByteRanges;
    if (mftStartCluster > 0) {
        std::vector<uint8_t> rec0(mftRecordBytes, 0);
        uint64_t rec0Off = partitionOffsetBytes + mftStartCluster * static_cast<uint64_t>(sectorsPerCluster) * sectorSize;
        if (reader.readSectors(rec0Off, mftRecordBytes, rec0.data()).success &&
            std::strncmp(reinterpret_cast<char*>(rec0.data()), "FILE", 4) == 0) {
            auto runs = unnamedDataRunsFromRecord(rec0.data(), mftRecordBytes, volumeStartSector,
                                                  sectorsPerCluster);
            for (const auto& r : runs) {
                if (r.startSector != UINT64_MAX && r.sectorCount > 0) {
                    ranges.push_back({r.startSector, r.sectorCount});
                    mftByteRanges.push_back({r.startSector * sectorSize, r.sectorCount * sectorSize});
                }
            }
        }
        if (ranges.empty()) {
            uint64_t mftSec = volumeStartSector + mftStartCluster * sectorsPerCluster;
            uint64_t secCount = (1024ull * mftRecordBytes + sectorSize - 1) / sectorSize;
            ranges.push_back({mftSec, secCount});
            mftByteRanges.push_back({mftSec * sectorSize, mftRecordBytes * 1024});
        }
    }

    std::vector<ScanRange> orphanRanges;
    if (carveOrphanMft) {
        uint64_t full = scanEndSector > volumeStartSector ? scanEndSector - volumeStartSector : 0;
        if (full > 0) orphanRanges.push_back({volumeStartSector, full});
    }

    // We will scan in 4MB chunks for MFT records (RAW MFT Carving)
    const uint32_t chunkSectors = (4 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();

    // FAZ 1.1 (B1/B2): Pass-1 builds ONLY the compact MftIndex. The previous
    // design pushed a full TempFile (FileRecord + resident $DATA blob) per
    // record here and accumulated gigabytes on multi-million-record volumes.
    std::unordered_map<uint64_t, MftEntry> mftEntries;
    ntfs::MftIndex mftIndex;
    // Data runs of $Extend\$UsnJrnl:$J captured while carving (see the USN
    // post-pass). Populated only when the journal attribute survives.
    std::vector<FileRecord::DataRun> usnJournalRuns;
    std::vector<std::vector<uint8_t>> usnJournalInline;

    auto mftRecFromAbsByte = [&](uint64_t absByte) -> uint64_t {
        uint64_t acc = 0;
        for (const auto& br : mftByteRanges) {
            if (absByte >= br.startByte && absByte < br.startByte + br.countBytes)
                return (acc + (absByte - br.startByte)) / mftRecordBytes;
            acc += br.countBytes;
        }
        return UINT64_MAX;
    };

    std::vector<ntfs::IndexNameHint> i30Hints;
    uint64_t mftBytes = 0;
    for (const auto& br : mftByteRanges) mftBytes += br.countBytes;
    const uint64_t estimatedMftRecords = std::max<uint64_t>(1,
        mftRecordBytes > 0 ? mftBytes / mftRecordBytes : 1);
    uint64_t recordsProcessed = 0;
    // Mirrors the legacy foundCount sequence (main record + its ADS children),
    // so fallback "UnknownFile_N.bin" names fed into the path index keep their
    // pre-refactor values.
    uint64_t pass1NameSeq = 0;

    struct ScanPass { std::vector<ScanRange> ranges; bool orphan; };
    std::vector<ScanPass> passes;
    if (!ranges.empty()) passes.push_back({ranges, false});
    if (!orphanRanges.empty()) passes.push_back({orphanRanges, true});
    if (passes.empty()) return true;

    for (const auto& pass : passes) {
    for (const auto& rg : pass.ranges) {
    if (rg.sectorCount == 0) continue;
    const uint64_t rangeEnd = rg.startSector + rg.sectorCount;
    for (uint64_t sector = rg.startSector; sector < rangeEnd; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;

        uint64_t remain = rangeEnd - sector;
        uint32_t thisSectors = static_cast<uint32_t>(std::min<uint64_t>(chunkSectors, remain));
        uint32_t thisSize = thisSectors * sectorSize;
        auto res = reader.readSectors(sector * sectorSize, thisSize, currentBuf->data());
        if (!res.success) continue;

        const uint32_t step = pass.orphan ? sectorSize : mftRecordBytes;
        for (uint32_t i = 0; i + 1024 <= res.bytesRead; i += step) {
            MFT_RecordHeader* header = reinterpret_cast<MFT_RecordHeader*>(currentBuf->data() + i);

            // Look for "FILE" signature
            if (std::strncmp(header->signature, "FILE", 4) == 0 && header->firstAttributeOffset > 0 && header->firstAttributeOffset < 1024) {

                uint32_t recordSize = header->allocatedSize;
                if (recordSize < 1024 || recordSize > 4096 || i + recordSize > res.bytesRead) {
                    recordSize = 1024; // Fallback for corrupt headers
                }

                // Work on a private copy: the USA fixup mutates the record and
                // the scan buffer is shared/read-only.
                std::vector<uint8_t> recordBuf(currentBuf->data() + i,
                                               currentBuf->data() + i + recordSize);

                ParsedMftRecord parsed;
                parsed.filename = "UnknownFile_" + std::to_string(pass1NameSeq) + ".bin";
                parseMftRecord(recordBuf.data(), recordSize, sectorSize,
                               volumeStartSector, sectorsPerCluster, parsed);
                const bool isDirectory = parsed.flags & ntfs::RECORD_FLAG_DIRECTORY;
                const bool inUse = parsed.flags & ntfs::RECORD_FLAG_IN_USE;

                // ponytail: resident $INDEX_ROOT only. Slack names stay off the
                // MFT map so reuse cannot overwrite the live FILE name. Full
                // $INDEX_ALLOCATION recarve later.
                i30Hints.insert(i30Hints.end(), parsed.i30Hints.begin(), parsed.i30Hints.end());

                // $UsnJrnl's journal data lives in an ADS named ":$J". Capture
                // its runs so the post-pass can parse USN records into
                // timeline events. Captured exactly once (Pass-1 only).
                for (const auto& ads : parsed.adsEntries) {
                    if (ads.streamName == "$J") {
                        if (!ads.runs.empty()) {
                            usnJournalRuns.insert(usnJournalRuns.end(),
                                                  ads.runs.begin(), ads.runs.end());
                        } else if (!ads.residentData.empty()) {
                            usnJournalInline.push_back(ads.residentData);
                        }
                    }
                }

                const uint64_t absByte = sector * sectorSize + i;
                // CA-032: resolve the MFT record number in BOTH passes. The
                // orphan pass re-scans the live MFT zone, so every record the
                // main pass indexed must be skipped here or it would emit a
                // second time.
                const uint64_t mftRec = mftRecFromAbsByte(absByte);
                if (pass.orphan && mftRec != UINT64_MAX && mftEntries.count(mftRec)) continue;

                if (mftRec != UINT64_MAX)
                    mftIndex.putFileRecord(mftRec, parsed.parentMft, parsed.filename,
                                           inUse, isDirectory);

                const uint64_t key = (mftRec != UINT64_MAX)
                    ? mftRec
                    : (absByte | kOrphanEntryKeyBit);
                if (mftEntries.count(key)) continue; // duplicate sighting: first wins

                MftEntry entry;
                entry.byteOffset = absByte;
                entry.byteLen = recordSize;
                entry.mftRef = mftRec;
                entry.flags = parsed.flags;
                const std::string packed = serializeRuns(parsed.dataRuns);
                entry.packedRuns.assign(packed.begin(), packed.end());
                mftEntries.emplace(key, std::move(entry));

                pass1NameSeq += 1 + parsed.adsEntries.size();

                if (!pass.orphan) {
                    ++recordsProcessed;
                    FileRecord progressTick{};
                    progressTick.id = -1;
                    progressTick.startSector = recordsProcessed;
                    progressTick.sizeBytes = estimatedMftRecords;
                    callback(progressTick);
                }
            }
        }
    }
    }
    }

    // Post-processing: LogFile hints + USN deleted refs are collected BEFORE
    // Pass-2 emits, so every streamed record gets the same boosts the old
    // buffered loop applied.
    NtfsLogHintCollector logHints;
    scanNtfsLogFileHints(reader, partitionOffsetBytes,
                         [&](const FileRecord& fr) {
                             if (fr.source == "ntfs_logfile_restart") callback(fr);
                         },
                         isRunning, &logHints);

    std::unordered_set<uint64_t> usnDeletedRefs;
    if (!usnJournalRuns.empty() || !usnJournalInline.empty()) {
        const uint32_t kUsnChunk = 1u << 20;
        std::vector<uint8_t> buf(kUsnChunk);
        for (const auto& run : usnJournalRuns) {
            if (isRunning && !(*isRunning)) break;
            if (run.startSector == UINT64_MAX) continue;
            uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
            uint64_t pos = 0;
            while (pos < runBytes) {
                if (isRunning && !(*isRunning)) break;
                uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(kUsnChunk, runBytes - pos));
                if (!reader.readSectors(run.startSector * sectorSize + pos, take, buf.data()).success) {
                    pos += take;
                    continue;
                }
                ntfs::harvestUsnDeletedMftRefs(buf.data(), take, usnDeletedRefs);
                pos += take;
            }
        }
        for (const auto& blob : usnJournalInline) {
            if (isRunning && !(*isRunning)) break;
            ntfs::harvestUsnDeletedMftRefs(blob.data(), blob.size(), usnDeletedRefs);
        }
    }

    // ------------------------------------------------------------------
    // FAZ 1.1 (C1-C5): Pass-2 streaming emit. Walk the compact index in a
    // deterministic (key/ino-sorted) order, re-read each record's bytes, and
    // stream the assembled FileRecord to the callback with the exact
    // pre-refactor emit semantics (path, confidence, status, runs,
    // startSector). Memory holds ONE record at a time — never the volume.
    // ------------------------------------------------------------------
    std::vector<uint64_t> emitOrder;
    emitOrder.reserve(mftEntries.size());
    for (const auto& kv : mftEntries) emitOrder.push_back(kv.first);
    std::sort(emitOrder.begin(), emitOrder.end());

    // C2: entries handled by Pass-2 (or intentionally routed to the recycle
    // flush) are marked here; the K1 orphan sweep afterwards emits anything
    // the Pass-2 walk could not re-read, so no indexed record is ever lost.
    // Indexed by position in emitOrder — a bit per entry instead of a node
    // per entry (2M records: ~250 KB instead of ~80 MB of set nodes).
    std::vector<bool> visited(emitOrder.size(), false);

    int64_t emitId = 0;              // legacy foundCount continuation
    int64_t thumbcacheId = 900000;
    constexpr int64_t kOrphanSweepIdBase = 600000; // distinct from i30(800k)/thumb(900k)
    int64_t orphanSweepCount = 0;

    struct RecycleCandidate {
        FileRecord fr;
        size_t orderIndex = 0;
        std::vector<FileRecord> adsChildren;
    };
    std::vector<RecycleCandidate> recycleCandidates;
    auto isRecycleName = [](const std::string& name) {
        char kind = 0;
        std::string id;
        return ntfs::recyclePairKey(name, kind, id);
    };

    std::vector<uint8_t> recBuf; // reused single-record read buffer

    // Per-record post-processing, verbatim from the pre-refactor tempFiles
    // loop: path rebuild, $LogFile boost, USN delete boost.
    auto applyRecordBoosts = [&](FileRecord& fr, uint64_t mftRef) {
        fr.path = mftIndex.rebuildPath(mftRef, fr.name, static_cast<uint64_t>(fr.parentId));
        uint64_t logMft = UINT64_MAX;
        std::string logName;
        const bool byName = logHints.findByName(fr.name, &logMft);
        const bool byRef = !byName && mftRef != UINT64_MAX &&
                           logHints.findByMftRef(mftRef, &logName);
        if (byName || byRef) {
            fr.confidence = std::min(100, fr.confidence + 12);
            if (fr.source == "ntfs_mft") fr.source = "ntfs_mft_logfile";
            if (byRef && fr.name.empty()) fr.name = logName;
            if (logMft != UINT64_MAX && mftRef == UINT64_MAX) mftRef = logMft;
        }
        if (mftRef != UINT64_MAX && fr.status == 0 &&
            usnDeletedRefs.count(mftRef)) {
            ntfs::applyUsnMftDeleteBoost(fr, mftRef, usnDeletedRefs);
        }
    };

    auto emitWithThumbcache = [&](const FileRecord& fr) {
        callback(fr);
        if (isRunning && !(*isRunning)) return;
        if (fr.status == 0) {
            ntfs::emitThumbcacheThumbnails(reader, fr, thumbcacheId, callback, isRunning);
        }
    };

    for (size_t visitIdx = 0; visitIdx < emitOrder.size(); ++visitIdx) {
        if (isRunning && !(*isRunning)) break; // C5: cancel at every block boundary
        const MftEntry& entry = mftEntries[emitOrder[visitIdx]];

        recBuf.resize(entry.byteLen);
        // readBytes: record offsets can sit inside a sector (1024-byte records
        // on 4096-byte-sector media); the aligned backend read + slice keeps
        // every backend contract intact.
        const auto res = reader.readBytes(entry.byteOffset, entry.byteLen, recBuf.data());
        if (!res.success || res.bytesRead < entry.byteLen) continue; // K1 sweep covers it

        ParsedMftRecord parsed;
        parsed.filename = "UnknownFile_" + std::to_string(emitId) + ".bin";
        parseMftRecord(recBuf.data(), entry.byteLen, sectorSize,
                       volumeStartSector, sectorsPerCluster, parsed);

        // We found a valid file record — assemble the FileRecord with the
        // pre-refactor field semantics.
        FileRecord fr;
        fr.id = emitId++;
        fr.parentId = static_cast<int64_t>(parsed.parentMft);
        fr.name = parsed.filename;

        size_t dotPos = fr.name.find_last_of('.');
        fr.extension = (dotPos != std::string::npos) ? fr.name.substr(dotPos + 1) : "";
        fr.path = "/Recovered/" + fr.name; // Temp path, fixed by applyRecordBoosts
        fr.sizeBytes = parsed.fileSize;
        fr.startSector = parsed.dataRuns.empty()
            ? entry.byteOffset / sectorSize
            : parsed.finalStartSector;
        fr.endSector = parsed.dataRuns.empty()
            ? fr.startSector + (entry.byteLen / sectorSize)
            : parsed.finalEndSector;
        fr.runs = parsed.dataRuns;
        fr.status = (parsed.flags & ntfs::RECORD_FLAG_IN_USE) ? 1 : 0; // 1 = Allocated, 0 = Deleted
        const bool isDirectory = parsed.flags & ntfs::RECORD_FLAG_DIRECTORY;
        fr.confidence = ntfs::scoreMftConfidence(
            fr.status != 0, parsed.usaOk, isDirectory, fr.sizeBytes, !parsed.dataRuns.empty(),
            parsed.mainStreamResident);
        // Directories are reported as a distinct category so the UI can
        // render a folder tree; they carry no recoverable $DATA payload.
        fr.category = isDirectory ? "Folder" : categoryForName(fr.name);
        fr.source = "ntfs_mft";
        fr.compressed = parsed.dataCompressed;
        fr.residentData = parsed.residentBytes;
        fr.createdAt = parsed.createdAt;
        fr.modifiedAt = parsed.modifiedAt;

        // Emit one record per Alternate Data Stream so each named stream is
        // independently recoverable. Reuses the parent record's metadata and
        // overrides name/size/runs. Resident (tiny) ADS streams have no data
        // runs and are reported with the MFT record's own sector. Built from
        // the PRE-boost parent (legacy semantics: the ADS confidence base is
        // the raw score minus 5, then its own boosts apply).
        std::vector<FileRecord> adsChildren;
        for (const auto& ads : parsed.adsEntries) {
            FileRecord adsFr = fr;
            adsFr.id = emitId++;
            adsFr.name = parsed.filename + ":" + (ads.streamName.empty() ? "stream" : ads.streamName);
            adsFr.extension = ads.streamName;
            adsFr.sizeBytes = ads.size;
            adsFr.runs = ads.runs;
            adsFr.residentData = ads.residentData;
            adsFr.startSector = ads.runs.empty()
                ? (entry.byteOffset / sectorSize)
                : ads.startSector;
            adsFr.endSector = adsFr.startSector;
            adsFr.source = "ntfs_ads";
            adsFr.confidence = std::max(0, fr.confidence - 5);
            applyRecordBoosts(adsFr, entry.mftRef);
            adsChildren.push_back(std::move(adsFr));
        }

        applyRecordBoosts(fr, entry.mftRef);

        // $I/$R records pair ACROSS records (Recycle Bin). They are buffered
        // and resolved after the walk — the set is tiny next to the volume.
        if (isRecycleName(fr.name)) {
            RecycleCandidate rc;
            rc.fr = std::move(fr);
            rc.orderIndex = visitIdx;
            rc.adsChildren = std::move(adsChildren);
            recycleCandidates.push_back(std::move(rc));
            visited[visitIdx] = true; // handled by the flush (or suppressed as $I-meta)
            continue;
        }

        emitWithThumbcache(fr);
        visited[visitIdx] = true;
        for (const auto& adsFr : adsChildren) {
            if (isRunning && !(*isRunning)) break;
            if (isRecycleName(adsFr.name)) {
                RecycleCandidate rc;
                rc.fr = adsFr;
                rc.orderIndex = visitIdx;
                recycleCandidates.push_back(std::move(rc));
                continue;
            }
            emitWithThumbcache(adsFr);
        }
    }

    // Recycle flush: pair $I metadata with $R content, then emit. $I rows that
    // paired are suppressed (source ntfs_recycle_meta) — the pre-refactor
    // emission loop skipped them identically.
    ntfs::applyRecycleBinRecords(recycleCandidates);
    for (auto& rc : recycleCandidates) {
        if (isRunning && !(*isRunning)) break;
        if (rc.fr.source == "ntfs_recycle_meta") continue;
        emitWithThumbcache(rc.fr);
        for (const auto& adsFr : rc.adsChildren) {
            if (isRunning && !(*isRunning)) break;
            emitWithThumbcache(adsFr);
        }
    }
    recycleCandidates.clear();

    // ------------------------------------------------------------------
    // K1 orphan sweep (C3): every MftIndex entry Pass-2 could not handle
    // (unreadable sector, failed re-read) is emitted here as a nameless
    // orphan record. This is the structural no-loss guarantee: emitted
    // records + suppressed recycle-meta == indexed entries. Deterministic
    // order (ino-sorted, same walk as Pass-2).
    // ------------------------------------------------------------------
    for (size_t visitIdx = 0; visitIdx < emitOrder.size(); ++visitIdx) {
        if (isRunning && !(*isRunning)) break;
        if (visited[visitIdx]) continue;
        const MftEntry& entry = mftEntries[emitOrder[visitIdx]];
        const bool inUse = entry.flags & ntfs::RECORD_FLAG_IN_USE;
        const bool isDirectory = entry.flags & ntfs::RECORD_FLAG_DIRECTORY;
        FileRecord fr;
        fr.id = kOrphanSweepIdBase + (orphanSweepCount++);
        fr.parentId = -1;
        fr.name = "";     // the directory walk never produced a name
        fr.path = "/";
        fr.sizeBytes = 0;
        fr.startSector = entry.byteOffset / sectorSize;
        fr.endSector = fr.startSector;
        fr.status = inUse ? 1 : 0;
        fr.confidence = ntfs::scoreMftConfidence(inUse, /*usaOk=*/false, isDirectory,
                                                 /*sizeBytes=*/0, /*hasDataRuns=*/false,
                                                 /*hasResidentPayload=*/false);
        fr.category = "Other";
        fr.source = "orphan";
        callback(fr);
    }

    // ponytail: resident $INDEX_ROOT only. Slack names stay off the MFT map so
    // reuse cannot overwrite the live FILE name. Full $INDEX_ALLOCATION recarve later.
    std::unordered_set<std::string> i30Seen;
    int64_t i30Id = 800000;
    for (const auto& h : i30Hints) {
        if (h.name.empty() || h.childMft == 0) continue;
        const bool unseen = !mftIndex.hasRecord(h.childMft);
        const bool reuse = !unseen && mftIndex.recordName(h.childMft) != h.name;
        if (!unseen && !reuse) continue;
        const std::string key = std::to_string(h.childMft) + "\n" + h.name;
        if (!i30Seen.insert(key).second) continue;
        FileRecord fr{};
        fr.id = i30Id++;
        fr.parentId = static_cast<int64_t>(h.parentMft);
        fr.name = h.name;
        fr.path = mftIndex.rebuildPath(h.childMft, h.name, h.parentMft);
        fr.status = 0;
        fr.source = "ntfs_i30";
        fr.confidence = 35;
        fr.category = categoryForName(h.name);
        callback(fr);
    }

    // ---- USN journal pass ----
    // Read the captured $UsnJrnl:$J runs and parse every v2/v3 record into a
    // timeline event (source="usn_journal"). The reason flags travel in
    // fr.status-encoded form: the store decodes CREATE/DELETE/RENAME for the
    // timeline UI. Records are validated structurally by parseUsnRecord, so
    // slack garbage at the tail of the journal is skipped rather than
    // poisoning the timeline.
    for (const auto& run : usnJournalRuns) {
        if (isRunning && !(*isRunning)) break;
        if (run.startSector == UINT64_MAX) continue; // sparse run

        const uint32_t kUsnChunk = 1u << 20; // 1 MiB
        std::vector<uint8_t> buf(kUsnChunk);
        uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
        uint64_t pos = 0;

        while (pos < runBytes) {
            if (isRunning && !(*isRunning)) break;
            uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(kUsnChunk, runBytes - pos));
            if (!reader.readSectors(run.startSector * sectorSize + pos, take, buf.data()).success) {
                pos += take; // bad sector in the journal: skip forward
                continue;
            }

            size_t off = 0;
            while (off + 64 <= take) {
                ntfs::UsnRecord usn;
                if (!ntfs::parseUsnRecord(buf.data() + off, take - off, usn)) {
                    // Records are 8-byte aligned; step forward on a bad slot.
                    off += 8;
                    continue;
                }
                uint32_t recLen = static_cast<uint32_t>(buf[off]) |
                                  (static_cast<uint32_t>(buf[off + 1]) << 8) |
                                  (static_cast<uint32_t>(buf[off + 2]) << 16) |
                                  (static_cast<uint32_t>(buf[off + 3]) << 24);
                if (recLen < 64) break; // paranoia: parser guarantees >= 60

                FileRecord fr;
                fr.id = -1;                       // not a scannable file
                fr.parentId = static_cast<int64_t>(usn.fileReference & 0x0000FFFFFFFFFFFFULL);
                fr.name = usn.name;
                fr.extension = "";
                fr.path = "usn";                  // timeline marker
                fr.sizeBytes = 0;
                fr.startSector = (run.startSector * sectorSize + pos + off) / sectorSize;
                fr.endSector = fr.startSector;
                fr.status = static_cast<int>(usn.reasonFlags); // reason bits
                fr.confidence = 90;
                fr.category = "Timeline";
                fr.source = "usn_journal";
                fr.createdAt = usn.timestamp;
                fr.modifiedAt = usn.timestamp;
                callback(fr);

                off += recLen;
            }
            pos += take;
        }
    }

    return true;
}

} // namespace byteback
