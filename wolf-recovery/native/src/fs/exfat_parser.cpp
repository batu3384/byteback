#include "wolf_fs.h"
#include <iostream>

namespace wolf {

ExFATParser::ExFATParser() {}
ExFATParser::~ExFATParser() {}

bool ExFATParser::scan(DiskReader& reader, FileRecordCallback callback) {
    if (!reader.isOpen()) return false;
    
    // Very simplified mock for exFAT parsing
    uint32_t sectorSize = 512;
    std::vector<uint8_t> buffer(sectorSize);
    
    // Read boot sector
    auto res = reader.readSectors(0, sectorSize, buffer.data());
    if (!res.success) return false;
    
    // exFAT signature check (OEM Name 'EXFAT   ')
    std::string oemName(buffer.begin() + 3, buffer.begin() + 11);
    if (oemName != "EXFAT   ") {
        return false; // Not exFAT
    }

    // Mock finding a file via exFAT directory entries
    FileRecord fr;
    fr.id = 2001;
    fr.parentId = 0; 
    fr.name = "exfat_recovered.mp4";
    fr.extension = "mp4";
    fr.path = "/videos/";
    fr.sizeBytes = 150420000;
    fr.startSector = 65000;
    fr.endSector = 65000 + (150420000 / sectorSize) + 1;
    fr.status = 1; // deleted but recoverable
    fr.confidence = 85;
    fr.category = "Video";
    fr.source = "exfat_dir";
    fr.createdAt = 1710000000;
    fr.modifiedAt = 1710000000;
    
    callback(fr);

    return true;
}

} // namespace wolf
