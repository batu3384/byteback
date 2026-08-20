#pragma once

#include "byteback_fs.h"
#include <cstdint>
#include <string>

namespace byteback {

struct BitLockerFveInfo {
    bool detected = false;
    uint64_t metadataOffset = 0;
    uint32_t encryptionMethod = 0;
    std::string encryptionName;
    std::string volumeGuidHex;
};

std::string bitLockerMethodName(uint32_t method);
bool parseBitLockerFve(const uint8_t* boot, size_t bootLen,
                       const uint8_t* metadata, size_t metaLen,
                       BitLockerFveInfo& out);

void emitBitLockerRecord(const BitLockerFveInfo& info, uint64_t startSector,
                         FileSystemParser::FileRecordCallback onFileFound);

} // namespace byteback
