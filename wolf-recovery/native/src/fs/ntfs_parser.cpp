#include "wolf_fs.h"
#include "wolf_memory.h"
#include "fs/ntfs_util.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <algorithm>

namespace wolf {

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

struct INDX_Header {
    char signature[4]; // "INDX"
    uint16_t updateSequenceOffset;
    uint16_t updateSequenceSize;
    uint64_t logFileSequenceNumber;
    uint64_t vcn;
    uint32_t indexEntryOffset; // Relative to 0x18
    uint32_t indexEntriesSize;
    uint32_t allocatedSize;
    uint8_t nonLeafNode;
    uint8_t padding[3];
};

struct INDX_Entry {
    uint64_t fileReference;
    uint16_t entryLength;
    uint16_t streamLength;
    uint16_t flags; // 0x01 = sub-node, 0x02 = last entry
    uint16_t padding;
    // stream data follows (typically NTFS_FileNameAttribute)
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
} // namespace

bool NTFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    uint32_t sectorsPerCluster = 8;
    std::vector<uint8_t> bootSector(sectorSize);
    if (reader.readSectors(0, sectorSize, bootSector.data()).success) {
        sectorsPerCluster = bootSector[0x0D];
        if (sectorsPerCluster == 0) sectorsPerCluster = 8;
    }
    
    // We will scan in 4MB chunks for MFT records (RAW MFT Carving)
    const uint32_t chunkSectors = (4 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();
    
    uint64_t maxSector = diskSize / sectorSize;
    int foundCount = 0;

    struct TempFile {
        FileRecord fr;
        uint64_t parentId;
    };
    std::vector<TempFile> tempFiles;
    std::unordered_map<uint64_t, uint64_t> indxParentMap;
    std::unordered_map<uint64_t, std::string> indxNameMap;

    for (uint64_t sector = 0; sector < maxSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        
        auto res = reader.readSectors(sector * sectorSize, chunkSize, currentBuf->data());
        if (!res.success) continue;

        // Iterate through the buffer in sector-sized steps (MFT records are sector-aligned)
        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            // MFT records are typically 1024 bytes, but can be 4096. We must ensure we have at least 1024 bytes to read the header safely.
            if (i + 1024 > res.bytesRead) break;
            
            MFT_RecordHeader* header = reinterpret_cast<MFT_RecordHeader*>(currentBuf->data() + i);
            
            // Look for "FILE" signature
            if (std::strncmp(header->signature, "FILE", 4) == 0 && header->firstAttributeOffset > 0 && header->firstAttributeOffset < 1024) {
                
                uint32_t recordSize = header->allocatedSize;
                if (recordSize < 1024 || recordSize > 4096 || i + recordSize > res.bytesRead) {
                    recordSize = 1024; // Fallback for corrupt headers
                }

                // Distinguish directories from regular files. We no longer skip
                // directories: reporting them is essential for reconstructing the
                // folder tree, and a deleted directory record may be the only
                // surviving trace of files that lived under it.
                bool isDirectory = (header->flags & ntfs::RECORD_FLAG_DIRECTORY) != 0;

                // Apply the NTFS Update Sequence Array (USA) fixup before any
                // attribute parsing: NTFS overwrites the last two bytes of each
                // sector in a multi-sector record with a sequence number and
                // stores the originals in the USA. We work on a private copy
                // because the scan buffer is shared/read-only. A failed fixup
                // (partial overwrite / stale slack) does not skip the record —
                // we parse on a best-effort basis and lower confidence instead.
                std::vector<uint8_t> recordBuf(currentBuf->data() + i,
                                               currentBuf->data() + i + recordSize);
                bool usaOk = ntfs::applyUsaFixup(recordBuf.data(), recordSize, sectorSize,
                                                 header->updateSequenceOffset,
                                                 header->updateSequenceSize);
                // Repoint the working header into the fixed-up copy.
                header = reinterpret_cast<MFT_RecordHeader*>(recordBuf.data());
                uint8_t* recBase = recordBuf.data();

                std::string filename = "UnknownFile_" + std::to_string(foundCount) + ".bin";
                uint64_t fileSize = header->usedSize;
                uint64_t fileParentMftId = 0;
                std::vector<FileRecord::DataRun> dataRuns;
                uint64_t finalStartSector = 0;
                uint64_t finalEndSector = 0;
                int64_t createdAt = 0;
                int64_t modifiedAt = 0;

                // Alternate Data Streams (ADS): a record may carry several
                // $DATA attributes; only the unnamed one is the file's main
                // content. Named $DATA streams (e.g. Zone.Identifier) are
                // collected here and reported as separate recoverable records
                // using the "filename:streamname" convention.
                struct AdsEntry {
                    std::string streamName;
                    uint64_t size;
                    std::vector<FileRecord::DataRun> runs;
                    uint64_t startSector;
                };
                std::vector<AdsEntry> adsEntries;

                // Parse Attributes
                uint32_t attrOffset = header->firstAttributeOffset;
                bool nameFound = false;

                while (attrOffset + sizeof(NTFS_AttributeHeader) <= recordSize) {
                    NTFS_AttributeHeader* attr = reinterpret_cast<NTFS_AttributeHeader*>(recBase + attrOffset);
                    if (attr->type == ntfs::ATTR_END_MARKER || attr->length == 0) break; // End of attributes or corrupt

                    // $FILE_NAME attribute (0x30). A record may carry multiple
                    // (POSIX / Win32 long / DOS 8.3). Prefer the long name; fall
                    // back to DOS only if nothing else was captured.
                    if (attr->type == ntfs::ATTR_FILE_NAME && attr->nonResidentFlag == 0) {
                        if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                            NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(recBase + attrOffset + sizeof(NTFS_AttributeHeader));

                            size_t nameStructOffset = attrOffset + resAttr->valueOffset;
                            if (nameStructOffset + offsetof(NTFS_FileNameAttribute, name) <= recordSize) {
                                NTFS_FileNameAttribute* fnAttr = reinterpret_cast<NTFS_FileNameAttribute*>(recBase + nameStructOffset);

                                size_t available = recordSize - nameStructOffset;
                                if (fnAttr->nameLength > 0 && fnAttr->nameLength < 255) {
                                    std::string decoded = decodeNtfsName(fnAttr, available);
                                    if (!decoded.empty()) {
                                        // Prefer a long (Win32) name over DOS 8.3
                                        // unless we have no name yet.
                                        uint8_t nt = fnAttr->nameType;
                                        bool prefer = (nt == ntfs::NAME_TYPE_POSIX || nt == ntfs::NAME_TYPE_LONG || !nameFound);
                                        if (prefer) {
                                            filename = decoded;
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
                            NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(recBase + attrOffset + sizeof(NTFS_AttributeHeader));
                            size_t siOff = attrOffset + resAttr->valueOffset;
                            // SI is at least 48 bytes (creation, modified, mft-change, access).
                            if (siOff + 4 * sizeof(uint64_t) <= recordSize) {
                                const uint64_t* times = reinterpret_cast<const uint64_t*>(recBase + siOff);
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
                                const uint16_t* nameUnits = reinterpret_cast<const uint16_t*>(recBase + namePos);
                                adsName = sanitizeUtf8Name(ntfs::utf16leToUtf8(nameUnits, attr->nameLength));
                            }
                        }

                        if (attr->nonResidentFlag == 0) {
                            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                                NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(recBase + attrOffset + sizeof(NTFS_AttributeHeader));
                                if (isAds) {
                                    adsEntries.push_back({adsName, resAttr->valueLength, {}, 0});
                                } else {
                                    fileSize = resAttr->valueLength;
                                    dataRuns.clear();
                                }
                            }
                        } else {
                            if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_NonResidentHeader) <= recordSize) {
                                NTFS_NonResidentHeader* nonResAttr = reinterpret_cast<NTFS_NonResidentHeader*>(recBase + attrOffset + sizeof(NTFS_AttributeHeader));

                                // Parse the data runs into a local list so an
                                // ADS does not clobber the main stream's runs.
                                uint16_t runOffset = nonResAttr->dataRunOffset;
                                size_t currentRunPos = attrOffset + runOffset;
                                int64_t previousLcn = 0;
                                std::vector<FileRecord::DataRun> localRuns;
                                uint64_t localStart = UINT64_MAX;

                                while (currentRunPos < attrOffset + attr->length && currentRunPos < recordSize) {
                                    uint8_t headerByte = (uint8_t)recBase[currentRunPos];
                                    if (headerByte == 0x00) break;

                                    uint8_t lenSize = headerByte & 0x0F;
                                    uint8_t offSize = (headerByte >> 4) & 0x0F;
                                    currentRunPos++;

                                    if (currentRunPos + lenSize + offSize > recordSize) break;

                                    uint64_t clusterCount = 0;
                                    for (int j = 0; j < lenSize; j++) {
                                        clusterCount |= (uint64_t)((uint8_t)recBase[currentRunPos + j]) << (j * 8);
                                    }
                                    currentRunPos += lenSize;

                                    bool sparse = (offSize == 0);
                                    int64_t lcnOffset = 0;
                                    if (!sparse) {
                                        for (int j = 0; j < offSize; j++) {
                                            lcnOffset |= (uint64_t)((uint8_t)recBase[currentRunPos + j]) << (j * 8);
                                        }
                                        if ((uint8_t)recBase[currentRunPos + offSize - 1] & 0x80) {
                                            for (int j = offSize; j < 8; j++) {
                                                lcnOffset |= (uint64_t)0xFF << (j * 8);
                                            }
                                        }
                                        previousLcn += lcnOffset;
                                    }
                                    currentRunPos += offSize;

                                    FileRecord::DataRun run;
                                    run.startSector = sparse ? UINT64_MAX : previousLcn * sectorsPerCluster;
                                    run.sectorCount = clusterCount * sectorsPerCluster;
                                    localRuns.push_back(run);
                                    if (!sparse && localStart == UINT64_MAX) {
                                        localStart = run.startSector;
                                    }
                                }

                                if (isAds) {
                                    adsEntries.push_back({adsName, nonResAttr->realSize, localRuns, localStart});
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

                    if (attr->length < sizeof(NTFS_AttributeHeader)) break; // Corrupt attribute, prevent infinite loop
                    attrOffset += attr->length;
                }
                
                // We found a valid file record
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = fileParentMftId;
                fr.name = filename;
                
                size_t dotPos = filename.find_last_of('.');
                fr.extension = (dotPos != std::string::npos) ? filename.substr(dotPos + 1) : "";
                fr.path = "/Recovered/" + filename; // Temp path, will be fixed later
                fr.sizeBytes = fileSize;
                fr.startSector = dataRuns.empty() ? sector + (i / sectorSize) : finalStartSector;
                fr.endSector = dataRuns.empty() ? fr.startSector + (recordSize / sectorSize) : finalEndSector;
                fr.runs = dataRuns;
                fr.status = (header->flags & ntfs::RECORD_FLAG_IN_USE) ? 1 : 0; // 1 = Allocated, 0 = Deleted
                // Start from the allocated/deleted baseline, then dock records
                // whose USA fixup failed (partially overwritten / stale slack):
                // their attributes may be partially corrupt.
                fr.confidence = fr.status ? 100 : 80;
                if (!usaOk) fr.confidence = std::max(0, fr.confidence - 25);
                // Directories are reported as a distinct category so the UI can
                // render a folder tree; they carry no recoverable $DATA payload.
                fr.category = isDirectory ? "Folder" : "Document";
                fr.source = "ntfs_mft";
                fr.createdAt = createdAt;
                fr.modifiedAt = modifiedAt;
                
                TempFile tf;
                tf.fr = fr;
                tf.parentId = fileParentMftId;
                tempFiles.push_back(tf);

                // Emit one record per Alternate Data Stream so each named
                // stream is independently recoverable. We reuse the parent
                // record's metadata and override name/size/runs. Resident
                // (tiny) ADS streams have no data runs and are reported with
                // the MFT record's own sector so a caller can at least locate
                // the attribute inline.
                for (const auto& ads : adsEntries) {
                    FileRecord adsFr = fr;
                    adsFr.id = foundCount++;
                    adsFr.name = filename + ":" + (ads.streamName.empty() ? "stream" : ads.streamName);
                    adsFr.extension = ads.streamName;
                    adsFr.sizeBytes = ads.size;
                    adsFr.runs = ads.runs;
                    adsFr.startSector = ads.runs.empty()
                        ? (sector + (i / sectorSize))
                        : ads.startSector;
                    adsFr.endSector = adsFr.startSector;
                    adsFr.source = "ntfs_ads";
                    adsFr.confidence = std::max(0, fr.confidence - 5);

                    TempFile adsTf;
                    adsTf.fr = adsFr;
                    adsTf.parentId = fileParentMftId;
                    tempFiles.push_back(adsTf);
                }
            }
            // Look for "INDX" signature
            else if (std::strncmp(header->signature, "INDX", 4) == 0 && i + sizeof(INDX_Header) <= res.bytesRead) {
                INDX_Header* indxHdr = reinterpret_cast<INDX_Header*>(currentBuf->data() + i);
                uint32_t entriesOffset = 0x18 + indxHdr->indexEntryOffset;
                uint32_t entriesSize = indxHdr->indexEntriesSize;
                
                uint32_t recordSize = 4096; // typical INDX buffer size
                if (i + recordSize > res.bytesRead) recordSize = res.bytesRead - i;
                
                if (entriesOffset < recordSize && entriesOffset + entriesSize <= recordSize) {
                    // Walk the live index entries first (entriesSize window),
                    // then keep scanning into the INDX slack — NTFS does not
                    // zero the tail after a delete, so deleted directory
                    // entries often survive there. We validate each candidate
                    // (plausible lengths, name in bounds) and skip rather than
                    // stop on a bad one so one corrupt slot cannot hide the
                    // remaining slack. The scan ends at the buffer boundary.
                    uint32_t offset = entriesOffset;
                    uint32_t scanLimit = recordSize;
                    while (offset + sizeof(INDX_Entry) <= scanLimit) {
                        INDX_Entry* entry = reinterpret_cast<INDX_Entry*>(currentBuf->data() + i + offset);

                        // A zeroed or nonsensical slot in the slack is skipped,
                        // not treated as end-of-index, so trailing slack is
                        // still examined.
                        if (entry->entryLength < 16) {
                            offset += 8; // advance by the minimum entry stride
                            continue;
                        }
                        if (offset + entry->entryLength > scanLimit) {
                            offset += 8;
                            continue;
                        }

                        if (entry->streamLength >= sizeof(NTFS_FileNameAttribute) && offset + 16 + sizeof(NTFS_FileNameAttribute) <= scanLimit) {
                            NTFS_FileNameAttribute* fnAttr = reinterpret_cast<NTFS_FileNameAttribute*>(currentBuf->data() + i + offset + 16);

                            uint64_t childMftId = entry->fileReference & 0x0000FFFFFFFFFFFFULL;
                            uint64_t parentMftId = fnAttr->parentDirectory & 0x0000FFFFFFFFFFFFULL;

                            size_t nameStructOffset = offset + 16;
                            size_t totalNameBytes = fnAttr->nameLength * 2;

                            if (fnAttr->nameLength > 0 && fnAttr->nameLength < 255 && nameStructOffset + offsetof(NTFS_FileNameAttribute, name) + totalNameBytes <= offset + entry->entryLength) {
                                size_t available = (offset + entry->entryLength) - nameStructOffset;
                                std::string decoded = decodeNtfsName(fnAttr, available);
                                if (!decoded.empty()) {
                                    indxParentMap[childMftId] = parentMftId;
                                    indxNameMap[childMftId] = decoded;
                                }
                            }
                        }
                        offset += entry->entryLength;
                    }
                }
            }
        }
        
        // Report progress at the end of each chunk
        FileRecord progressTick;
        progressTick.id = -1; // Special ID for progress tick
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }
    
    // Post-processing: reconstruct full paths
    for (auto& tf : tempFiles) {
        uint64_t currParent = tf.parentId;
        std::string fullPath = tf.fr.name;
        
        int depth = 0;
        while (currParent != 5 && currParent != 0 && indxNameMap.count(currParent) && depth < 100) {
            fullPath = indxNameMap[currParent] + "/" + fullPath;
            currParent = indxParentMap[currParent];
            depth++;
        }
        
        tf.fr.path = "/Recovered/" + fullPath;
        callback(tf.fr);
    }
    
    return true;
}

} // namespace wolf
