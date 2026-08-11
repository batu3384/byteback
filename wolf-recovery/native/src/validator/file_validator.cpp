#include "wolf_validator.h"

namespace wolf {

FileValidator::FileValidator() {}
FileValidator::~FileValidator() {}

void FileValidator::validateFile(FileRecord& record, DiskReader& reader) {
    if (record.sizeBytes == 0 || !reader.isOpen()) {
        record.confidence = 0;
        return;
    }

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    std::vector<uint8_t> header(sectorSize);
    std::vector<uint8_t> footer(sectorSize);

    // Read header (first sector)
    auto resHeader = reader.readSectors(record.startSector * sectorSize, sectorSize, header.data());
    
    // Read footer (last sector)
    uint64_t lastSector = record.endSector > 0 ? record.endSector - 1 : 0;
    auto resFooter = reader.readSectors(lastSector * sectorSize, sectorSize, footer.data());

    if (!resHeader.success) {
        record.confidence = 10;
        return;
    }

    record.confidence = calculateConfidence(record, header, resFooter.success ? footer : std::vector<uint8_t>());
}

int FileValidator::calculateConfidence(const FileRecord& record, const std::vector<uint8_t>& header, const std::vector<uint8_t>& footer) {
    int baseScore = 50;

    // Very naive mock validation
    if (record.extension == "jpg" || record.extension == "jpeg") {
        if (header.size() > 2 && header[0] == 0xFF && header[1] == 0xD8) baseScore += 25;
        if (footer.size() > 2) {
            // Find FFD9 in the last sector
            for (size_t i = 0; i < footer.size() - 1; ++i) {
                if (footer[i] == 0xFF && footer[i+1] == 0xD9) {
                    baseScore += 25;
                    break;
                }
            }
        }
    } else if (record.extension == "png") {
        if (header.size() > 8 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
            baseScore += 50;
        }
    } else {
        // Unknown format, default base score
        baseScore += 20;
    }

    if (baseScore > 100) baseScore = 100;
    return baseScore;
}

} // namespace wolf
