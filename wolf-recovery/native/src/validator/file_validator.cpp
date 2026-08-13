#include "wolf_validator.h"
#include "math/entropy_calculator.h"

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
    if (header.empty()) return 10;
    int baseScore = 30;

    // Mathematical Entropy Analysis for scientific validation (single source of truth)
    double entropy = wolf::math::calculateEntropy(header.data(), header.size());

    // High entropy = likely encrypted/compressed, Low entropy = text/zeroed out
    if (entropy < 0.5) return 0; // Empty or near-empty sector (junk)
    if (entropy >= 3.0 && entropy <= 7.8) baseScore += 20; // Healthy structured data range

    // Forensic Signature Matching
    if (header.size() > 8) {
        if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
            // JPEG Signature
            if (record.extension == "jpg" || record.extension == "jpeg") baseScore += 40;
            else baseScore += 20; // Found signature but mismatching extension

            // Footer check for EOI (End of Image)
            if (!footer.empty()) {
                for (size_t i = 0; i < footer.size() - 1; ++i) {
                    if (footer[i] == 0xFF && footer[i+1] == 0xD9) {
                        baseScore += 10;
                        break;
                    }
                }
            }
        } 
        else if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
            // PNG Signature
            if (record.extension == "png") baseScore += 50;
            else baseScore += 25;
        }
        else if (header[0] == 0x25 && header[1] == 0x50 && header[2] == 0x44 && header[3] == 0x46) {
            // PDF Signature (%PDF)
            if (record.extension == "pdf") baseScore += 50;
            else baseScore += 25;
            
            // Footer check for %%EOF
            if (!footer.empty() && footer.size() > 5) {
                for (size_t i = 0; i < footer.size() - 5; ++i) {
                    if (footer[i] == '%' && footer[i+1] == '%' && footer[i+2] == 'E' && footer[i+3] == 'O' && footer[i+4] == 'F') {
                        baseScore += 10;
                        break;
                    }
                }
            }
        }
        else if (header[0] == 0x50 && header[1] == 0x4B && header[2] == 0x03 && header[3] == 0x04) {
            // ZIP Signature
            if (record.extension == "zip" || record.extension == "docx" || record.extension == "xlsx") baseScore += 40;
            else baseScore += 20;
        }
        else if (header[0] == 'R' && header[1] == 'a' && header[2] == 'r' && header[3] == '!') {
            // RAR Signature
            if (record.extension == "rar") baseScore += 40;
            else baseScore += 20;
        }
    }

    if (baseScore > 100) baseScore = 100;
    return baseScore;
}

} // namespace wolf
