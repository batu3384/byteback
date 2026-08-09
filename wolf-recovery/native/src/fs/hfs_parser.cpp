#include "wolf_fs.h"
#include <iostream>

namespace wolf {

HFSParser::HFSParser() {}
HFSParser::~HFSParser() {}

bool HFSParser::scan(DiskReader& reader, FileRecordCallback callback) {
    if (!reader.isOpen()) return false;
    
    uint32_t sectorSize = 512;
    std::vector<uint8_t> buffer(2048);
    
    // Read Volume Header (offset 1024 bytes)
    auto res = reader.readSectors(1024 / sectorSize, 2048, buffer.data());
    if (!res.success) return false;
    
    // HFS+ signature check: 'H+' (0x48 0x2B) or 'HX' (0x48 0x58) at offset 0
    if (buffer[0] != 0x48 || (buffer[1] != 0x2B && buffer[1] != 0x58)) {
        return false; // Not HFS+
    }

    // Mock finding a file via HFS+ catalog
    FileRecord fr;
    fr.id = 3001;
    fr.parentId = 1; 
    fr.name = "hfs_recovered.dmg";
    fr.extension = "dmg";
    fr.path = "/Downloads/";
    fr.sizeBytes = 256000000;
    fr.startSector = 85000;
    fr.endSector = 85000 + (256000000 / sectorSize) + 1;
    fr.status = 0; 
    fr.confidence = 95;
    fr.category = "Archive";
    fr.source = "hfs_catalog";
    fr.createdAt = 1720000000;
    fr.modifiedAt = 1720000000;
    
    callback(fr);

    return true;
}

} // namespace wolf
