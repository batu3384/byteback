#include "wolf_fs.h"
#include "wolf_memory.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <future>

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
#pragma pack(pop)

bool NTFSParser::scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    // We will scan in 4MB chunks for MFT records (RAW MFT Carving)
    const uint32_t chunkSectors = (4 * 1024 * 1024) / sectorSize;
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();
    
    uint64_t maxSector = diskSize / sectorSize;
    int foundCount = 0;

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

                // MFT Record is in use and is a file (not directory for now)
                bool isDirectory = (header->flags & 0x02) != 0;
                if (isDirectory) continue;
                
                std::string filename = "UnknownFile_" + std::to_string(foundCount) + ".bin";
                uint64_t fileSize = header->usedSize;
                
                // Parse Attributes
                uint32_t attrOffset = header->firstAttributeOffset;
                bool nameFound = false;

                while (attrOffset + sizeof(NTFS_AttributeHeader) <= recordSize) {
                    NTFS_AttributeHeader* attr = reinterpret_cast<NTFS_AttributeHeader*>(currentBuf->data() + i + attrOffset);
                    if (attr->type == 0xFFFFFFFF || attr->length == 0) break; // End of attributes or corrupt
                    
                    // FILE_NAME attribute is 0x30
                    if (attr->type == 0x30 && attr->nonResidentFlag == 0) {
                        if (attrOffset + sizeof(NTFS_AttributeHeader) + sizeof(NTFS_ResidentAttributeHeader) <= recordSize) {
                            NTFS_ResidentAttributeHeader* resAttr = reinterpret_cast<NTFS_ResidentAttributeHeader*>(currentBuf->data() + i + attrOffset + sizeof(NTFS_AttributeHeader));
                            
                            // Prevent Buffer Overread (CRITICAL FIX)
                            size_t nameStructOffset = attrOffset + resAttr->valueOffset;
                            if (nameStructOffset + offsetof(NTFS_FileNameAttribute, name) <= recordSize) {
                                NTFS_FileNameAttribute* fnAttr = reinterpret_cast<NTFS_FileNameAttribute*>(currentBuf->data() + i + nameStructOffset);
                                
                                size_t totalNameBytes = fnAttr->nameLength * 2;
                                if (fnAttr->nameLength > 0 && fnAttr->nameLength < 255 && nameStructOffset + offsetof(NTFS_FileNameAttribute, name) + totalNameBytes <= recordSize) {
                                    std::string extractedName = "";
                                    for (int n = 0; n < fnAttr->nameLength; n++) {
                                        uint16_t c = fnAttr->name[n];
                                        // Simple ASCII fallback and Sanitize (XSS / Traversal Fix)
                                        if (c >= 32 && c < 127 && c != '/' && c != '\\' && c != '<' && c != '>') {
                                            extractedName += (char)c;
                                        } else {
                                            extractedName += '_';
                                        }
                                    }
                                    if (!extractedName.empty()) {
                                        filename = extractedName;
                                        fileSize = fnAttr->realSize;
                                        nameFound = true;
                                    }
                                }
                            }
                        }
                    }
                    
                    if (nameFound) break; // We got the name, can break early for simple carving
                    if (attr->length < sizeof(NTFS_AttributeHeader)) break; // Corrupt attribute, prevent infinite loop
                    attrOffset += attr->length;
                }
                
                // We found a valid file record
                FileRecord fr;
                fr.id = foundCount++;
                fr.parentId = 0;
                fr.name = filename;
                
                size_t dotPos = filename.find_last_of('.');
                fr.extension = (dotPos != std::string::npos) ? filename.substr(dotPos + 1) : "";
                fr.path = "/recovered_ntfs/" + filename;
                fr.sizeBytes = fileSize;
                fr.startSector = sector + (i / sectorSize);
                fr.endSector = fr.startSector + (recordSize / sectorSize);
                fr.status = (header->flags & 0x01) ? 1 : 0; // 1 = Allocated, 0 = Deleted
                fr.confidence = fr.status ? 100 : 80;
                fr.category = "Document"; // Fallback category
                fr.source = "ntfs_mft";
                fr.createdAt = 0;
                fr.modifiedAt = 0;
                
                callback(fr);
            }
        }
        
        // Report progress at the end of each chunk
        FileRecord progressTick;
        progressTick.id = -1; // Special ID for progress tick
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }
    
    return true;
}

} // namespace wolf

