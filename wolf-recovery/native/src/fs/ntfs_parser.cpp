#include "wolf_fs.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

namespace wolf {

NTFSParser::NTFSParser() {}
NTFSParser::~NTFSParser() {}

#pragma pack(push, 1)
struct NTFS_BootSector {
    uint8_t jmp[3];
    char oemID[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint8_t reserved[7];
    uint8_t mediaDescriptor;
    uint16_t zero;
    uint16_t sectorsPerTrack;
    uint16_t heads;
    uint32_t hiddenSectors;
    uint32_t notUsed;
    uint32_t notUsed2;
    uint64_t totalSectors;
    uint64_t mftCluster;
    uint64_t mftMirrCluster;
    int8_t clustersPerMftRecord;
    uint8_t notUsed3[3];
    int8_t clustersPerIndexBuffer;
    uint8_t notUsed4[3];
    uint64_t serialNumber;
    uint32_t checksum;
};

struct MFT_RecordHeader {
    char signature[4]; // "FILE"
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
#pragma pack(pop)

bool NTFSParser::scan(DiskReader& reader, FileRecordCallback callback) {
    uint64_t sectorSize = reader.getSectorSize();
    std::vector<uint8_t> buffer(sectorSize);

    if (!reader.readSectors(0, 1, buffer.data()).success) return false;

    NTFS_BootSector* boot = reinterpret_cast<NTFS_BootSector*>(buffer.data());
    if (std::strncmp(boot->oemID, "NTFS    ", 8) != 0) return false;

    uint64_t mftSector = boot->mftCluster * boot->sectorsPerCluster;
    uint32_t recordSize = boot->clustersPerMftRecord > 0 ? 
        (boot->clustersPerMftRecord * boot->sectorsPerCluster * sectorSize) :
        (1 << (-boot->clustersPerMftRecord));

    uint32_t sectorsPerRecord = recordSize / sectorSize;
    if (sectorsPerRecord == 0) sectorsPerRecord = 1;

    std::vector<uint8_t> mftBuffer(recordSize);

    for (int i = 0; i < 100; i++) {
        uint64_t currentSector = mftSector + (i * sectorsPerRecord);
        if (reader.readSectors(currentSector, sectorsPerRecord, mftBuffer.data()).success) {
            MFT_RecordHeader* header = reinterpret_cast<MFT_RecordHeader*>(mftBuffer.data());
            
            if (std::strncmp(header->signature, "FILE", 4) == 0 && (header->flags & 0x0001)) {
                FileRecord fr;
                fr.id = i;
                fr.name = "recovered_file_" + std::to_string(i) + ".bin";
                fr.path = "/";
                fr.sizeBytes = header->usedSize;
                
                FileRecord::DataRun run;
                run.startSector = currentSector;
                run.sectorCount = sectorsPerRecord;
                fr.runs.push_back(run);

                fr.status = 0;
                fr.confidence = 90;
                callback(fr);
            }
        }
    }
    return true;
}

} // namespace wolf
