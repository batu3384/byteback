#include "wolf_carver.h"
#include "wolf_memory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <future>
#include <algorithm>

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
    // We embed the signatures directly for maximum reliability.
    signatures.clear();

    auto addSig = [&](const std::string& fmt, const std::string& ext, const std::string& cat, 
                      const std::vector<uint8_t>& head, const std::vector<uint8_t>& foot, uint64_t maxS) {
        FileSignature s;
        s.format = fmt; s.extension = ext; s.category = cat;
        s.header = head; s.footer = foot; s.maxSize = maxS;
        signatures.push_back(s);
    };

    // Images
    addSig("JPEG Image", ".jpg", "Image", {0xFF, 0xD8, 0xFF}, {0xFF, 0xD9}, 15 * 1024 * 1024);
    addSig("PNG Image", ".png", "Image", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, 30 * 1024 * 1024);
    addSig("GIF Image", ".gif", "Image", {0x47, 0x49, 0x46, 0x38}, {0x00, 0x3B}, 10 * 1024 * 1024);
    addSig("BMP Image", ".bmp", "Image", {0x42, 0x4D}, {}, 50 * 1024 * 1024);

    // Documents
    addSig("PDF Document", ".pdf", "Document", {0x25, 0x50, 0x44, 0x46, 0x2D}, {0x25, 0x25, 0x45, 0x4F, 0x46}, 100 * 1024 * 1024);
    addSig("RTF Document", ".rtf", "Document", {0x7B, 0x5C, 0x72, 0x74, 0x66, 0x31}, {0x7D}, 20 * 1024 * 1024);
    addSig("DOCX/XLSX/ZIP", ".zip", "Archive", {0x50, 0x4B, 0x03, 0x04}, {0x50, 0x4B, 0x05, 0x06}, 500 * 1024 * 1024);

    // Audio/Video
    addSig("MP4 Video", ".mp4", "Video", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MP4 Video (Alt)", ".mp4", "Video", {0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("AVI Video", ".avi", "Video", {0x52, 0x49, 0x46, 0x46}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MKV Video", ".mkv", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MP3 Audio", ".mp3", "Audio", {0x49, 0x44, 0x33}, {}, 20 * 1024 * 1024);
    addSig("FLAC Audio", ".flac", "Audio", {0x66, 0x4C, 0x61, 0x43}, {}, 100 * 1024 * 1024);
    addSig("WAV Audio", ".wav", "Audio", {0x52, 0x49, 0x46, 0x46}, {}, 100 * 1024 * 1024);

    // Archives
    addSig("RAR Archive", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00}, {}, 500 * 1024 * 1024);
    addSig("RAR Archive v5", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00}, {}, 500 * 1024 * 1024);
    addSig("7-Zip Archive", ".7z", "Archive", {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C}, {}, 500 * 1024 * 1024);
    addSig("GZIP Archive", ".gz", "Archive", {0x1F, 0x8B, 0x08}, {}, 100 * 1024 * 1024);
    addSig("BZIP2 Archive", ".bz2", "Archive", {0x42, 0x5A, 0x68}, {}, 100 * 1024 * 1024);

    // Others
    addSig("Windows Executable", ".exe", "Executable", {0x4D, 0x5A}, {}, 100 * 1024 * 1024);
    addSig("ELF Executable", ".elf", "Executable", {0x7F, 0x45, 0x4C, 0x46}, {}, 100 * 1024 * 1024);
    addSig("SQLite Database", ".sqlite", "Database", {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("PST Email Data", ".pst", "Database", {0x21, 0x42, 0x44, 0x4E}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MPEG Video", ".mpg", "Video", {0x00, 0x00, 0x01, 0xBA}, {0x00, 0x00, 0x01, 0xB9}, 2ULL * 1024 * 1024 * 1024);

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
                    if (entropy < 1.0 && sig.category == "Archive") continue; 

                    uint64_t actualSize = sig.maxSize;
                    bool footerFound = false;

                    if (!sig.footer.empty()) {
                        // 1. Search in current chunk
                        if (i + sig.header.size() < res.bytesRead) {
                            uint32_t maxSearchJ = res.bytesRead >= sig.footer.size() ? res.bytesRead - (uint32_t)sig.footer.size() : 0;
                            for (uint32_t j = i + (uint32_t)sig.header.size(); j <= maxSearchJ; ++j) {
                                bool fMatch = true;
                                for (size_t f = 0; f < sig.footer.size(); ++f) {
                                    if (currentBuf->data()[j + f] != sig.footer[f]) {
                                        fMatch = false; break;
                                    }
                                }
                                if (fMatch) {
                                    actualSize = (j + sig.footer.size()) - i;
                                    footerFound = true;
                                    break;
                                }
                            }
                        }

                        // 2. Search ahead if not found
                        if (!footerFound) {
                            uint64_t maxSearchBytes = std::min(sig.maxSize, diskSize - (sector * sectorSize + i));
                            uint64_t bytesSearched = res.bytesRead - i;
                            uint64_t currentOffset = sector * sectorSize + res.bytesRead;

                            auto tempPoolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);
                            auto* tempBuf = tempPoolBuf.get();

                            while (bytesSearched < maxSearchBytes) {
                                if (isRunning && !(*isRunning)) break;

                                uint32_t readSize = (uint32_t)std::min((uint64_t)chunkSize, maxSearchBytes - bytesSearched);
                                readSize = ((readSize + sectorSize - 1) / sectorSize) * sectorSize; // Align to sector
                                if (readSize == 0 || currentOffset + readSize > diskSize) break;

                                auto tempRes = reader.readSectors(currentOffset, readSize, tempBuf->data());
                                if (!tempRes.success || tempRes.bytesRead == 0) break;

                                uint32_t maxSearchJ = tempRes.bytesRead >= sig.footer.size() ? tempRes.bytesRead - (uint32_t)sig.footer.size() : 0;
                                for (uint32_t j = 0; j <= maxSearchJ; ++j) {
                                    bool fMatch = true;
                                    for (size_t f = 0; f < sig.footer.size(); ++f) {
                                        if (tempBuf->data()[j + f] != sig.footer[f]) {
                                            fMatch = false; break;
                                        }
                                    }
                                    if (fMatch) {
                                        actualSize = bytesSearched + j + sig.footer.size();
                                        footerFound = true;
                                        break;
                                    }
                                }

                                if (footerFound) break;

                                bytesSearched += tempRes.bytesRead;
                                currentOffset += tempRes.bytesRead;
                            }
                        }
                    }

                    FileRecord fr;
                    fr.id = 0;
                    fr.parentId = 0;
                    uint64_t startSec = sector + (i / sectorSize);
                    fr.name = "carved_" + std::to_string(foundCount++) + "_" + std::to_string(startSec) + sig.extension;
                    fr.extension = sig.extension.empty() ? "" : sig.extension.substr(1);
                    fr.path = "/recovered_raw/" + fr.name;
                    fr.sizeBytes = actualSize; 
                    fr.startSector = startSec;
                    uint64_t endSectorsOff = (actualSize + sectorSize - 1) / sectorSize;
                    fr.endSector = fr.startSector + endSectorsOff;
                    fr.status = 0;
                    fr.confidence = footerFound ? 95 : 70;
                    fr.category = sig.category;
                    fr.source = "carver";
                    fr.createdAt = 0;
                    fr.modifiedAt = 0;
                    
                    callback(fr);
                }
            }
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }

    return true;
}
} // namespace wolf
