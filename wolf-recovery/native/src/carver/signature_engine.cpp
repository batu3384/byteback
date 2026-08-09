#include "wolf_carver.h"
#include "wolf_memory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

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

bool CarvingEngine::loadSignatures(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) return false;
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Extremely naive JSON parsing for this specific format
    size_t pos = 0;
    while ((pos = content.find("\"format\":", pos)) != std::string::npos) {
        FileSignature sig;
        
        auto extractString = [&](const std::string& key) -> std::string {
            size_t kPos = content.find("\"" + key + "\":", pos);
            if (kPos == std::string::npos) return "";
            size_t start = content.find("\"", kPos + key.length() + 3) + 1;
            size_t end = content.find("\"", start);
            return content.substr(start, end - start);
        };
        
        auto extractNum = [&](const std::string& key) -> uint64_t {
            size_t kPos = content.find("\"" + key + "\":", pos);
            if (kPos == std::string::npos) return 0;
            size_t start = kPos + key.length() + 3;
            while (content[start] == ' ' || content[start] == ':') start++;
            size_t end = start;
            while (content[end] >= '0' && content[end] <= '9') end++;
            std::string numStr = content.substr(start, end - start);
            if (numStr.empty()) return 0;
            return std::stoull(numStr);
        };

        sig.format = extractString("format");
        sig.extension = extractString("extension");
        sig.category = extractString("category");
        std::string headerHex = extractString("header");
        std::string footerHex = extractString("footer");
        sig.header = hexToBytes(headerHex);
        sig.footer = hexToBytes(footerHex);
        sig.maxSize = extractNum("max_size");
        
        signatures.push_back(sig);
        pos += 10;
    }
    
    return !signatures.empty();
}

bool CarvingEngine::scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback) {
    if (!reader.isOpen() || signatures.empty()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = 512; 
    
    const uint32_t chunkSectors = 2048; // 1MB chunks
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto* poolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);
        auto& buffer = *poolBuf;
    
    // Very limited maxSector for quick mock test
    uint64_t maxSector = std::min(diskSize / sectorSize, (uint64_t)1000000); 

    int foundCount = 0;

    for (uint64_t sector = 0; sector < maxSector; sector += chunkSectors) {
        auto res = reader.readSectors(sector * sectorSize, chunkSize, buffer.data());
        if (!res.success) continue;

        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            for (const auto& sig : signatures) {
                if (sig.header.empty() || i + sig.header.size() > res.bytesRead) continue;
                
                bool match = true;
                for (size_t h = 0; h < sig.header.size(); ++h) {
                    if (buffer[i + h] != sig.header[h]) {
                        match = false;
                        break;
                    }
                }
                
                if (match) {
                    double entropy = EntropyAnalyzer::calculateShannonEntropy(buffer, i, std::min((uint32_t)4096, res.bytesRead - i));
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
    }

    return true;
}

} // namespace wolf


