#include "wolf_fs.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace wolf {

#pragma pack(push, 1)
struct NTFS_VBR {
    uint8_t jump[3];
    char oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t zeros[3];
    uint16_t not_used1;
    uint8_t media_descriptor;
    uint16_t zeros2;
    uint16_t sectors_per_track;
    uint16_t number_of_heads;
    uint32_t hidden_sectors;
    uint32_t not_used2;
    uint32_t not_used3;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirr_lcn;
    int8_t clusters_per_mft_record;
    uint8_t not_used4[3];
    int8_t clusters_per_index_buffer;
    uint8_t not_used5[3];
    uint64_t volume_serial_number;
    uint32_t checksum;
};

struct MFT_HEADER {
    char signature[4]; // "FILE"
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t sequence_number;
    uint16_t link_count;
    uint16_t attr_offset;
    uint16_t flags; // 1 = in use, 2 = directory
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_mft_record;
    uint16_t next_attr_instance;
    uint16_t reserved;
    uint32_t mft_record_number;
};

struct ATTR_HEADER {
    uint32_t type; 
    uint32_t length; 
    uint8_t non_resident;
    uint8_t name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t id;
};

struct ATTR_RESIDENT {
    ATTR_HEADER header;
    uint32_t attr_length;
    uint16_t attr_offset;
    uint8_t indexed_flag;
    uint8_t padding;
};

struct ATTR_NON_RESIDENT {
    ATTR_HEADER header;
    uint64_t start_vcn;
    uint64_t last_vcn;
    uint16_t data_runs_offset;
    uint16_t compression_unit;
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t real_size;
    uint64_t initialized_size;
};

struct FILE_NAME_ATTR {
    uint64_t parent_directory;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_change_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse_value;
    uint8_t name_length;
    uint8_t namespace_type;
};
#pragma pack(pop)

struct DataRun {
    uint64_t lengthClusters;
    int64_t offsetClusters;
    uint64_t lcn; 
};

static std::string utf16le_to_utf8(const uint16_t* utf16_str, size_t len) {
    std::string utf8_str;
    for (size_t i = 0; i < len; ++i) {
        uint16_t wc = utf16_str[i];
        if (wc < 0x80) {
            utf8_str.push_back(static_cast<char>(wc));
        } else if (wc < 0x800) {
            utf8_str.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            utf8_str.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            utf8_str.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            utf8_str.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            utf8_str.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    return utf8_str;
}

static std::vector<DataRun> parseDataRuns(const uint8_t* runData, size_t maxSize) {
    std::vector<DataRun> runs;
    size_t pos = 0;
    int64_t currentLcn = 0;
    while (pos < maxSize) {
        uint8_t header = runData[pos++];
        if (header == 0) break;

        uint8_t lenSize = header & 0x0F;
        uint8_t offsetSize = (header >> 4) & 0x0F;

        if (pos + lenSize + offsetSize > maxSize) break;

        uint64_t length = 0;
        for (int i = 0; i < lenSize; ++i) {
            length |= (static_cast<uint64_t>(runData[pos++]) << (i * 8));
        }

        int64_t offset = 0;
        for (int i = 0; i < offsetSize; ++i) {
            offset |= (static_cast<int64_t>(runData[pos++]) << (i * 8));
        }
        
        if (offsetSize > 0 && (runData[pos - 1] & 0x80)) {
            for (int i = offsetSize; i < 8; ++i) {
                offset |= (static_cast<int64_t>(0xFF) << (i * 8));
            }
        }

        currentLcn += offset;
        runs.push_back({length, offset, static_cast<uint64_t>(currentLcn)});
    }
    return runs;
}

static void parseMFTRecord(const uint8_t* recordData, uint32_t recordSize, uint64_t sectorOffset, FileSystemParser::FileRecordCallback callback) {
    const MFT_HEADER* header = reinterpret_cast<const MFT_HEADER*>(recordData);
    if (std::strncmp(header->signature, "FILE", 4) != 0) return;
    if (!(header->flags & 1)) return; // Not in use
    if (header->base_mft_record != 0) return; // Only process base records

    FileRecord fr;
    fr.id = header->mft_record_number;
    fr.source = "mft";
    fr.status = 0;
    fr.confidence = 100;
    fr.startSector = sectorOffset;
    fr.endSector = sectorOffset + (recordSize / 512);
    fr.category = (header->flags & 2) ? "Directory" : "File";
    fr.sizeBytes = 0;
    fr.createdAt = 0;
    fr.modifiedAt = 0;
    fr.parentId = 0;
    
    uint32_t attrOff = header->attr_offset;
    while (attrOff + sizeof(ATTR_HEADER) <= header->bytes_in_use && attrOff < recordSize) {
        const ATTR_HEADER* attrHeader = reinterpret_cast<const ATTR_HEADER*>(recordData + attrOff);
        if (attrHeader->type == 0xFFFFFFFF) break; 
        if (attrHeader->length == 0 || attrOff + attrHeader->length > recordSize) break; 

        if (attrHeader->type == 0x10) { // STANDARD_INFORMATION
            if (!attrHeader->non_resident && attrOff + sizeof(ATTR_RESIDENT) <= recordSize) {
                const ATTR_RESIDENT* res = reinterpret_cast<const ATTR_RESIDENT*>(attrHeader);
                if (attrOff + res->attr_offset + 32 <= recordSize) { 
                    const uint64_t* timestamps = reinterpret_cast<const uint64_t*>(recordData + attrOff + res->attr_offset);
                    auto filetime_to_unix = [](uint64_t ft) -> int64_t {
                        if (ft < 116444736000000000ULL) return 0;
                        return (ft - 116444736000000000ULL) / 10000000;
                    };
                    fr.createdAt = filetime_to_unix(timestamps[0]);
                    fr.modifiedAt = filetime_to_unix(timestamps[1]);
                }
            }
        } else if (attrHeader->type == 0x30) { // FILE_NAME
            if (!attrHeader->non_resident && attrOff + sizeof(ATTR_RESIDENT) <= recordSize) {
                const ATTR_RESIDENT* res = reinterpret_cast<const ATTR_RESIDENT*>(attrHeader);
                if (attrOff + res->attr_offset + sizeof(FILE_NAME_ATTR) <= recordSize) {
                    const FILE_NAME_ATTR* fn = reinterpret_cast<const FILE_NAME_ATTR*>(recordData + attrOff + res->attr_offset);
                    
                    if (fr.name.empty() || fn->namespace_type == 1 || fn->namespace_type == 3) {
                        fr.parentId = fn->parent_directory & 0xFFFFFFFFFFFF;
                        if (fr.sizeBytes == 0) fr.sizeBytes = fn->real_size; 
                        
                        const uint16_t* nameStr = reinterpret_cast<const uint16_t*>(recordData + attrOff + res->attr_offset + sizeof(FILE_NAME_ATTR));
                        fr.name = utf16le_to_utf8(nameStr, fn->name_length);
                        
                        size_t extPos = fr.name.find_last_of('.');
                        if (extPos != std::string::npos && extPos + 1 < fr.name.length()) {
                            fr.extension = fr.name.substr(extPos + 1);
                        } else {
                            fr.extension = "";
                        }
                    }
                }
            }
        } else if (attrHeader->type == 0x80) { // DATA
            if (attrHeader->non_resident && attrOff + sizeof(ATTR_NON_RESIDENT) <= recordSize) {
                const ATTR_NON_RESIDENT* nonRes = reinterpret_cast<const ATTR_NON_RESIDENT*>(attrHeader);
                fr.sizeBytes = nonRes->real_size;
            } else if (!attrHeader->non_resident && attrOff + sizeof(ATTR_RESIDENT) <= recordSize) {
                const ATTR_RESIDENT* res = reinterpret_cast<const ATTR_RESIDENT*>(attrHeader);
                fr.sizeBytes = res->attr_length;
            }
        }

        attrOff += attrHeader->length;
    }

    if (!fr.name.empty()) {
        callback(fr);
    }
}

NTFSParser::NTFSParser() {}
NTFSParser::~NTFSParser() {}

bool NTFSParser::scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback) {
    if (!reader.isOpen()) return false;

    uint8_t bootSector[512];
    auto res = reader.readSectors(0, 512, bootSector);
    if (!res.success) return false;

    NTFS_VBR* vbr = reinterpret_cast<NTFS_VBR*>(bootSector);
    if (std::strncmp(vbr->oem_id, "NTFS    ", 8) != 0) {
        return false; 
    }

    uint32_t bytesPerCluster = vbr->bytes_per_sector * vbr->sectors_per_cluster;
    if (bytesPerCluster == 0) return false;

    uint32_t mftRecordSize = 1024;
    if (vbr->clusters_per_mft_record > 0) {
        mftRecordSize = vbr->clusters_per_mft_record * bytesPerCluster;
    } else {
        mftRecordSize = 1 << (-vbr->clusters_per_mft_record);
    }
    
    uint64_t mftOffset = vbr->mft_lcn * bytesPerCluster;
    uint32_t readSize = (mftRecordSize + 511) / 512 * 512;
    std::vector<uint8_t> mftRecordBuf(readSize);
    res = reader.readSectors(mftOffset, readSize, mftRecordBuf.data());
    if (!res.success) return false;

    const MFT_HEADER* mftHeader = reinterpret_cast<const MFT_HEADER*>(mftRecordBuf.data());
    if (std::strncmp(mftHeader->signature, "FILE", 4) != 0) return false;

    uint32_t attrOff = mftHeader->attr_offset;
    std::vector<DataRun> mftRuns;
    while (attrOff + sizeof(ATTR_HEADER) <= mftHeader->bytes_in_use && attrOff < mftRecordSize) {
        const ATTR_HEADER* attrHeader = reinterpret_cast<const ATTR_HEADER*>(mftRecordBuf.data() + attrOff);
        if (attrHeader->type == 0xFFFFFFFF) break;
        if (attrHeader->length == 0 || attrOff + attrHeader->length > mftRecordSize) break;

        if (attrHeader->type == 0x80 && attrHeader->non_resident) { 
            const ATTR_NON_RESIDENT* nonRes = reinterpret_cast<const ATTR_NON_RESIDENT*>(attrHeader);
            uint32_t runsOff = attrOff + nonRes->data_runs_offset;
            mftRuns = parseDataRuns(mftRecordBuf.data() + runsOff, attrHeader->length - nonRes->data_runs_offset);
            break; 
        }
        attrOff += attrHeader->length;
    }

    if (mftRuns.empty()) return false;

    std::vector<uint8_t> clusterBuf;
    for (const auto& run : mftRuns) {
        uint64_t runOffsetBytes = run.lcn * bytesPerCluster;
        uint64_t runSizeBytes = run.lengthClusters * bytesPerCluster;
        
        uint64_t chunkSize = 1024 * 1024; // 1MB chunks
        clusterBuf.resize(chunkSize);

        uint64_t bytesRead = 0;
        while (bytesRead < runSizeBytes) {
            uint64_t toRead = std::min(chunkSize, runSizeBytes - bytesRead);
            uint32_t sectorAlignedToRead = (static_cast<uint32_t>(toRead) + 511) / 512 * 512;
            
            auto runRes = reader.readSectors(runOffsetBytes + bytesRead, sectorAlignedToRead, clusterBuf.data());
            if (!runRes.success) break;

            for (uint32_t recOff = 0; recOff + mftRecordSize <= toRead; recOff += mftRecordSize) {
                uint64_t absoluteSector = (runOffsetBytes + bytesRead + recOff) / 512;
                parseMFTRecord(clusterBuf.data() + recOff, mftRecordSize, absoluteSector, callback);
            }
            bytesRead += toRead;
        }
    }
    
    return true;
}

} // namespace wolf
