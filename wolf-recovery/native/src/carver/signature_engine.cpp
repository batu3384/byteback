#include "wolf_carver.h"
#include "wolf_memory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <future>

namespace wolf {

CarvingEngine::CarvingEngine() {}
CarvingEngine::~CarvingEngine() {}

std::vector<uint8_t> CarvingEngine::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

bool CarvingEngine::loadSignatures(const std::string& /*jsonPath*/) {
    // Instead of loading from JSON which can fail or be missing in prod,
    // we embed the signatures directly for maximum reliability.
    signatures.clear();

    // JPEG
    FileSignature jpg;
    jpg.format = "JPEG Image";
    jpg.extension = ".jpg";
    jpg.category = "Image";
    jpg.header = {0xFF, 0xD8, 0xFF};
    // footer often {0xFF, 0xD9} but can vary, omitting for simplicity
    jpg.maxSize = 10 * 1024 * 1024; // 10MB
    signatures.push_back(jpg);

    // PNG
    FileSignature png;
    png.format = "PNG Image";
    png.extension = ".png";
    png.category = "Image";
    png.header = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.maxSize = 20 * 1024 * 1024; // 20MB
    signatures.push_back(png);

    // PDF
    FileSignature pdf;
    pdf.format = "PDF Document";
    pdf.extension = ".pdf";
    pdf.category = "Document";
    pdf.header = {0x25, 0x50, 0x44, 0x46, 0x2D}; // %PDF-
    pdf.maxSize = 50 * 1024 * 1024; // 50MB
    signatures.push_back(pdf);

    // ZIP (including DOCX, XLSX, etc.)
    FileSignature zip;
    zip.format = "ZIP Archive";
    zip.extension = ".zip";
    zip.category = "Archive";
    zip.header = {0x50, 0x4B, 0x03, 0x04};
    zip.maxSize = 500 * 1024 * 1024; // 500MB
    signatures.push_back(zip);

    return true;
}

bool CarvingEngine::scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen() || signatures.empty()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    const uint32_t chunkSectors = 8192; // 4MB chunks
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();
    
    uint64_t maxSector = diskSize / sectorSize;

    int foundCount = 0;

    for (uint64_t sector = 0; sector < maxSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        
        auto res = reader.readSectors(sector * sectorSize, chunkSize, currentBuf->data());
        if (!res.success) continue;

        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            for (const auto& sig : signatures) {
                if (sig.header.empty() || i + sig.header.size() > res.bytesRead) continue;
                
                bool match = true;
                for (size_t h = 0; h < sig.header.size(); ++h) {
                    if (currentBuf->data()[i + h] != sig.header[h]) {
                        match = false;
                        break;
                    }
                }
                
                if (match) {
                    double entropy = EntropyAnalyzer::calculateShannonEntropy(currentBuf->data(), res.bytesRead, i, std::min<uint32_t>((uint32_t)4096, (uint32_t)(res.bytesRead - i)));
                    if (entropy < 1.0 && sig.category == "Archive") continue; // Zip files shouldn't have zero entropy
                    FileRecord fr;
                    fr.id = 0;
                    fr.parentId = 0;
                    fr.name = "carved_file_" + std::to_string(foundCount++) + sig.extension;
                    fr.extension = sig.extension.empty() ? "" : sig.extension.substr(1);
                    fr.path = "/recovered_raw/" + fr.name;
                    fr.sizeBytes = sig.maxSize; // Unknown actual size until footer is found
                    fr.startSector = sector + (i / sectorSize);
                    fr.endSector = fr.startSector + (sig.maxSize / sectorSize);
                    fr.status = 0;
                    fr.confidence = 70;
                    fr.category = sig.category;
                    fr.source = "carver";
                    fr.createdAt = 0;
                    fr.modifiedAt = 0;
                    
                    callback(fr);
                }
            }
        }
        
        // std::swap removed
        
        // Emit progress tick at the end of each chunk
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }

    return true;
}

} // namespace wolf





